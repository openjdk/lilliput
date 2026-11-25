/*
 * Copyright (c) 2005, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "compiler/oopMap.hpp"
#include "gc/parallel/objectStartArray.inline.hpp"
#include "gc/parallel/parallelArguments.hpp"
#include "gc/parallel/parallelScavengeHeap.inline.hpp"
#include "gc/parallel/parMarkBitMap.inline.hpp"
#include "gc/parallel/psAdaptiveSizePolicy.hpp"
#include "gc/parallel/psCompactionManagerNew.inline.hpp"
#include "gc/parallel/psOldGen.hpp"
#include "gc/parallel/psParallelCompactNew.inline.hpp"
#include "gc/parallel/psPromotionManager.inline.hpp"
#include "gc/parallel/psScavenge.hpp"
#include "gc/parallel/psYoungGen.hpp"
#include "gc/shared/classUnloadingContext.hpp"
#include "gc/shared/fullGCForwarding.inline.hpp"
#include "gc/shared/gcCause.hpp"
#include "gc/shared/gcHeapSummary.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcLocker.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "gc/shared/isGCActiveMark.hpp"
#include "gc/shared/oopStorageSetParState.inline.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shared/referencePolicy.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/referenceProcessorPhaseTimes.hpp"
#include "gc/shared/spaceDecorator.hpp"
#include "gc/shared/taskTerminator.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "gc/shared/workerThread.hpp"
#include "gc/shared/workerUtils.hpp"
#include "logging/log.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/memoryReserver.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "nmt/memTracker.hpp"
#include "oops/methodData.hpp"
#include "runtime/java.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/threads.hpp"
#include "runtime/vmThread.hpp"
#include "services/memoryService.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/events.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/macros.hpp"

SpaceInfoNew PSParallelCompactNew::_space_info[last_space_id];

size_t PSParallelCompactNew::_num_regions;
PCRegionData* PSParallelCompactNew::_region_data_array;
GrowableArray<PCRegionData*>* PSParallelCompactNew::_parallel_regions;
GrowableArray<PCRegionData*>* PSParallelCompactNew::_large_object_regions;
size_t PSParallelCompactNew::_num_regions_serial;
PCRegionData* PSParallelCompactNew::_region_data_array_serial;
GrowableArray<PCRegionData*>* PSParallelCompactNew::_serial_regions;
PCRegionData** PSParallelCompactNew::_top_parallel_region_array;
bool PSParallelCompactNew::_serial = false;

SpanSubjectToDiscoveryClosure PSParallelCompactNew::_span_based_discoverer;
ReferenceProcessor* PSParallelCompactNew::_ref_processor = nullptr;

void PSParallelCompactNew::print_on(outputStream* st) {
  _mark_bitmap.print_on(st);
}

STWGCTimer          PSParallelCompactNew::_gc_timer;
ParallelOldTracer   PSParallelCompactNew::_gc_tracer;
elapsedTimer        PSParallelCompactNew::_accumulated_time;
uint                PSParallelCompactNew::_maximum_compaction_gc_num = 0;
CollectorCounters*  PSParallelCompactNew::_counters = nullptr;
ParMarkBitMap       PSParallelCompactNew::_mark_bitmap;

PSParallelCompactNew::IsAliveClosure PSParallelCompactNew::_is_alive_closure;

class PCAdjustPointerClosure: public BasicOopIterateClosure {
  template <typename T>
  void do_oop_work(T* p) { PSParallelCompactNew::adjust_pointer(p); }

public:
  void do_oop(oop* p) final          { do_oop_work(p); }
  void do_oop(narrowOop* p) final    { do_oop_work(p); }

  ReferenceIterationMode reference_iteration_mode() final { return DO_FIELDS; }
};

static PCAdjustPointerClosure pc_adjust_pointer_closure;

class IsAliveClosure: public BoolObjectClosure {
public:
  bool do_object_b(oop p) final;
};


bool PSParallelCompactNew::IsAliveClosure::do_object_b(oop p) { return mark_bitmap()->is_marked(p); }

void PSParallelCompactNew::post_initialize() {
  ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
  _span_based_discoverer.set_span(heap->reserved_region());
  _ref_processor =
    new ReferenceProcessor(&_span_based_discoverer,
                           ParallelGCThreads,   // mt processing degree
                           ParallelGCThreads,   // mt discovery degree
                           false,               // concurrent_discovery
                           &_is_alive_closure); // non-header is alive closure

  _counters = new CollectorCounters("Parallel full collection pauses", 1);

  // Initialize static fields in ParCompactionManager.
  ParCompactionManagerNew::initialize(mark_bitmap());
}

bool PSParallelCompactNew::initialize_aux_data() {
  ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
  MemRegion mr = heap->reserved_region();
  assert(mr.byte_size() != 0, "heap should be reserved");

  initialize_space_info();

  if (!_mark_bitmap.initialize(mr)) {
    vm_shutdown_during_initialization(
      err_msg("Unable to allocate %zuKB bitmaps for parallel "
      "garbage collection for the requested %zuKB heap.",
      _mark_bitmap.reserved_byte_size()/K, mr.byte_size()/K));
    return false;
  }

  return true;
}

void PSParallelCompactNew::initialize_space_info()
{
  memset(&_space_info, 0, sizeof(_space_info));

  PSYoungGen* young_gen = ParallelScavengeHeap::heap()->young_gen();

  _space_info[old_space_id].set_space(ParallelScavengeHeap::heap()->old_gen()->object_space());
  _space_info[eden_space_id].set_space(young_gen->eden_space());
  _space_info[from_space_id].set_space(young_gen->from_space());
  _space_info[to_space_id].set_space(young_gen->to_space());

  _space_info[old_space_id].set_start_array(ParallelScavengeHeap::heap()->old_gen()->start_array());
}

void
PSParallelCompactNew::clear_data_covering_space(SpaceId id)
{
  // At this point, top is the value before GC, new_top() is the value that will
  // be set at the end of GC.  The marking bitmap is cleared to top; nothing
  // should be marked above top.
  MutableSpace* const space = _space_info[id].space();
  HeapWord* const bot = space->bottom();
  HeapWord* const top = space->top();

  _mark_bitmap.clear_range(bot, top);
}

void PSParallelCompactNew::pre_compact()
{
  // Update the from & to space pointers in space_info, since they are swapped
  // at each young gen gc.  Do the update unconditionally (even though a
  // promotion failure does not swap spaces) because an unknown number of young
  // collections will have swapped the spaces an unknown number of times.
  GCTraceTime(Debug, gc, phases) tm("Pre Compact", &_gc_timer);
  ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
  _space_info[from_space_id].set_space(ParallelScavengeHeap::heap()->young_gen()->from_space());
  _space_info[to_space_id].set_space(ParallelScavengeHeap::heap()->young_gen()->to_space());

  // Increment the invocation count
  heap->increment_total_collections(true);

  CodeCache::on_gc_marking_cycle_start();

  heap->print_before_gc();
  heap->trace_heap_before_gc(&_gc_tracer);

  // Fill in TLABs
  heap->ensure_parsability(true);  // retire TLABs

  if (VerifyBeforeGC && heap->total_collections() >= VerifyGCStartAt) {
    Universe::verify("Before GC");
  }

  DEBUG_ONLY(mark_bitmap()->verify_clear();)
}

void PSParallelCompactNew::synchronize_regions()
{
  // Update new_top for serial regions to reflect parallel forwarding.
  // Last non-empty region is the top (or first region if all empty).
  for (PCRegionData* parallel_region : *_parallel_regions) {
    SpaceId current_space_id = parallel_region->space_id();
    if (_top_parallel_region_array[current_space_id] == nullptr) {
      // First region is top by default.
      _top_parallel_region_array[current_space_id] = parallel_region;
    }

    if (parallel_region->new_top() == parallel_region->bottom()) {
      continue;
    } else {
      PCRegionData* serial_region = &_region_data_array_serial[current_space_id];
      if (parallel_region->new_top() > serial_region->new_top()) {
        serial_region->set_new_top(parallel_region->new_top());
        _top_parallel_region_array[current_space_id] = parallel_region;
      }
    }
  }
}

void PSParallelCompactNew::post_compact()
{
  GCTraceTime(Info, gc, phases) tm("Post Compact", &_gc_timer);

  CodeCache::on_gc_marking_cycle_finish();
  CodeCache::arm_all_nmethods();

  for (uint id = old_space_id; id < last_space_id; ++id) {
    // Clear the marking bitmap, summary data and split info.
    clear_data_covering_space(SpaceId(id));
  }

  size_t total_live = 0;
  size_t total_waste = 0;

  // Fill gaps and count stats
  if (_serial) {
    for (PCRegionData* region : *_serial_regions) {
      total_live += pointer_delta(region->new_top(), region->bottom());
      // No waste/gaps (perfect compaction within each space).
    }
  } else {
    // Fill gaps in parallel regions below each space's top.
    for (uint space_id = old_space_id; space_id < last_space_id; ++space_id) {
      PCRegionData* serial_region = &_region_data_array_serial[space_id];
      for (PCRegionData* region : *_parallel_regions) {
        if (region->space_id() != space_id) {
          continue;
        }
        size_t live = pointer_delta(region->new_top(), region->bottom());
        total_live += live;
        if (region == _top_parallel_region_array[space_id]) {
          // After the top of this region, we may have some perfectly compacted large objects, and then any free space for allocation.
          log_develop_trace(gc, compaction)(
                  "Live compaction region (final), #%zu: [" PTR_FORMAT ", " PTR_FORMAT "), new_top: " PTR_FORMAT ", live: %zu, waste: 0",
                  region->idx(), p2i(region->bottom()), p2i(region->end()), p2i(region->new_top()), live);
          break;
        }
        HeapWord* waste_start = region->new_top();
        HeapWord* waste_end = region->end();
        size_t waste = pointer_delta(waste_end, waste_start);
        total_waste += waste;
        if (waste > 0) {
          ObjectStartArray* sa = start_array(SpaceId(space_id));
          if (sa != nullptr) {
            sa->update_for_block(waste_start, waste_end);
          }
          ParallelScavengeHeap::heap()->fill_with_dummy_object(waste_start, waste_end, false);
        }

        log_develop_trace(gc, compaction)(
                "Live compaction region, #%zu: [" PTR_FORMAT ", " PTR_FORMAT "), new_top: " PTR_FORMAT ", live: %zu, waste: %zu",
                region->idx(), p2i(region->bottom()), p2i(region->end()), p2i(region->new_top()), live, waste);
      }
    }
    // Account for large objects in live count. Each region represents a single live object.
    for (PCRegionData* large_region : *_large_object_regions) {
      total_live += pointer_delta(large_region->end(), large_region->bottom());
    }
  }

  log_debug(gc, compaction)("total live: %zu, total waste: %zu, ratio: %f", total_live, total_waste, ((float)total_waste)/((float)(total_live + total_waste)));

  // Update space boundaries
  for (uint id = old_space_id; id < last_space_id; ++id) {
    MutableSpace* space = _space_info[id].space();
    PCRegionData* serial_region = &_region_data_array_serial[id];
    space->set_top(serial_region->new_top());
  }

  {
    FREE_C_HEAP_ARRAY(_region_data_array);
    FREE_C_HEAP_ARRAY(_region_data_array_serial);
    FREE_C_HEAP_ARRAY(_top_parallel_region_array);
    delete _parallel_regions;
    delete _large_object_regions;
    delete _serial_regions;
  }
#ifdef ASSERT
  {
    mark_bitmap()->verify_clear();
  }
#endif

  ParCompactionManagerNew::flush_all_string_dedup_requests();

  MutableSpace* const eden_space = _space_info[eden_space_id].space();
  MutableSpace* const from_space = _space_info[from_space_id].space();
  MutableSpace* const to_space   = _space_info[to_space_id].space();

  ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
  bool eden_empty = eden_space->is_empty();

  // Update heap occupancy information which is used as input to the soft ref
  // clearing policy at the next gc.
  Universe::heap()->update_capacity_and_used_at_gc();

  bool young_gen_empty = eden_empty && from_space->is_empty() &&
    to_space->is_empty();

  PSCardTable* ct = heap->card_table();
  MemRegion old_mr = ParallelScavengeHeap::heap()->old_gen()->committed();
  if (young_gen_empty) {
    ct->clear_MemRegion(old_mr);
  } else {
    ct->dirty_MemRegion(old_mr);
  }

  {
    // Delete metaspaces for unloaded class loaders and clean up loader_data graph
    GCTraceTime(Debug, gc, phases) t("Purge Class Loader Data", gc_timer());
    ClassLoaderDataGraph::purge(true /* at_safepoint */);
    DEBUG_ONLY(MetaspaceUtils::verify();)
  }

  // Need to clear claim bits for the next mark.
  ClassLoaderDataGraph::clear_claimed_marks();

  heap->prune_scavengable_nmethods();

