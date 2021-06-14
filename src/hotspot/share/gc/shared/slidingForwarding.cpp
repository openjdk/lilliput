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
 *2,q
 */

#include "precompiled.hpp"
#include "gc/shared/slidingForwarding.hpp"
#include "oops/markWord.hpp"
#include "oops/oop.inline.hpp"

#ifdef _LP64
HeapWord* const SlidingForwarding::UNUSED_BASE = reinterpret_cast<HeapWord*>(0x1);
#endif

SlidingForwarding::SlidingForwarding(MemRegion heap, size_t region_size_words_shift)
#ifdef _LP64
: _heap_start(heap.start()),
  _num_regions(((heap.end() - heap.start()) >> region_size_words_shift) + 1),
  _region_size_words_shift(region_size_words_shift),
  _target_base_table(NEW_C_HEAP_ARRAY(HeapWord*, _num_regions * 2, mtGC)) {
  assert(region_size_words_shift <= NUM_COMPRESSED_BITS, "regions must not be larger than maximum addressing bits allow");
#else
{
#endif
}

SlidingForwarding::~SlidingForwarding() {
#ifdef _LP64
  FREE_C_HEAP_ARRAY(HeapWord*, _target_base_table);
#endif
}

void SlidingForwarding::clear() {
#ifdef _LP64
  size_t max = _num_regions * 2;
  for (size_t i = 0; i < max; i++) {
    _target_base_table[i] = UNUSED_BASE;
  }
#endif
}

#ifdef _LP64
size_t SlidingForwarding::region_index_containing(HeapWord* addr) const {
  assert(addr >= _heap_start, "sanity: addr: " PTR_FORMAT " heap base: " PTR_FORMAT, p2i(addr), p2i(_heap_start));
  size_t index = ((size_t) (addr - _heap_start)) >> _region_size_words_shift;
  assert(index < _num_regions, "Region index is in bounds: " PTR_FORMAT, p2i(addr));
  return index;
}

bool SlidingForwarding::region_contains(HeapWord* region_base, HeapWord* addr) const {
  return (addr - region_base) < (ptrdiff_t)(ONE << _region_size_words_shift);
}

uintptr_t SlidingForwarding::encode_forwarding(HeapWord* original, HeapWord* target) {
  size_t orig_idx = region_index_containing(original);
  size_t base_table_idx = orig_idx * 2;
  size_t target_idx = region_index_containing(target);
  HeapWord* encode_base = _target_base_table[base_table_idx];
  uintptr_t flag = 0;
  if (encode_base == UNUSED_BASE) {
    encode_base = _heap_start + target_idx * (ONE << _region_size_words_shift);
    _target_base_table[base_table_idx] = encode_base;
  } else if (!region_contains(encode_base, target)) {
    base_table_idx++;
    flag = 1;
    encode_base = _target_base_table[base_table_idx];
    if (encode_base == UNUSED_BASE) {
      encode_base = _heap_start + target_idx * (ONE << _region_size_words_shift);
      _target_base_table[base_table_idx] = encode_base;
    }
  }
  assert(region_contains(encode_base, target), "region must contain target");
  uintptr_t encoded = (((uintptr_t)(target - encode_base)) << COMPRESSED_BITS_SHIFT) |
                      (flag << REGION_INDICATOR_FLAG_SHIFT) | markWord::marked_value;
  assert(target == decode_forwarding(original, encoded), "must be reversible");
  return encoded;
}

HeapWord* SlidingForwarding::decode_forwarding(HeapWord* original, uintptr_t encoded) {
  assert((encoded & markWord::marked_value) == markWord::marked_value, "must be marked as forwarded");
  size_t orig_idx = region_index_containing(original);
  size_t flag = (encoded >> REGION_INDICATOR_FLAG_SHIFT) & 1;
  size_t base_table_idx = orig_idx * 2 + flag;
  HeapWord* decoded = _target_base_table[base_table_idx] + (encoded >> COMPRESSED_BITS_SHIFT);
  return decoded;
}
#endif

void SlidingForwarding::forward_to(oop original, oop target) {
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

oop SlidingForwarding::forwardee(oop original) {
#ifdef _LP64
  markWord header = original->mark();
  uintptr_t encoded = header.value() & ~markWord::klass_mask_in_place;
  HeapWord* forwardee = decode_forwarding(cast_from_oop<HeapWord*>(original), encoded);
  return cast_to_oop(forwardee);
#else
  return original->forwardee();
#endif
}
