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
#include "oops/klass.hpp"
#include "oops/oopMapLUTable.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

uint32_t* OopMapLUTable::_entries = nullptr;

void OopMapLUTable::initialize() {
  if (UseCompressedClassPointers &&
      CompressedKlassPointers::tiny_classpointer_mode() &&
      CompressedKlassPointers::use_oopmap_lu_table()) {
    assert(UseCompactObjectHeaders, "sanity");
    assert(CompressedKlassPointers::narrow_klass_pointer_bits() <= 22, "sanity");
    _entries = NEW_C_HEAP_ARRAY(uint32_t, num_entries(), mtX1);
    // each Klass, upon creation, initializes its entry, so it is okay to leave the
    // area uninitialized. Here, we just do it for later checking.
#ifdef ASSERT
    memset(_entries, 0xFF, num_entries() * sizeof(uint32_t));
    assert(_entries[0] == invalid_entry, "odd");
#endif
  }
}

#ifdef ASSERT

#define XX(xx)                      \
volatile uint64_t counter_##xx = 0; \
void OopMapLUTable::inc_##xx() {    \
  Atomic::inc(&counter_##xx);       \
}
STATS_DO(XX)
#undef XX

void OopMapLUTable::print_statistics(outputStream* st) {
  st->print("OopMapLUTable ");
#define XX(xx) st->print(#xx ":" UINT64_FORMAT ", ", counter_##xx);
STATS_DO(XX)
#undef XX
}

void OopMapLUTable::InstanceKlassEntry::verify_against(const InstanceKlass* k) const {
  const uint32_t v = *entry_for_klass(k);
  assert(!is_array(v), "Not an array?");
  InstanceKlassEntry ike(v);

  int lh = 0;
  if (ike.get_layouthelper(lh)) {
    assert(lh == k->layout_helper(), "lh differs");
  } else {
    assert(k->layout_helper() < 4096, "should have worked");
  }

  OopMapBlock b;
  const int result = ike.get_oopmapblock(&b);
  if (result == 0) {
    assert(k->nonstatic_oop_map_count() == 0, "mismatch");
  } else if (result == 1) {
    assert(k->nonstatic_oop_map_count() == 1, "mismatch");
    assert(k->start_of_nonstatic_oop_maps()->equals(&b), "mismatch");
  } else {
    assert(result == 2, "mismatch");
    assert(k->nonstatic_oop_map_count() > 1 ||
           k->start_of_nonstatic_oop_maps()->count() > 0xFE ||
           k->start_of_nonstatic_oop_maps()->offset() > 0xFE, "could have fit?");
  }

  assert(ike.get_kind() == k->kind(), "kind differs");
}

void OopMapLUTable::ArrayKlassEntry::verify_against(const ArrayKlass* k) const {
  const uint32_t v = *entry_for_klass(k);
  assert(is_array(v), "Not an array?");
  ArrayKlassEntry ake(v);
  assert(ake.get_layouthelper() == k->layout_helper(), "lh differs");
  assert(ake.get_kind() == k->kind(), "kind differs");
}

#endif // ASSERT