#ifdef COMPILER2
  DerivedPointerTable::update_pointers();
#endif

  // Signal that we have completed a visit to all live objects.
  Universe::heap()->record_whole_heap_examined_timestamp();
}

void PSParallelCompactNew::setup_regions_parallel() {
  const size_t region_size_words = calculate_region_size();

  size_t num_regions = 0;
  for (uint i = old_space_id; i < last_space_id; ++i) {
    MutableSpace* const space = _space_info[i].space();
    size_t const space_size_words = space->capacity_in_words();
    num_regions += align_up(space_size_words, region_size_words) / region_size_words;
  }
  _region_data_array = NEW_C_HEAP_ARRAY(PCRegionData, num_regions * 2, mtGC);
  _parallel_regions = new (mtGC) GrowableArray<PCRegionData*>(num_regions, mtGC);
  _large_object_regions = new (mtGC) GrowableArray<PCRegionData*>(0, mtGC);

  size_t region_idx = 0;
  for (uint space_id = old_space_id; space_id < last_space_id; ++space_id) {
    const MutableSpace* space = _space_info[space_id].space();
    HeapWord* addr = space->bottom();
    HeapWord* sp_end = space->end();
    HeapWord* sp_top = space->top();
    while (addr < sp_end) {
      HeapWord* end = MIN2(align_up(addr + region_size_words, region_size_words), space->end());
      HeapWord* large_obj_start = nullptr;
      HeapWord* large_obj_end = nullptr;
      if (addr < sp_top) {
        HeapWord* prev_obj_start = _mark_bitmap.find_obj_beg_reverse(addr, end);
        if (prev_obj_start < end) {
          oop prev_obj = cast_to_oop(prev_obj_start);
          HeapWord* prev_obj_end = prev_obj_start + prev_obj->size();
          if (end < prev_obj_end) {
            // Object crosses region boundary.
            size_t large_object_threshold = region_size_words;
            size_t overlap_object_size = prev_obj->copy_size(prev_obj->size(), prev_obj->mark());
            if (overlap_object_size > large_object_threshold) {
              // Skip this large object for initial compaction. Finish the previous region just before this object
              // and we'll create a new region just for this object, to be compacted separately.
              large_obj_start = prev_obj_start;
              large_obj_end = prev_obj_end;
              end = large_obj_start;
            } else {
              // Expand this region to cover the object.
              end = prev_obj_end;
            }
          }
        }
      }
      HeapWord* top;
      if (sp_top < addr) {
        top = addr;
      } else if (sp_top >= end) {
        top = end;
      } else {
        top = sp_top;
      }
      assert(ParallelScavengeHeap::heap()->is_in_reserved(addr), "addr must be in heap: " PTR_FORMAT, p2i(addr));

      if (large_obj_start != nullptr) {
        // We have a large object at the end. Split into two regions.
        // 1. Normal prefix
        new (_region_data_array + region_idx) PCRegionData(region_idx, SpaceId(space_id), addr, large_obj_start, large_obj_start);
        _parallel_regions->append(&_region_data_array[region_idx]);
        region_idx++;
        // 2. Large suffix
        new (_region_data_array + region_idx) PCRegionData(region_idx, SpaceId(space_id), large_obj_start, large_obj_end, large_obj_end);
        _large_object_regions->append(&_region_data_array[region_idx]);
        region_idx++;
        addr = large_obj_end;
      } else {
        new (_region_data_array + region_idx) PCRegionData(region_idx, SpaceId(space_id), addr, top, end);
        _parallel_regions->append(&_region_data_array[region_idx]);
        region_idx++;
        addr = end;
      }
    }
  }
  _num_regions = region_idx;
  int num_parallel_regions = _parallel_regions->length();
  int num_large_object_regions = _large_object_regions->length();
  assert((size_t)num_parallel_regions <= num_regions, "must not exceed number of regions: parallel_regions: %d, num_regions: %zu", num_parallel_regions, num_regions);
  assert(num_large_object_regions <= num_parallel_regions, "max one large object region per parallel region: large_object_regions: %d, parallel_regions: %d", num_large_object_regions, num_parallel_regions);
  log_debug(gc)("Number of regions: %zu (normal: %d, large: %d), region size: " EXACTFMT, _num_regions, num_parallel_regions, num_large_object_regions, EXACTFMTARGS(region_size_words * HeapWordSize));
}

