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
#include "gc/shared/gc_globals.hpp"
#include "utilities/ostream.hpp"

inline void FwdTableEntry::forward_to(HeapWord* from, HeapWord* to) {
  _from = from;
  _to = to;
}

// When found, returns theindex into _table
// When not found, returns a negative value i from which the insertion index can be derived:
// insertion_idx = -(i + 1)
inline intx PerRegionTable::lookup(HeapWord* from) {
  intx left = 0;
  intx right = _insertion_idx - 1;
  while (true) {
    if (left > right) {
      assert(left >= 0 && left <= _insertion_idx, "must be in bounds and positive");
      assert(left == 0 || _table[left - 1].from() < from, "correct insertion point");
      assert(left == _insertion_idx || _table[left].from() > from, "correction insertion point");
      intx ins_pt_encoded = -left - 1;
      assert(ins_pt_encoded < 0, "must be negative");
      assert(-(ins_pt_encoded + 1) == left, "check decoding");
      return ins_pt_encoded;
    }
    intx middle = (left + right) / 2;
    HeapWord* middle_val = _table[middle].from();
    if (middle_val < from) {
      left = middle + 1;
    } else if (middle_val > from) {
      right = middle - 1;
    } else {
      assert(middle_val == from, "must have found forwarding");
      return middle;
    }
  }
}

inline void PerRegionTable::reforward(HeapWord* from, HeapWord* to) {
  intx idx = lookup(from);
  if (idx < 0) {
    intx ins_idx = -(idx + 1);
    assert(ins_idx >= 0 && ins_idx < _insertion_idx, "insertion index must be within bounds");
    assert(_insertion_idx < _num_forwardings, "must have space for insertion");
    assert(_insertion_idx > 0, "otherwise entry would be appended");
    assert(ins_idx == 0 || _table[ins_idx - 1].from() < from, "must insert in order");
    assert(_table[ins_idx].from() > from, "must insert in order");
    for (intx i = _insertion_idx - 1; i >= ins_idx; i--) {
      _table[i + 1] = _table[i];
    }
    _table[ins_idx].forward_to(from, to);
    _insertion_idx++;
  } else {
    _table[idx].forward_to(from, to);
  }
}

inline void PerRegionTable::forward_to(HeapWord* from, HeapWord* to) {
  if (_insertion_idx > 0 && _table[_insertion_idx - 1].from() >= from) {
    assert(UseG1GC || UseSerialGC, "happens only with G1 serial compaction");
    reforward(from, to);
    return;
  }
  assert(_insertion_idx < _num_forwardings, "must be within bounds: _insertion_idx: " INTX_FORMAT ", _num_forwardings: " INTX_FORMAT, _insertion_idx, _num_forwardings);
  assert(_used, "per region table must have been initialized");
  _table[_insertion_idx].forward_to(from, to);
  _insertion_idx++;
}

inline HeapWord* PerRegionTable::forwardee(HeapWord* from) {
  intx idx = lookup(from);
  if (idx < 0) {
    return nullptr;
  } else {
    return _table[idx].to();
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
