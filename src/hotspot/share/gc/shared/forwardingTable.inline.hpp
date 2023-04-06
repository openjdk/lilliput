/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHARED_FORWARDINGTABLE_INLINE_HPP
#define SHARE_GC_SHARED_FORWARDINGTABLE_INLINE_HPP

#include "gc/shared/forwardingTable.hpp"
#include "utilities/ostream.hpp"

inline void FwdTableEntry::forward_to(HeapWord* from, HeapWord* to) {
  assert(_from == nullptr, "forward only once");
  assert(_to   == nullptr, "forward only once");
  _from = from;
  _to = to;
}

inline void PerRegionTable::forward_to(HeapWord* from, HeapWord* to) {
#ifdef ASSERT
  if (_insertion_idx > 0) {
    assert(_table[_insertion_idx].from() < from, "insertion must be monotonic");
  }
#endif
  assert(_insertion_idx < _num_forwardings, "must be within bounds");
  _table[_insertion_idx].forward_to(from, to);
  _insertion_idx++;
}

inline HeapWord* PerRegionTable::forwardee(HeapWord* from) {
  intx left = 0;
  intx right = _insertion_idx - 1;
  while (true) {
    if (left > right) {
      return nullptr;
    }
    // TODO: We could optimize this by not choosing the middle
    // but instead interpolate based on current bounds and
    // the address we are looking for.
    intx middle = (left + right) / 2;
    HeapWord* middle_value = _table[middle].from();
    if (middle_value < from) {
      left = middle + 1;
    } else if (middle_value > from) {
      right = middle - 1;
    } else {
      assert(middle_value == from, "must have found forwarding");
      return _table[middle].to();
    }
  }
}

inline void ForwardingTable::forward_to(oop from, oop to) {
  assert(_table != nullptr, "must have been initialized");
  size_t idx = _addr_to_idx(from);
  assert(idx < _max_regions, "must be within bounds");
  //tty->print_cr("forward_to: from: " PTR_FORMAT ", to: " PTR_FORMAT ", idx: " SIZE_FORMAT, p2i(from), p2i(to), idx);
  _table[idx].forward_to(cast_from_oop<HeapWord*>(from), cast_from_oop<HeapWord*>(to));
}

inline oop ForwardingTable::forwardee(oop from) {
  assert(_table != nullptr, "must have been initialized");
  size_t idx = _addr_to_idx(from);
  assert(idx < _max_regions, "must be within bounds: idx: " SIZE_FORMAT ", _max_regions: " SIZE_FORMAT, idx, _max_regions);
  HeapWord* to = _table[idx].forwardee(cast_from_oop<HeapWord*>(from));
  //tty->print_cr("forwardee: from: " PTR_FORMAT ", to: " PTR_FORMAT ", idx: " SIZE_FORMAT, p2i(from), p2i(to), idx);
  return cast_to_oop(to);
}

#endif // SHARE_GC_SHARED_FORWARDINGTABLE_INLINE_HPP