size_t PSParallelCompactNew::calculate_region_size() {
  // Minimum 0.5MB region size
  static const size_t FLOOR_REGION_SIZE_WORDS = (SpaceAlignment / HeapWordSize);
  size_t total_heap_words = 0;
  for (uint i = old_space_id; i < last_space_id; ++i) {
    total_heap_words += _space_info[i].space()->capacity_in_words();
  }

  // Per-worker region count for dynamic region sizing
  static const uint REGIONS_PER_WORKER = 100;  // Based on 1% boundary waste threshold
  const uint max_workers = ParallelScavengeHeap::heap()->workers().max_workers();
  const size_t total_regions_count = (size_t)max_workers * REGIONS_PER_WORKER;
  const size_t dynamic_region_size_words = total_heap_words / total_regions_count;
  return round_up_power_of_2(MAX2(dynamic_region_size_words, FLOOR_REGION_SIZE_WORDS));
}

void PSParallelCompactNew::setup_regions_serial() {
  _num_regions_serial = last_space_id;
  _region_data_array_serial = NEW_C_HEAP_ARRAY(PCRegionData, _num_regions_serial, mtGC);
  new (_region_data_array_serial + old_space_id)  PCRegionData(old_space_id, old_space_id, space(old_space_id)->bottom(), space(old_space_id)->top(), space(old_space_id)->end());
  new (_region_data_array_serial + eden_space_id) PCRegionData(eden_space_id, eden_space_id, space(eden_space_id)->bottom(), space(eden_space_id)->top(), space(eden_space_id)->end());
  new (_region_data_array_serial + from_space_id) PCRegionData(from_space_id, from_space_id, space(from_space_id)->bottom(), space(from_space_id)->top(), space(from_space_id)->end());
  new (_region_data_array_serial + to_space_id)   PCRegionData(to_space_id, to_space_id, space(to_space_id)->bottom(), space(to_space_id)->top(), space(to_space_id)->end());
  _serial_regions = new (mtGC) GrowableArray<PCRegionData*>(_num_regions_serial, mtGC);
  _serial_regions->append(&_region_data_array_serial[old_space_id]);
  _serial_regions->append(&_region_data_array_serial[eden_space_id]);
  _serial_regions->append(&_region_data_array_serial[from_space_id]);
  _serial_regions->append(&_region_data_array_serial[to_space_id]);
  _top_parallel_region_array = NEW_C_HEAP_ARRAY(PCRegionData*, _num_regions_serial, mtGC);
  for (uint i = 0; i < _num_regions_serial; i++) {
    _top_parallel_region_array[i] = nullptr;
  }
}

