/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
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
 */

#ifndef SHARE_GC_SHARED_SLIDINGFORWARDING_INLINE_HPP
#define SHARE_GC_SHARED_SLIDINGFORWARDING_INLINE_HPP

#include "gc/shared/slidingForwarding.hpp"
#include "oops/markWord.hpp"
#include "oops/oop.inline.hpp"

#ifdef _LP64
template <int NUM_REGION_BITS>
HeapWord* const SlidingForwarding<NUM_REGION_BITS>::UNUSED_BASE = reinterpret_cast<HeapWord*>(0x1);
#endif

template <int NUM_REGION_BITS>
SlidingForwarding<NUM_REGION_BITS>::SlidingForwarding(MemRegion heap, size_t region_size_words_shift)
#ifdef _LP64
: _heap_start(heap.start()),
  _num_regions(((heap.end() - heap.start()) >> region_size_words_shift) + 1),
  _region_size_words_shift(region_size_words_shift),
  _target_base_table(NEW_C_HEAP_ARRAY(HeapWord*, _num_regions * (ONE << NUM_REGION_BITS), mtGC)) {
  assert(region_size_words_shift <= NUM_COMPRESSED_BITS, "regions must not be larger than maximum addressing bits allow");
#else
{
#endif
}

template <int NUM_REGION_BITS>
SlidingForwarding<NUM_REGION_BITS>::~SlidingForwarding() {
#ifdef _LP64
  FREE_C_HEAP_ARRAY(HeapWord*, _target_base_table);
#endif
}

template <int NUM_REGION_BITS>
void SlidingForwarding<NUM_REGION_BITS>::clear() {
#ifdef _LP64
  size_t max = _num_regions * (ONE << NUM_REGION_BITS);
  for (size_t i = 0; i < max; i++) {
    _target_base_table[i] = UNUSED_BASE;
  }
#endif
}

#ifdef _LP64
template <int NUM_REGION_BITS>
size_t SlidingForwarding<NUM_REGION_BITS>::region_index_containing(HeapWord* addr) const {
  assert(addr >= _heap_start, "sanity: addr: " PTR_FORMAT " heap base: " PTR_FORMAT, p2i(addr), p2i(_heap_start));
  size_t index = ((size_t) (addr - _heap_start)) >> _region_size_words_shift;
  assert(index < _num_regions, "Region index is in bounds: " PTR_FORMAT, p2i(addr));
  return index;
}

template <int NUM_REGION_BITS>
bool SlidingForwarding<NUM_REGION_BITS>::region_contains(HeapWord* region_base, HeapWord* addr) const {
  return uintptr_t(addr - region_base) < (ONE << _region_size_words_shift);
}

template <int NUM_REGION_BITS>
uintptr_t SlidingForwarding<NUM_REGION_BITS>::encode_forwarding(HeapWord* original, HeapWord* target) {
  size_t orig_idx = region_index_containing(original);
  size_t base_table_idx = orig_idx * 2;
  size_t target_idx = region_index_containing(target);
  HeapWord* encode_base;
  uintptr_t region_idx;
  for (region_idx = 0; region_idx < (ONE << NUM_REGION_BITS); region_idx++) {
    encode_base = _target_base_table[base_table_idx + region_idx];
    if (encode_base == UNUSED_BASE) {
      encode_base = _heap_start + target_idx * (ONE << _region_size_words_shift);
      _target_base_table[base_table_idx + region_idx] = encode_base;
      break;
    } else if (region_contains(encode_base, target)) {
      break;
    }
  }
  assert(region_contains(encode_base, target), "region must contain target");
  uintptr_t encoded = (((uintptr_t)(target - encode_base)) << COMPRESSED_BITS_SHIFT) |
                      (region_idx << BASE_SHIFT) | markWord::marked_value;
  assert(target == decode_forwarding(original, encoded), "must be reversible");
  return encoded;
}

template <int NUM_REGION_BITS>
HeapWord* SlidingForwarding<NUM_REGION_BITS>::decode_forwarding(HeapWord* original, uintptr_t encoded) const {
  assert((encoded & markWord::marked_value) == markWord::marked_value, "must be marked as forwarded");
  size_t orig_idx = region_index_containing(original);
  size_t region_idx = (encoded >> BASE_SHIFT) & right_n_bits(NUM_REGION_BITS);
  size_t base_table_idx = orig_idx * 2 + region_idx;
  HeapWord* decoded = _target_base_table[base_table_idx] + (encoded >> COMPRESSED_BITS_SHIFT);
  return decoded;
}
#endif

template <int NUM_REGION_BITS>
void SlidingForwarding<NUM_REGION_BITS>::forward_to(oop original, oop target) {
#ifdef _LP64
  markWord header = original->mark();
  uintptr_t encoded = encode_forwarding(cast_from_oop<HeapWord*>(original), cast_from_oop<HeapWord*>(target));
  assert((encoded & markWord::klass_mask_in_place) == 0, "encoded forwardee must not overlap with Klass*");
  header = markWord((header.value() & markWord::klass_mask_in_place) | encoded);
  original->set_mark(header);
#else
  original->forward_to(target);
#endif
}

template <int NUM_REGION_BITS>
oop SlidingForwarding<NUM_REGION_BITS>::forwardee(oop original) const {
#ifdef _LP64
  markWord header = original->mark();
  uintptr_t encoded = header.value() & ~markWord::klass_mask_in_place;
  HeapWord* forwardee = decode_forwarding(cast_from_oop<HeapWord*>(original), encoded);
  return cast_to_oop(forwardee);
#else
  return original->forwardee();
#endif
}

#endif // SHARE_GC_SHARED_SLIDINGFORWARDING_INLINE_HPP
