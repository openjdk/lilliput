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
#include "oops/oop.inline.hpp"
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

inline uintx PerRegionTable::home_index(HeapWord* from) const {
  if (_table_size_bits == 0) {
    return 0; // Single-element table would underflow the right-shift and hit UB
  }

  uint64_t val = reinterpret_cast<uint64_t>(from);
  val *= 0xbf58476d1ce4e5b9ull;
  val ^= val >> 56;
  val *= 0x94d049bb133111ebull;
  val = (val * 11400714819323198485llu) >> (64 - _table_size_bits);
  assert(val < _table_size, "must fit in table: val: " UINT64_FORMAT ", table-size: " UINTX_FORMAT ", table-size-bits: %d", val, _table_size, _table_size_bits);
  return reinterpret_cast<uintx>(val);
  //uintx hash = murmur3_hash(reinterpret_cast<uintx>(from));
  //return hash % _table_size;
}

inline uintx PerRegionTable::psl(uintx idx, uintx home) const {
  // Calculate distance between idx and home, accounting for
  // wrap-around.
  // By adding the table-size to idx, we ensure that the resulting
  // index is larger than home. Then we can subtract home and
  // modulo with table-size (by masking, table-size is power-of-2)
  // to get the actual difference.
  assert(home < _table_size, "home index must be within table");
  uintx psl = (idx + _table_size - home) & (_table_size - 1);
  assert(psl < _table_size, "must be within table size");
  return psl;
}

inline void PerRegionTable::forward_to(HeapWord* from, HeapWord* to) {
  uintx home_idx = home_index(from);
  uintx end = home_idx + _table_size;
  uintx mask = _table_size - 1;
  //tty->print_cr("Inserting: " PTR_FORMAT ", home index: " UINTX_FORMAT, p2i(from), home_idx);
  for (uintx i = home_idx; i < end; i++) {
    uintx idx = i & mask;
    uintx my_psl = psl(idx, home_idx);
    HeapWord* entry = _table[idx].from();
    if (entry == nullptr || entry == from) {
      //tty->print_cr("Inserted at idx: " UINTX_FORMAT, idx);
      // We found an empty slot or a slot containing the key we are looking for.
      _table[idx].forward_to(from, to);
      _max_psl = MAX2(my_psl, _max_psl);
      return;
    }
    // See if we shall swap the existing entry with the new one.
    // See: https://programming.guide/robin-hood-hashing.html
    // PSL = probe sequence length.
    uintx other_home_idx = home_index(entry);
    uintx other_psl = psl(idx, other_home_idx);
    //tty->print_cr("My PSL: " UINTX_FORMAT ", other PSL: " UINTX_FORMAT, my_psl, other_psl);
    if (my_psl > other_psl) {
      // Take from the rich, give to the poor :-)
      HeapWord* new_to = _table[idx].to();
      _table[idx].forward_to(from, to);
      from = entry;
      to = new_to;
      //tty->print_cr("swap, no inserting: " PTR_FORMAT, p2i(from));
      home_idx = other_home_idx;
      _max_psl = MAX2(my_psl, _max_psl);
    }
  }
  guarantee(false, "overflow while inserting");
}

inline HeapWord* PerRegionTable::forwardee(HeapWord* from) {
  if (!_used) {
    return nullptr;
  }
  //if (_max_psl > 0) tty->print_cr("Max-PSL: " UINTX_FORMAT, _max_psl);
  uintx home_idx = home_index(from);
  uintx mask = _table_size - 1;
  //tty->print_cr("Searching: " PTR_FORMAT ", home index: " UINTX_FORMAT, p2i(from), home_idx);
  for (uintx i = home_idx; i <= home_idx + _max_psl; i++) {
    //tty->print_cr("checking index: " UINTX_FORMAT, i);
    uintx idx = i & mask;
    HeapWord* entry = _table[idx].from();
    if (entry == from) {
      //tty->print_cr("Found at idx: " UINTX_FORMAT, idx);
      return _table[idx].to();
    } else if (entry == nullptr) {
      //tty->print_cr("Found null at idx: " UINTX_FORMAT, idx);
      return nullptr;
    } else {
      uintx my_psl = psl(idx, home_idx);
      uintx other_psl = psl(idx, home_index(entry));
      if (other_psl < my_psl) {
        //tty->print_cr("Found smaller PSL at idx: " UINTX_FORMAT, idx);
        return nullptr;
      }
    }
  }
  /*
  uintx middle = home_idx + _max_psl / 2;
  HeapWord* entry = _table[middle].from();
  if (entry == from) {
    return _table[middle].to();
  }

  uintx mask = _table_size - 1;
  for (uintx i = 1; i <= (_max_psl + 1) / 2; i++) {
    uintx left = (middle + _table_size - i) & mask;
    entry = _table[left].from();
    if (entry == from) {
      return _table[left].to();
    }
    uintx right = (middle + i) & mask;
    entry = _table[right].from();
    if (entry == from) {
      return _table[right].to();
    }
  }
  */
  return nullptr;
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