bool PSParallelCompactNew::check_maximum_compaction(bool should_do_max_compaction) {

  ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();

  // Check System.GC
  bool is_max_on_system_gc = UseMaximumCompactionOnSystemGC
                          && GCCause::is_user_requested_gc(heap->gc_cause());

  return should_do_max_compaction
      || is_max_on_system_gc;
}

void PSParallelCompactNew::summary_phase() {
  GCTraceTime(Info, gc, phases) tm("Summary Phase", &_gc_timer);

  setup_regions_serial();
  setup_regions_parallel();

#ifndef PRODUCT
  for (PCRegionData* region : *_serial_regions) {
    log_develop_trace(gc, compaction)("Compaction region (serial) #%zu: [" PTR_FORMAT ", " PTR_FORMAT ")",
                          region->idx(), p2i(region->bottom()), p2i(region->end()));
  }
  for (PCRegionData* region : *_parallel_regions) {
    log_develop_trace(gc, compaction)("Compaction region (parallel) #%zu: [" PTR_FORMAT ", " PTR_FORMAT ")",
                          region->idx(), p2i(region->bottom()), p2i(region->end()));
  }
  for (PCRegionData* region : *_large_object_regions) {
    log_develop_trace(gc, compaction)("Compaction region (large) #%zu: [" PTR_FORMAT ", " PTR_FORMAT ")",
                          region->idx(), p2i(region->bottom()), p2i(region->end()));
  }
#endif
}

