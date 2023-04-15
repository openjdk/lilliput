/*
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

inline uintx murmur3_hash(uintx k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdLLU;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53LLU;
  k ^= k >> 33;
  return k;
}

// When found, returns the index into _table
// When not found, returns a negative value i from which the insertion index can be derived:
// insertion_idx = -(i + 1)
inline intx PerRegionTable::lookup(HeapWord* from) {
  uintx hash = murmur3_hash(reinterpret_cast<uintx>(from));
  uintx idx = hash % _table_size;
  // tty->print_cr("looking for: " PTR_FORMAT ", initial index: " UINTX_FORMAT ", table size: " UINTX_FORMAT, p2i(from), idx, _table_size);
  for (uintx i = idx; i < (idx + _table_size); i++) {
    uintx lookup_idx = i % _table_size;
    HeapWord* entry = _table[lookup_idx].from();
    if (entry == from) {
      // tty->print_cr("Found: " UINTX_FORMAT ": " PTR_FORMAT, lookup_idx, p2i(_table[lookup_idx].from()));
      return static_cast<intx>(lookup_idx);
    } else if (entry == nullptr) {
      //tty->print_cr("Not found: " UINTX_FORMAT, lookup_idx);
      return -static_cast<intx>(lookup_idx) - 1;
    }
    //tty->print_cr("Skipping: " UINTX_FORMAT ": " PTR_FORMAT, lookup_idx, p2i(_table[lookup_idx].from()));
  }
  guarantee(false, "Forwarding table overflow - must not happen, tried to find entry: " PTR_FORMAT ", region-used: %s", p2i(from), BOOL_TO_STR(_used));
  return 0;
}

inline void PerRegionTable::forward_to(HeapWord* from, HeapWord* to) {
  intx idx = lookup(from);
  if (idx >= 0) {
    assert(_table[idx].from() == from, "must have found correct entry");
    _table[idx].forward_to(from, to);
  } else {
    idx = -(idx + 1);
    assert(_table[idx].from() == nullptr, "must not have found entry");
    _table[idx].forward_to(from, to);
  }
}

inline HeapWord* PerRegionTable::forwardee(HeapWord* from) {
  if (!_used) {
    return nullptr;
  }
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
