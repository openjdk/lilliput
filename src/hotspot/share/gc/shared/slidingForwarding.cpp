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

#include "precompiled.hpp"
#include "gc/shared/slidingForwarding.hpp"
#include "logging/log.hpp"
#include "utilities/ostream.hpp"
#include "utilities/concurrentHashTable.inline.hpp"
#include "utilities/fastHash.hpp"
#include "utilities/powerOfTwo.hpp"

static uintx hash(HeapWord* const& addr) {
  uint64_t val = reinterpret_cast<uint64_t>(addr);
  uint32_t hash = FastHash::get_hash32((uint32_t)val, (uint32_t)(val >> 32));
  return hash;
}

struct ForwardingEntry {
  HeapWord* _from;
  HeapWord* _to;
  ForwardingEntry(HeapWord* from, HeapWord* to) : _from(from), _to(to) {}
};

struct FallbackTableConfig {
  using Value = ForwardingEntry;
  static uintx get_hash(Value const& entry, bool* is_dead) {
    return hash(entry._from);
  }
  static void* allocate_node(void* context, size_t size, Value const& value) {
    return AllocateHeap(size, MEMFLAGS::mtGC);
  }
  static void free_node(void* context, void* memory, Value const& value) {
    FreeHeap(memory);
  }
};

class FallbackTable : public ConcurrentHashTable<FallbackTableConfig, MEMFLAGS::mtGC> {

};

class FallbackTableLookup : public StackObj {
  ForwardingEntry const _entry;
public:
  explicit FallbackTableLookup(HeapWord* from) : _entry(from, nullptr) {}
  uintx get_hash() const {
    return hash(_entry._from);
  }
  bool equals(ForwardingEntry* value) {
    return _entry._from == value->_from;
  }
  bool is_dead(ForwardingEntry* value) { return false; }
};

// We cannot use 0, because that may already be a valid base address in zero-based heaps.
// 0x1 is safe because heap base addresses must be aligned by much larger alignment
HeapWord* const SlidingForwarding::UNUSED_BASE = reinterpret_cast<HeapWord*>(0x1);

HeapWord* SlidingForwarding::_heap_start = nullptr;
size_t SlidingForwarding::_region_size_words = 0;
size_t SlidingForwarding::_heap_start_region_bias = 0;
size_t SlidingForwarding::_num_regions = 0;
uint SlidingForwarding::_region_size_bytes_shift = 0;
uintptr_t SlidingForwarding::_region_mask = 0;
HeapWord** SlidingForwarding::_biased_bases[SlidingForwarding::NUM_TARGET_REGIONS] = { nullptr, nullptr };
HeapWord** SlidingForwarding::_bases_table = nullptr;
FallbackTable* SlidingForwarding::_fallback_table = nullptr;
#ifndef PRODUCT
volatile uint64_t SlidingForwarding::_num_forwardings = 0;
volatile uint64_t SlidingForwarding::_num_fallback_forwardings = 0;
#endif

void SlidingForwarding::initialize(MemRegion heap, size_t region_size_words) {
#ifdef _LP64
  _heap_start = heap.start();

  // If the heap is small enough to fit directly into the available offset bits,
  // and we are running Serial GC, we can treat the whole heap as a single region
  // if it happens to be aligned to allow biasing.
  size_t rounded_heap_size = round_up_power_of_2(heap.byte_size());

  _region_size_words = MIN2(region_size_words, size_t(1 << NUM_OFFSET_BITS));
  _num_regions = (rounded_heap_size / BytesPerWord) / _region_size_words;
  _region_size_bytes_shift = log2i_exact(_region_size_words) + LogHeapWordSize;

  _heap_start_region_bias = (uintptr_t)_heap_start >> _region_size_bytes_shift;
  _region_mask = ~((uintptr_t(1) << _region_size_bytes_shift) - 1);

  guarantee((_heap_start_region_bias << _region_size_bytes_shift) == (uintptr_t)_heap_start, "must be aligned: _heap_start_region_bias: " SIZE_FORMAT ", _region_size_byte_shift: %u, _heap_start: " PTR_FORMAT, _heap_start_region_bias, _region_size_bytes_shift, p2i(_heap_start));

  assert(_region_size_words >= 1, "regions must be at least a word large");
  assert(_bases_table == nullptr, "should not be initialized yet");
  assert(_fallback_table == nullptr, "should not be initialized yet");
#endif
}