// This method should contain all heap-specific policy for invoking a full
// collection.  invoke_no_policy() will only attempt to compact the heap; it
// will do nothing further.  If we need to bail out for policy reasons, scavenge
// before full gc, or any other specialized behavior, it needs to be added here.
//
// Note that this method should only be called from the vm_thread while at a
// safepoint.
//
// Note that the all_soft_refs_clear flag in the soft ref policy
// may be true because this method can be called without intervening
// activity.  For example when the heap space is tight and full measure
// are being taken to free space.
bool PSParallelCompactNew::invoke(bool clear_all_soft_refs, bool should_do_max_compaction) {
  assert(SafepointSynchronize::is_at_safepoint(), "should be at safepoint");
  assert(Thread::current() == (Thread*)VMThread::vm_thread(),
         "should be in vm thread");

  SvcGCMarker sgcm(SvcGCMarker::FULL);
  IsSTWGCActiveMark mark;

  ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();

  GCIdMark gc_id_mark;
  _gc_timer.register_gc_start();
  _gc_tracer.report_gc_start(heap->gc_cause(), _gc_timer.gc_start());

  GCCause::Cause gc_cause = heap->gc_cause();
  PSYoungGen* young_gen = ParallelScavengeHeap::heap()->young_gen();
  PSOldGen* old_gen = ParallelScavengeHeap::heap()->old_gen();
  PSAdaptiveSizePolicy* size_policy = heap->size_policy();

  // Make sure data structures are sane, make the heap parsable, and do other
  // miscellaneous bookkeeping.
  pre_compact();

  const PreGenGCValues pre_gc_values = heap->get_pre_gc_values();

  {
    const uint active_workers =
      WorkerPolicy::calc_active_workers(ParallelScavengeHeap::heap()->workers().max_workers(),
                                        ParallelScavengeHeap::heap()->workers().active_workers(),
                                        Threads::number_of_non_daemon_threads());
    ParallelScavengeHeap::heap()->workers().set_active_workers(active_workers);

    if (check_maximum_compaction(should_do_max_compaction)) {
      // Serial compaction executes the forwarding and compaction phases serially,
      // thus achieving perfect compaction.
      // Marking and ajust-references would still be executed in parallel threads.
      _serial = true;
    } else {
      _serial = false;
    }

    GCTraceCPUTime tcpu(&_gc_tracer);
    GCTraceTime(Info, gc) tm("Pause Full", nullptr, gc_cause, true);

    heap->pre_full_gc_dump(&_gc_timer);

    TraceCollectorStats tcs(counters());
    TraceMemoryManagerStats tms(heap->old_gc_manager(), gc_cause, "end of major GC");

    if (log_is_enabled(Debug, gc, heap, exit)) {
      accumulated_time()->start();
    }

    // Let the size policy know we're starting
    size_policy->major_collection_begin();

#ifdef COMPILER2
    DerivedPointerTable::clear();
#endif

    ref_processor()->start_discovery(clear_all_soft_refs);

    ClassUnloadingContext ctx(1 /* num_nmethod_unlink_workers */,
                              false /* unregister_nmethods_during_purge */,
                              false /* lock_nmethod_free_separately */);

    marking_phase(&_gc_tracer);

    summary_phase();

#ifdef COMPILER2
    assert(DerivedPointerTable::is_active(), "Sanity");
    DerivedPointerTable::set_active(false);
#endif

    FullGCForwarding::begin();

    if (_serial) {
      RegionSet serial_pass { _serial_regions, 1 };
      forward_normal_objects(&serial_pass);
      adjust_pointers();
      compact(&serial_pass, "Compaction Phase (maximum compaction)");
    } else {
      RegionSet parallel_pass { _parallel_regions, get_num_workers() };
      RegionSet large_objects_pass { _large_object_regions, 1 };
      forward_normal_objects(&parallel_pass);
      synchronize_regions();
      if (_large_object_regions->length() > 0) { // TODO: needed?
        // A separate serial pass for large objects which couldn't be compacted during the parallel pass.
        forward_large_objects();
      }
      adjust_pointers();
      compact(&parallel_pass, "Compaction Phase (normal objects)");
      if (_large_object_regions->length() > 0) {
        compact(&large_objects_pass, "Compaction Phase (large objects)");
      }
    }

    FullGCForwarding::end();

    ParCompactionManagerNew::_preserved_marks_set->restore(&ParallelScavengeHeap::heap()->workers());

    // Reset the mark bitmap, summary data, and do other bookkeeping.  Must be
    // done before resizing.
    post_compact();

    // Let the size policy know we're done
    size_policy->major_collection_end();

    size_policy->sample_old_gen_used_bytes(MAX2(pre_gc_values.old_gen_used(), old_gen->used_in_bytes()));

    if (UseAdaptiveSizePolicy) {
      heap->resize_after_full_gc();
    }

    heap->resize_all_tlabs();

    // Resize the metaspace capacity after a collection
    MetaspaceGC::compute_new_size();

    if (log_is_enabled(Debug, gc, heap, exit)) {
      accumulated_time()->stop();
    }

    heap->print_heap_change(pre_gc_values);

    // Track memory usage and detect low memory
    MemoryService::track_memory_usage();
    heap->update_counters();

    heap->post_full_gc_dump(&_gc_timer);

    size_policy->record_gc_pause_end_instant();
  }

  heap->gc_epilogue(true);

  if (VerifyAfterGC && heap->total_collections() >= VerifyGCStartAt) {
    Universe::verify("After GC");
  }

  heap->print_after_gc();
  heap->trace_heap_after_gc(&_gc_tracer);

  _gc_timer.register_gc_end();

  _gc_tracer.report_gc_end(_gc_timer.gc_end(), _gc_timer.time_partitions());

  return true;
}

class PCAddThreadRootsMarkingTaskClosureNew : public ThreadClosure {
private:
  uint _worker_id;

public:
  explicit PCAddThreadRootsMarkingTaskClosureNew(uint worker_id) : _worker_id(worker_id) { }
  void do_thread(Thread* thread) final {
    assert(ParallelScavengeHeap::heap()->is_stw_gc_active(), "called outside gc");

    ResourceMark rm;

    ParCompactionManagerNew* cm = ParCompactionManagerNew::gc_thread_compaction_manager(_worker_id);

    MarkingNMethodClosure mark_and_push_in_blobs(&cm->_mark_and_push_closure);

    thread->oops_do(&cm->_mark_and_push_closure, &mark_and_push_in_blobs);

    // Do the real work
    cm->follow_marking_stacks();
  }
};

void steal_marking_work_new(TaskTerminator& terminator, uint worker_id) {
  assert(ParallelScavengeHeap::heap()->is_stw_gc_active(), "called outside gc");

  ParCompactionManagerNew* cm =
    ParCompactionManagerNew::gc_thread_compaction_manager(worker_id);

  do {
    ScannerTask task;
    if (ParCompactionManagerNew::steal(worker_id, task)) {
      cm->follow_contents(task, true);
    }
    cm->follow_marking_stacks();
  } while (!terminator.offer_termination());
}

class MarkFromRootsTaskNew : public WorkerTask {
  NMethodMarkingScope _nmethod_marking_scope;
  ThreadsClaimTokenScope _threads_claim_token_scope;
  OopStorageSetStrongParState<false /* concurrent */, false /* is_const */> _oop_storage_set_par_state;
  TaskTerminator _terminator;
  uint _active_workers;

public:
  explicit MarkFromRootsTaskNew(uint active_workers) :
      WorkerTask("MarkFromRootsTaskNew"),
      _nmethod_marking_scope(),
      _threads_claim_token_scope(),
      _terminator(active_workers, ParCompactionManagerNew::marking_stacks()),
      _active_workers(active_workers) {}

  void work(uint worker_id) final {
    ParCompactionManagerNew* cm = ParCompactionManagerNew::gc_thread_compaction_manager(worker_id);
    {
      CLDToOopClosure cld_closure(&cm->_mark_and_push_closure, ClassLoaderData::_claim_stw_fullgc_mark);
      ClassLoaderDataGraph::always_strong_cld_do(&cld_closure);

      // Do the real work
      cm->follow_marking_stacks();
    }

    {
      PCAddThreadRootsMarkingTaskClosureNew closure(worker_id);
      Threads::possibly_parallel_threads_do(_active_workers > 1 /* is_par */, &closure);
    }

    // Mark from OopStorages
    {
      _oop_storage_set_par_state.oops_do(&cm->_mark_and_push_closure);
      // Do the real work
      cm->follow_marking_stacks();
    }

    if (_active_workers > 1) {
      steal_marking_work_new(_terminator, worker_id);
    }
  }
};

