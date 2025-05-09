/*
 * Copyright (c) 2017, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/g1/g1FullCollector.inline.hpp"
#include "gc/g1/g1FullGCCompactionPoint.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/shared/fullGCForwarding.inline.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/debug.hpp"

G1FullGCCompactionPoint::G1FullGCCompactionPoint(G1FullCollector* collector, PreservedMarks* preserved_stack) :
    _collector(collector),
    _current_region(nullptr),
    _compaction_top(nullptr),
    _preserved_stack(preserved_stack) {
  _compaction_regions = new (mtGC) GrowableArray<G1HeapRegion*>(32, mtGC);
  _compaction_region_iterator = _compaction_regions->begin();
}

G1FullGCCompactionPoint::~G1FullGCCompactionPoint() {
  delete _compaction_regions;
}

void G1FullGCCompactionPoint::update() {
  if (is_initialized()) {
    _collector->set_compaction_top(_current_region, _compaction_top);
  }
}

void G1FullGCCompactionPoint::initialize_values() {
  _compaction_top = _collector->compaction_top(_current_region);
}

bool G1FullGCCompactionPoint::has_regions() {
  return !_compaction_regions->is_empty();
}

bool G1FullGCCompactionPoint::is_initialized() {
  return _current_region != nullptr;
}

void G1FullGCCompactionPoint::initialize(G1HeapRegion* hr) {
  _current_region = hr;
  initialize_values();
}

G1HeapRegion* G1FullGCCompactionPoint::current_region() {
  return *_compaction_region_iterator;
}

G1HeapRegion* G1FullGCCompactionPoint::next_region() {
  G1HeapRegion* next = *(++_compaction_region_iterator);
  assert(next != nullptr, "Must return valid region");
  return next;
}

GrowableArray<G1HeapRegion*>* G1FullGCCompactionPoint::regions() {
  return _compaction_regions;
}

bool G1FullGCCompactionPoint::object_will_fit(size_t size) {
  size_t space_left = pointer_delta(_current_region->end(), _compaction_top);
  return size <= space_left;
}

void G1FullGCCompactionPoint::switch_region() {
  // Save compaction top in the region.
  _collector->set_compaction_top(_current_region, _compaction_top);
  // Get the next region and re-initialize the values.
  _current_region = next_region();
  initialize_values();
}

void G1FullGCCompactionPoint::forward(oop object, size_t size) {
  assert(_current_region != nullptr, "Must have been initialized");

  size_t old_size = size;
  size_t new_size = object->copy_size(old_size, object->mark());
  size = cast_from_oop<HeapWord*>(object) != _compaction_top ? new_size : old_size;

  // Ensure the object fit in the current region.
  while (!object_will_fit(size)) {
    switch_region();
    size = cast_from_oop<HeapWord*>(object) != _compaction_top ? new_size : old_size;
  }

  // Store a forwarding pointer if the object should be moved.
  if (cast_from_oop<HeapWord*>(object) != _compaction_top) {
    if (!object->is_forwarded()) {
      preserved_stack()->push_if_necessary(object, object->mark());
    }
    FullGCForwarding::forward_to(object, cast_to_oop(_compaction_top));
    assert(FullGCForwarding::is_forwarded(object), "must be forwarded");
  } else {
    assert(!FullGCForwarding::is_forwarded(object), "must not be forwarded");
  }

  // Update compaction values.
  _compaction_top += size;
  _current_region->update_bot_for_block(_compaction_top - size, _compaction_top);
}

void G1FullGCCompactionPoint::add(G1HeapRegion* hr) {
  _compaction_regions->append(hr);
}

void G1FullGCCompactionPoint::remove_at_or_above(uint bottom) {
  G1HeapRegion* cur = current_region();
  assert(cur->hrm_index() >= bottom, "Sanity!");

  int start_index = 0;
  for (G1HeapRegion* r : *_compaction_regions) {
    if (r->hrm_index() < bottom) {
      start_index++;
    }
  }

  assert(start_index >= 0, "Should have at least one region");
  _compaction_regions->trunc_to(start_index);
}

void G1FullGCCompactionPoint::add_humongous(G1HeapRegion* hr) {
  assert(hr->is_starts_humongous(), "Sanity!");

  _collector->add_humongous_region(hr);

  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  g1h->humongous_obj_regions_iterate(hr,
                                     [&] (G1HeapRegion* r) {
                                       add(r);
                                       _collector->update_from_skip_compacting_to_compacting(r->hrm_index());
                                     });
}

void G1FullGCCompactionPoint::forward_humongous(G1HeapRegion* hr) {
  assert(hr->is_starts_humongous(), "Sanity!");

  oop obj = cast_to_oop(hr->bottom());
  size_t old_size = obj->size();
  size_t new_size = obj->copy_size(old_size, obj->mark());
  uint num_regions = (uint)G1CollectedHeap::humongous_obj_size_in_regions(new_size);

  if (!has_regions()) {
    return;
  }

  // Find contiguous compaction target regions for the humongous object.
  uint range_begin = find_contiguous_before(hr, num_regions);

  if (range_begin == UINT_MAX) {
    // No contiguous compaction target regions found, so the object cannot be moved.
    return;
  }

  // Preserve the mark for the humongous object as the region was initially not compacting.
  preserved_stack()->push_if_necessary(obj, obj->mark());

  G1HeapRegion* dest_hr = _compaction_regions->at(range_begin);
  assert(hr->bottom() != dest_hr->bottom(), "assuming actual humongous move");
  FullGCForwarding::forward_to(obj, cast_to_oop(dest_hr->bottom()));
  assert(FullGCForwarding::is_forwarded(obj), "Object must be forwarded!");

  // Add the humongous object regions to the compaction point.
  add_humongous(hr);

  // Remove covered regions from compaction target candidates.
  _compaction_regions->remove_range(range_begin, (range_begin + num_regions));

  return;
}

uint G1FullGCCompactionPoint::find_contiguous_before(G1HeapRegion* hr, uint num_regions) {
  assert(num_regions > 0, "Sanity!");
  assert(has_regions(), "Sanity!");

  if (num_regions == 1) {
    // If only one region, return the first region.
    return 0;
  }

  uint contiguous_region_count = 1;

  uint range_end = 1;
  uint range_limit = (uint)_compaction_regions->length();

  for (; range_end < range_limit; range_end++) {
    if (contiguous_region_count == num_regions) {
      break;
    }
    // Check if the current region and the previous region are contiguous.
    bool regions_are_contiguous = (_compaction_regions->at(range_end)->hrm_index() - _compaction_regions->at(range_end - 1)->hrm_index()) == 1;
    contiguous_region_count = regions_are_contiguous ? contiguous_region_count + 1 : 1;
  }

  if (contiguous_region_count < num_regions &&
      hr->hrm_index() - _compaction_regions->at(range_end-1)->hrm_index() != 1) {
    // We reached the end but the final region is not contiguous with the target region;
    // no contiguous regions to move to.
    return UINT_MAX;
  }
  // Return the index of the first region in the range of contiguous regions.
  return range_end - contiguous_region_count;
}
