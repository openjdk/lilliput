/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shenandoah/bitmapObjectMarker.hpp"
#include "logging/log.hpp"
#include "memory/virtualspace.hpp"
#include "runtime/os.hpp"

BitmapObjectMarker::BitmapObjectMarker(MemRegion heap_region) :
        _mark_bit_map(),
        _bitmap_region() {
  size_t bitmap_size = MarkBitMap::compute_size(heap_region.byte_size());
  ReservedSpace bitmap(bitmap_size);
  _bitmap_region = MemRegion((HeapWord*) bitmap.base(), bitmap.size() / HeapWordSize);
  _mark_bit_map.initialize(heap_region, _bitmap_region);

  os::commit_memory_or_exit((char*)_bitmap_region.start(), _bitmap_region.byte_size(), false,
                            "Could not commit native memory for auxiliary marking bitmap for JVMTI object marking");
  _mark_bit_map.clear();
}

BitmapObjectMarker::~BitmapObjectMarker() {
  if (!os::uncommit_memory((char*)_bitmap_region.start(), _bitmap_region.byte_size())) {
    log_warning(gc)("Could not uncommit native memory for auxiliary marking bitmap for JVMTI object marking");
  }
}

void BitmapObjectMarker::mark(oop o) {
  assert(Universe::heap()->is_in(o), "sanity check");
  assert(!is_marked(o), "should only mark an object once");
  _mark_bit_map.mark(o);
}

bool BitmapObjectMarker::is_marked(oop o) {
  assert(Universe::heap()->is_in(o), "sanity check");
  return _mark_bit_map.is_marked(o);
}