class ParallelCompactRefProcProxyTaskNew : public RefProcProxyTask {
  TaskTerminator _terminator;

public:
  explicit ParallelCompactRefProcProxyTaskNew(uint max_workers)
    : RefProcProxyTask("ParallelCompactRefProcProxyTaskNew", max_workers),
      _terminator(_max_workers, ParCompactionManagerNew::marking_stacks()) {}

  void work(uint worker_id) final {
    assert(worker_id < _max_workers, "sanity");
    ParCompactionManagerNew* cm = (_tm == RefProcThreadModel::Single) ? ParCompactionManagerNew::get_vmthread_cm() : ParCompactionManagerNew::gc_thread_compaction_manager(worker_id);
    BarrierEnqueueDiscoveredFieldClosure enqueue;
    ParCompactionManagerNew::FollowStackClosure complete_gc(cm, (_tm == RefProcThreadModel::Single) ? nullptr : &_terminator, worker_id);
    _rp_task->rp_work(worker_id, PSParallelCompactNew::is_alive_closure(), &cm->_mark_and_push_closure, &enqueue, &complete_gc);
  }

  void prepare_run_task_hook() final {
    _terminator.reset_for_reuse(_queue_count);
  }
};

void PSParallelCompactNew::marking_phase(ParallelOldTracer *gc_tracer) {
  // Recursively traverse all live objects and mark them
  GCTraceTime(Info, gc, phases) tm("Marking Phase", &_gc_timer);

  uint active_gc_threads = ParallelScavengeHeap::heap()->workers().active_workers();

  ClassLoaderDataGraph::verify_claimed_marks_cleared(ClassLoaderData::_claim_stw_fullgc_mark);
  {
    GCTraceTime(Debug, gc, phases) pm_tm("Par Mark", &_gc_timer);

    MarkFromRootsTaskNew task(active_gc_threads);
    ParallelScavengeHeap::heap()->workers().run_task(&task);
  }

  // Process reference objects found during marking
  {
    GCTraceTime(Debug, gc, phases) rp_tm("Reference Processing", &_gc_timer);

    ReferenceProcessorStats stats;
    ReferenceProcessorPhaseTimes pt(&_gc_timer, ref_processor()->max_num_queues());

    ParallelCompactRefProcProxyTaskNew task(ref_processor()->max_num_queues());
    stats = ref_processor()->process_discovered_references(task, &ParallelScavengeHeap::heap()->workers(), pt);

    gc_tracer->report_gc_reference_stats(stats);
    pt.print_all_references();
  }

  // This is the point where the entire marking should have completed.
  ParCompactionManagerNew::verify_all_marking_stack_empty();

  {
    GCTraceTime(Debug, gc, phases) wp_tm("Weak Processing", &_gc_timer);
    WeakProcessor::weak_oops_do(&ParallelScavengeHeap::heap()->workers(),
                                is_alive_closure(),
                                &do_nothing_cl,
                                1);
  }

  {
    GCTraceTime(Debug, gc, phases) tm_m("Class Unloading", &_gc_timer);

    ClassUnloadingContext* ctx = ClassUnloadingContext::context();

    bool unloading_occurred;
    {
      CodeCache::UnlinkingScope scope(is_alive_closure());

      // Follow system dictionary roots and unload classes.
      unloading_occurred = SystemDictionary::do_unloading(&_gc_timer);

      // Unload nmethods.
      CodeCache::do_unloading(unloading_occurred);
    }

    {
      GCTraceTime(Debug, gc, phases) t("Purge Unlinked NMethods", gc_timer());
      // Release unloaded nmethod's memory.
      ctx->purge_nmethods();
    }
    {
      GCTraceTime(Debug, gc, phases) ur("Unregister NMethods", &_gc_timer);
      ParallelScavengeHeap::heap()->prune_unlinked_nmethods();
    }
    {
      GCTraceTime(Debug, gc, phases) t("Free Code Blobs", gc_timer());
      ctx->free_nmethods();
    }

    // Prune dead klasses from subklass/sibling/implementor lists.
    Klass::clean_weak_klass_links(unloading_occurred);
  }

  {
    GCTraceTime(Debug, gc, phases) roc_tm("Report Object Count", &_gc_timer);
    _gc_tracer.report_object_count_after_gc(is_alive_closure(), &ParallelScavengeHeap::heap()->workers());
  }
#if TASKQUEUE_STATS
  ParCompactionManagerNew::print_and_reset_taskqueue_stats();
#endif
}

void PSParallelCompactNew::adjust_pointers_in_spaces(uint worker_id) {
  auto start_time = Ticks::now();
  for (size_t i = 0; i < _num_regions; i++) {
    PCRegionData* region = &_region_data_array[i];
    if (!region->claim()) {
      continue;
    }
    log_trace(gc, compaction)("Adjusting pointers in region: %zu (worker_id: %u)", region->idx(), worker_id);
    HeapWord* end = region->top();
    HeapWord* current = _mark_bitmap.find_obj_beg(region->bottom(), end);
    while (current < end) {
      assert(_mark_bitmap.is_marked(current), "must be marked");
      oop obj = cast_to_oop(current);
      size_t size = obj->size();
      obj->oop_iterate(&pc_adjust_pointer_closure);
      current = _mark_bitmap.find_obj_beg(current + size, end);
    }
  }
  log_trace(gc, phases)("adjust_pointers_in_spaces worker %u: %.3f ms", worker_id, (Ticks::now() - start_time).seconds() * 1000);
}

class PSAdjustTaskNew final : public WorkerTask {
  SubTasksDone                               _sub_tasks;
  WeakProcessor::Task                        _weak_proc_task;
  OopStorageSetStrongParState<false, false>  _oop_storage_iter;
  uint                                       _nworkers;

  enum PSAdjustSubTask {
    PSAdjustSubTask_code_cache,

