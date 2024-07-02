/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/allocation.hpp"
#include "oops/compressedKlass.inline.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/oopMapLUTable.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

OopMapLUTable::Entry* OopMapLUTable::_entries = nullptr;

void OopMapLUTable::initialize() {
  if (CompressedKlassPointers::tiny_classpointer_mode()) {
    assert(CompressedKlassPointers::tiny_classpointer_mode(), "sanity");
    assert(CompressedKlassPointers::narrow_klass_pointer_bits() <= 22, "sanity");
    _entries = NEW_C_HEAP_ARRAY(Entry, max_index(), mtX1);
    memset(_entries, 0xFE, max_index() * sizeof(Entry));
  }
}

#ifdef ASSERT
volatile uint64_t OopMapLUTable::hits_0 = 0;
volatile uint64_t OopMapLUTable::hits_non_0 = 0;
volatile uint64_t OopMapLUTable::misses = 0;

void OopMapLUTable::add_to_statistics(int rc) {
  switch (rc) {
  case 0: Atomic::inc(&hits_0); break;
  case 1: Atomic::inc(&hits_non_0); break;
  case 2: Atomic::inc(&misses); break;
  default: ShouldNotReachHere();
  }
}

void OopMapLUTable::print_statistics(outputStream* st) {
  st->print_cr("OopMapLUTable hits_0: " UINT64_FORMAT ", hits_non_0: " UINT64_FORMAT ", misses: " UINT64_FORMAT,
               hits_0, hits_non_0, misses);
}

void OopMapLUTable::verify_after_decode(Klass* k, int rc, const OopMapBlock* cached) {
  const InstanceKlass* ik = (const InstanceKlass*) k;
  if (rc == 0) {
    assert(ik->nonstatic_oop_map_count() == 0, "nonstatic_oop_map_count not 0 (%u)", ik->nonstatic_oop_map_count());
  } else {
    const OopMapBlock* real = ik->start_of_nonstatic_oop_maps();
    if (rc == 1) {
      assert(real->count() == cached->count() && real->offset() == cached->offset(), "mismatch");
    } else {
      assert(rc == 2, "wtf");
      assert(ik->nonstatic_oop_map_count() > 1 ||
          real->count() >= 0xFF || real->offset() > 0xFF, "mismatch");
    }
  }
}

#endif // ASSERT

jint* KlassSizeLUTable::_entries = nullptr;

void KlassSizeLUTable::initialize() {
  if (CompressedKlassPointers::tiny_classpointer_mode()) {
    assert(CompressedKlassPointers::tiny_classpointer_mode(), "sanity");
    assert(CompressedKlassPointers::narrow_klass_pointer_bits() <= 22, "sanity");
    _entries = NEW_C_HEAP_ARRAY(jint, max_index(), mtX2);
    memset(_entries, 0xFE, max_index() * sizeof(jint));
  }
}


#ifdef ASSERT
void KlassSizeLUTable::verify_after_decode(Klass* k, jint cached) {
  assert(k->layout_helper() == cached, "mismatch");
}
#endif