void SlidingForwarding::begin() {
#ifdef _LP64
  assert(_bases_table == nullptr, "should not be initialized yet");
  assert(_fallback_table == nullptr, "should not be initialized yet");

  _fallback_table = new FallbackTable();

#ifndef PRODUCT
  _num_forwardings = 0;
  _num_fallback_forwardings = 0;
#endif

  size_t max = _num_regions * NUM_TARGET_REGIONS;
  _bases_table = NEW_C_HEAP_ARRAY(HeapWord*, max, mtGC);
  HeapWord** biased_start = _bases_table - _heap_start_region_bias;
  _biased_bases[0] = biased_start;
  _biased_bases[1] = biased_start + _num_regions;
  for (size_t i = 0; i < max; i++) {
    _bases_table[i] = UNUSED_BASE;
  }
#endif
}

void SlidingForwarding::end() {
#ifndef PRODUCT
  log_info(gc)("Total forwardings: " UINT64_FORMAT ", fallback forwardings: " UINT64_FORMAT
                ", ratio: %f, memory used by fallback table: " SIZE_FORMAT "%s, memory used by bases table: " SIZE_FORMAT "%s",
               _num_forwardings, _num_fallback_forwardings, (float)_num_forwardings/(float)_num_fallback_forwardings,
               byte_size_in_proper_unit(_fallback_table->get_mem_size(Thread::current())),
               proper_unit_for_byte_size(_fallback_table->get_mem_size(Thread::current())),
               byte_size_in_proper_unit(sizeof(HeapWord*) * _num_regions * NUM_TARGET_REGIONS),
               proper_unit_for_byte_size(sizeof(HeapWord*) * _num_regions * NUM_TARGET_REGIONS));
#endif
#ifdef _LP64
  assert(_bases_table != nullptr, "should be initialized");
  FREE_C_HEAP_ARRAY(HeapWord*, _bases_table);
  _bases_table = nullptr;
  delete _fallback_table;
  _fallback_table = nullptr;
#endif
}

void SlidingForwarding::fallback_forward_to(HeapWord* from, HeapWord* to) {
  assert(to != nullptr, "no null forwarding");
  assert(_fallback_table != nullptr, "should be initialized");
  FallbackTableLookup lookup_f(from);
  ForwardingEntry entry(from, to);
  auto found_f = [&](ForwardingEntry* found) {
    // If dupe has been found, override it with new value.
    // This is also called when new entry is succussfully inserted.
    if (found->_to != to) {
      found->_to = to;
    }
  };
  Thread* current_thread = Thread::current();
  bool grow;
  bool added = _fallback_table->insert_get(current_thread, lookup_f, entry, found_f, &grow);
  NOT_PRODUCT(Atomic::inc(&_num_fallback_forwardings);)
#ifdef ASSERT
  assert(fallback_forwardee(from) != nullptr, "must have entered forwarding");
  assert(fallback_forwardee(from) == to, "forwarding must be correct, added: %s, from: " PTR_FORMAT ", to: " PTR_FORMAT ", fwd: " PTR_FORMAT, BOOL_TO_STR(added), p2i(from), p2i(to), p2i(fallback_forwardee(from)));
#endif
  if (grow) {
    _fallback_table->grow(current_thread);
    tty->print_cr("grow fallback table to size: " SIZE_FORMAT " bytes",
                  _fallback_table->get_mem_size(current_thread));
  }
}

HeapWord* SlidingForwarding::fallback_forwardee(HeapWord* from) {
  assert(_fallback_table != nullptr, "fallback table must be present");
  HeapWord* result;
  FallbackTableLookup lookup_f(from);
  auto found_f = [&](ForwardingEntry* found) {
    result = found->_to;
  };
  bool found = _fallback_table->get(Thread::current(), lookup_f, found_f);
  assert(found, "something must have been found");
  assert(result != nullptr, "must have found forwarding");
  return result;
}