    PSAdjustSubTask_num_elements
  };

public:
  explicit PSAdjustTaskNew(uint nworkers) :
    WorkerTask("PSAdjust task"),
    _sub_tasks(PSAdjustSubTask_num_elements),
    _weak_proc_task(nworkers),
    _nworkers(nworkers) {

    ClassLoaderDataGraph::verify_claimed_marks_cleared(ClassLoaderData::_claim_stw_fullgc_adjust);
    if (nworkers > 1) {
      Threads::change_thread_claim_token();
    }
  }

  ~PSAdjustTaskNew() {
    Threads::assert_all_threads_claimed();
  }

  void work(uint worker_id) final {
    ParCompactionManagerNew* cm = ParCompactionManagerNew::gc_thread_compaction_manager(worker_id);
    cm->preserved_marks()->adjust_during_full_gc();
    {
      // adjust pointers in all spaces
      PSParallelCompactNew::adjust_pointers_in_spaces(worker_id);
    }
    {
      ResourceMark rm;
      Threads::possibly_parallel_oops_do(_nworkers > 1, &pc_adjust_pointer_closure, nullptr);
    }
    _oop_storage_iter.oops_do(&pc_adjust_pointer_closure);
    {
      CLDToOopClosure cld_closure(&pc_adjust_pointer_closure, ClassLoaderData::_claim_stw_fullgc_adjust);
      ClassLoaderDataGraph::cld_do(&cld_closure);
    }
    {
      AlwaysTrueClosure always_alive;
      _weak_proc_task.work(worker_id, &always_alive, &pc_adjust_pointer_closure);
    }
    if (_sub_tasks.try_claim_task(PSAdjustSubTask_code_cache)) {
      NMethodToOopClosure adjust_code(&pc_adjust_pointer_closure, NMethodToOopClosure::FixRelocations);
      CodeCache::nmethods_do(&adjust_code);
    }
    _sub_tasks.all_tasks_claimed();
  }
};

void PSParallelCompactNew::adjust_pointers() {
  // Adjust the pointers to reflect the new locations
  GCTraceTime(Info, gc, phases) tm("Adjust Pointers", &_gc_timer);
  uint num_workers = ParallelScavengeHeap::heap()->workers().active_workers();
  PSAdjustTaskNew task(num_workers);
  ParallelScavengeHeap::heap()->workers().run_task(&task);
}

void PSParallelCompactNew::forward_normal_objects(RegionSet* regions) {
  GCTraceTime(Info, gc, phases) tm("Forward (normal objects)", &_gc_timer);

  class ForwardState {
    RegionSet* _regions;
    uint _worker_id;
    PCRegionData* _compaction_region;
    size_t _compaction_region_idx;
    HeapWord* _compaction_point;

    void ensure_compaction_point() {
      if (_compaction_point == nullptr) {
        assert(_compaction_region == nullptr, "invariant");
        _compaction_region = _regions->region_for_worker(_worker_id, 0);
        assert(_compaction_region != nullptr, "invariant");
        _compaction_point = _compaction_region->new_top();
      }
    }
  public:
    explicit ForwardState(RegionSet* regions, uint worker_id) :
            _regions(regions),
            _worker_id(worker_id),
            _compaction_region(nullptr),
            _compaction_region_idx(0),
            _compaction_point(nullptr) {
    }

    size_t available() const {
      return pointer_delta(_compaction_region->end(), _compaction_point);
    }

    void forward_objs_in_region(ParCompactionManagerNew* cm, PCRegionData* region) {
      ensure_compaction_point();
      HeapWord* end = region->top();
      HeapWord* current = _mark_bitmap.find_obj_beg(region->bottom(), end);
      while (current < end) {
        assert(_compaction_point >= _compaction_region->bottom() && _compaction_point <= _compaction_region->end(), "compaction point must be in compaction region");
        assert(_mark_bitmap.is_marked(current), "must be marked");
        oop obj = cast_to_oop(current);
        assert(region->contains(obj), "object must not cross region boundary: obj: " PTR_FORMAT ", obj_end: " PTR_FORMAT ", region start: " PTR_FORMAT ", region end: " PTR_FORMAT, p2i(obj), p2i(cast_from_oop<HeapWord*>(obj) + obj->size()), p2i(region->bottom()), p2i(region->end()));
        size_t old_size = obj->size();
        size_t copy_size = obj->copy_size(old_size, obj->mark());

        bool should_move = current != _compaction_point;
        size_t new_size = should_move ? copy_size : old_size;

        // Find compaction region
        while (new_size > available()) {
          _compaction_region->set_new_top(_compaction_point);
          log_develop_trace(gc, compaction)("Compaction region, worker: %u, #%zu: [" PTR_FORMAT ", " PTR_FORMAT "), new_top: " PTR_FORMAT,
                          _worker_id, _compaction_region->idx(), p2i(_compaction_region->bottom()), p2i(_compaction_region->end()), p2i(_compaction_region->new_top()));
          _compaction_region = _regions->region_for_worker(_worker_id, ++_compaction_region_idx);
          assert(_compaction_region != nullptr, "must find a compaction region");
          _compaction_point = _compaction_region->new_top();
          should_move = current != _compaction_point;
          new_size = should_move ? copy_size : old_size;
        }
        if (should_move) {
          // Forward
          cm->preserved_marks()->push_if_necessary(obj, obj->mark());
          FullGCForwarding::forward_to(obj, cast_to_oop(_compaction_point));
        }

        _compaction_point += new_size;
        assert(_compaction_point <= _compaction_region->end(), "object must fit in region");
        current += old_size;
        assert(current <= end, "object must not cross region boundary");
        current = _mark_bitmap.find_obj_beg(current, end);
      }
    }
    void finish() {
      if (_compaction_region != nullptr) {
        _compaction_region->set_new_top(_compaction_point);
        log_develop_trace(gc, compaction)("Compaction region (final), worker: %u, #%zu: [" PTR_FORMAT ", " PTR_FORMAT "), new_top: " PTR_FORMAT,
                _worker_id, _compaction_region->idx(), p2i(_compaction_region->bottom()), p2i(_compaction_region->end()), p2i(_compaction_region->new_top()));
      }
    }
  };

  struct ForwardTask final : public WorkerTask {
    RegionSet* _regions;
    ForwardTask(RegionSet* regions) : WorkerTask("PSForward task"), _regions(regions) {}

    void work(uint worker_id) override {
      ParCompactionManagerNew* cm = ParCompactionManagerNew::gc_thread_compaction_manager(worker_id);
      ForwardState state(_regions, worker_id);
      uint i = 0;
      PCRegionData* next_region = _regions->region_for_worker(worker_id, i);
      while (next_region != nullptr) {
        state.forward_objs_in_region(cm, next_region);
        next_region = _regions->region_for_worker(worker_id, ++i);
      }
      state.finish();
    }
  } task(regions);

  uint par_workers = ParallelScavengeHeap::heap()->workers().active_workers();
  ParallelScavengeHeap::heap()->workers().set_active_workers(regions->num_workers());
  ParallelScavengeHeap::heap()->workers().run_task(&task);
  ParallelScavengeHeap::heap()->workers().set_active_workers(par_workers);
}

