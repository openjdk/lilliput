/*
 * Copyright (c) 2016, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_INLINE_HPP
#define SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_INLINE_HPP

#include "gc/parallel/psParallelCompactNew.hpp"

#include "gc/parallel/parallelScavengeHeap.hpp"
#include "gc/parallel/parMarkBitMap.inline.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/continuationGCSupport.inline.hpp"
#include "gc/shared/fullGCForwarding.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/klass.hpp"
#include "oops/oop.inline.hpp"

inline bool PSParallelCompactNew::is_marked(oop obj) {
  return mark_bitmap()->is_marked(obj);
}

inline MutableSpace* PSParallelCompactNew::space(SpaceId id) {
  assert(id < last_space_id, "id out of range");
  return _space_info[id].space();
}

inline ObjectStartArray* PSParallelCompactNew::start_array(SpaceId id) {
  assert(id < last_space_id, "id out of range");
  return _space_info[id].start_array();
}

template <class T>
inline void PSParallelCompactNew::adjust_pointer(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(heap_oop)) {
    oop obj = CompressedOops::decode_not_null(heap_oop);
    assert(ParallelScavengeHeap::heap()->is_in(obj), "should be in heap");
    assert(is_marked(obj), "must be marked");
    if (!FullGCForwarding::is_forwarded(obj)) {
      return;
    }
    oop new_obj = FullGCForwarding::forwardee(obj);
    assert(new_obj != nullptr, "non-null address for live objects");
    assert(new_obj != obj, "inv");
    assert(ParallelScavengeHeap::heap()->is_in_reserved(new_obj),
           "should be in object space");
    RawAccess<IS_NOT_NULL>::oop_store(p, new_obj);
  }
}

#endif // SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_INLINE_HPP
