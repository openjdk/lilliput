/*
 * Copyright (c) 2024, Red Hat, Inc. All rights reserved.
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
//#include "oops/compressedKlass.inline.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/klass.hpp"
#include "oops/klassInfoLUT.inline.hpp"
#include "oops/klassInfoLUTEntry.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

uint32_t* KlassInfoLUT::_entries = nullptr;

void KlassInfoLUT::initialize() {
  assert(UseKLUT, "?");
  assert(CompressedKlassPointers::narrow_klass_pointer_bits() <= 22, "sanity");
  // Note: this can be done a lot smarter, e.g. with spotwise mmap. We also should use large pages if possible.
  // For now, this suffices.
  _entries = NEW_C_HEAP_ARRAY(uint32_t, num_entries(), mtClass);
  for (unsigned i = 0; i < num_entries(); i++) {
    _entries[i] = KlassLUTEntry::invalid_entry;
  }
}

void KlassInfoLUT::register_klass(const Klass* k) {
  assert(UseKLUT, "?");
  const narrowKlass nk = CompressedKlassPointers::encode(const_cast<Klass*>(k)); // grr why is this nonconst
  assert(nk < num_entries(), "oob");
  KlassLUTEntry e(k);
  _entries[nk] = e.value();
#ifdef ASSERT
  // sanity checks
  {
    // We use at(), not get_entry(), since we don't want to log or count stats
    KlassLUTEntry e2(at(nk));
    assert(e2.value() == e.value(), "Sanity");
    e2.verify_against(k);
  }
  // stats
  switch (k->kind()) {
  case Klass::InstanceKlassKind:             inc_registered_IK(); break;
  case Klass::InstanceRefKlassKind:          inc_registered_IRK(); break;
  case Klass::InstanceMirrorKlassKind:       inc_registered_IMK(); break;
  case Klass::InstanceClassLoaderKlassKind:  inc_registered_ICLK(); break;
  case Klass::InstanceStackChunkKlassKind:   inc_registered_ISCK(); break;
  case Klass::TypeArrayKlassKind:            inc_registered_TAK(); break;
  case Klass::ObjArrayKlassKind:             inc_registered_OAK(); break;
  default: ShouldNotReachHere();
  };
#endif
}

#ifdef ASSERT

#define XX(xx)                      \
volatile uint64_t counter_##xx = 0; \
void KlassInfoLUT::inc_##xx() {     \
  Atomic::inc(&counter_##xx);       \
}
STATS_DO(XX)
#undef XX

void KlassInfoLUT::print_statistics(outputStream* st) {
  assert(UseKLUT, "?");
  st->print_cr("KLUT stats:");
#define XX(xx) st->print_cr("   " #xx ":" UINT64_FORMAT ", ", counter_##xx);
STATS_DO(XX)
#undef XX
}

#endif // ASSERT