void PSParallelCompactNew::forward_large_objects() {
  GCTraceTime(Info, gc, phases) tm("Forward (large objects)", &_gc_timer);
  ParCompactionManagerNew* cm = ParCompactionManagerNew::get_vmthread_cm();
  for (PCRegionData* region : *_large_object_regions) {
    oop obj = cast_to_oop(region->bottom());
    size_t copy_size = obj->copy_size(obj->size(), obj->mark());
    PCRegionData* dst_region = nullptr;
    bool should_forward = false;
    for (uint dst_space_id = old_space_id; dst_space_id <= region->space_id(); ++dst_space_id) {
      dst_region = &_region_data_array_serial[dst_space_id];
      // Lower spaces - check if there's room.
      if (dst_space_id < region->space_id()) {
        if (copy_size <= pointer_delta(dst_region->end(), dst_region->new_top())) {
          should_forward = true;
          break;
        } else {
          // Try the next space.
          continue;
        }
      }
      // Current space - only move downwards.
      assert(region->space_id() == dst_space_id, "objects cannot move to higher spaces");
      if (region->bottom() == dst_region->new_top()) {
        // Target is right where we are. Skip past this object.
        dst_region->set_new_top(region->end());
      } else if (region->bottom() < dst_region->new_top()) {
        // We are already below the compaction point, no need to move.
        assert(region->end() <= dst_region->new_top(), "compaction point cannot be inside object");
      } else {
        // We are moving down in the same space.
        should_forward = true;
      }
    }

    // Forward if necessary
    if (should_forward) {
      assert(copy_size <= pointer_delta(dst_region->end(), dst_region->new_top()), "must fit");
      cm->preserved_marks()->push_if_necessary(obj, obj->mark());
      FullGCForwarding::forward_to(obj, cast_to_oop(dst_region->new_top()));
      dst_region->set_new_top(dst_region->new_top() + copy_size);
    }
  }
}

void PSParallelCompactNew::compact(RegionSet* regions, const char* phase_name) {
  GCTraceTime(Info, gc, phases) tm(phase_name, &_gc_timer);
  class CompactTask final : public WorkerTask {
    RegionSet* _regions;
    static void compact_region(PCRegionData* region) {
      HeapWord* bottom = region->bottom();
      HeapWord* end = region->top();
      if (bottom == end) {
        return;
      }
      HeapWord* current = _mark_bitmap.find_obj_beg(bottom, end);
      while (current < end) {
        oop obj = cast_to_oop(current);
        size_t size = obj->size();
        if (FullGCForwarding::is_forwarded(obj)) {
          oop fwd = FullGCForwarding::forwardee(obj);
          auto* dst = cast_from_oop<HeapWord*>(fwd);
          ObjectStartArray* sa = start_array(space_id(dst));
          if (sa != nullptr) {
            assert(dst != current, "expect moving object");
            size_t new_words = obj->copy_size(size, obj->mark());
            sa->update_for_block(dst, dst + new_words);
          }

          Copy::aligned_conjoint_words(current, dst, size);
          fwd->reinit_mark();
          fwd->initialize_hash_if_necessary(obj);
        } else {
          // The start_array must be updated even if the object is not moving.
          ObjectStartArray* sa = start_array(space_id(current));
          if (sa != nullptr) {
            sa->update_for_block(current, current + size);
          }
        }
        current = _mark_bitmap.find_obj_beg(current + size, end);
      }
    }
  public:
    explicit CompactTask(RegionSet* regions) : WorkerTask("PSCompact task"), _regions(regions) {}
    void work(uint worker_id) override {
      uint region_id = 0;
      PCRegionData* region = _regions->region_for_worker(worker_id, region_id);
      while (region != nullptr) {
        log_trace(gc)("Compact worker: %u, compacting region: %zu", worker_id, region->idx());
        compact_region(region);
        region = _regions->region_for_worker(worker_id, ++region_id);
      }
    }
  } task(regions);

  uint par_workers = ParallelScavengeHeap::heap()->workers().active_workers();
  ParallelScavengeHeap::heap()->workers().set_active_workers(regions->num_workers());
  ParallelScavengeHeap::heap()->workers().run_task(&task);
  ParallelScavengeHeap::heap()->workers().set_active_workers(par_workers);
}

// Return the SpaceId for the space containing addr.  If addr is not in the
// heap, last_space_id is returned.  In debug mode it expects the address to be
// in the heap and asserts such.
SpaceId PSParallelCompactNew::space_id(HeapWord* addr) {
  assert(ParallelScavengeHeap::heap()->is_in_reserved(addr), "addr not in the heap");

  for (uint id = old_space_id; id < last_space_id; ++id) {
    if (_space_info[id].space()->contains(addr)) {
      return SpaceId(id);
    }
  }

  assert(false, "no space contains the addr");
  return last_space_id;
}

PCRegionData* region_for_worker(uint worker_id, uint num_workers, uint idx, GrowableArray<PCRegionData*>* regions) {
  size_t position = num_workers * idx + worker_id;
  if (position >= (size_t)regions->length()) {
    return nullptr;
  }
  return regions->at(position);
}
