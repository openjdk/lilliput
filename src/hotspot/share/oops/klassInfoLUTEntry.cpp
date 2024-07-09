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
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/klass.inline.hpp"
#include "oops/klassInfoLUTEntry.inline.hpp"
#include "utilities/debug.hpp"

// Helper function. Returns true if ik can be represented by a 32-bit entry
bool KlassLUTEntry::klass_is_representable(const Klass* k) {

  STATIC_ASSERT(bits_ak_lh == bits_ik_omb_offset + bits_ik_omb_count + bits_ik_wordsize);
  STATIC_ASSERT(sizeof(UAK) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(UIK) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(U) == sizeof(uint32_t));

  // Can always represent arrays (obj and type)
  if (k->is_array_klass()) {
    return true;
  }

  assert(k->is_instance_klass(), "sanity");

  const int kind = k->kind();
  const int lh = k->layout_helper();

  // for the moment we don't handle InstanceStackChunkKlassKind
  if (kind == Klass::InstanceStackChunkKlassKind) {
    return false;
  }

  // Size not trivially computable?
  if (Klass::layout_helper_needs_slow_path(lh)) {
    return false;
  }

  // Size trivially computable but would not fit?
  const size_t wordsize = Klass::layout_helper_to_size_helper(lh);
  if (wordsize >= ik_wordsize_limit) {
    return false;
  }

  const InstanceKlass* const ik = InstanceKlass::cast(k);

  // Has more than one nonstatic OopMapBlock?
  const int num_omb = ik->nonstatic_oop_map_count();
  if (num_omb > 1) {
    return false;
  }

  unsigned first_omb_count = 0;
  int first_omb_offset = 0;

  if (num_omb == 1) {
    first_omb_count = ik->start_of_nonstatic_oop_maps()->count();
    first_omb_offset = ik->start_of_nonstatic_oop_maps()->offset();

    assert(first_omb_count > 0, "Unexpected omb count");
    assert(first_omb_offset >= (oopDesc::header_size() * BytesPerWord), "Unexpected omb offset");

    // Has one omb, but it won't fit into 16 bit?
    if (first_omb_count >= ik_omb_count_limit ||
        first_omb_offset >= (int)ik_omb_offset_limit ) {
      return false;
    }
  }

  // otherwise, this should be fine
  return true;
}

#ifdef ASSERT

void KlassLUTEntry::verify_against(const Klass* k) const {

  if (!klass_is_representable(k)) {
    assert(invalid(), "should be invalid");
    return;
  }

  assert(valid(), "should be valid");

  // kind
  const int real_kind = k->kind();
  const int our_kind = kind();
  assert(our_kind == real_kind, "kind mismatch");

  const int real_lh = k->layout_helper();

  if (real_kind >= Klass::TypeArrayKlassKind) {

    // lh29
    const int our_lh = ak_layouthelper_full();
    assert(our_lh == real_lh, "lh mismatch");

  } else {

    const InstanceKlass* const ik = static_cast<const InstanceKlass*>(k);

    // word size
    const size_t our_wordsize = ik_wordsize();
    const size_t real_wordsize = Klass::layout_helper_to_size_helper(real_lh);
    assert(our_wordsize == real_wordsize, "mismatch: wordsize");

    // omb
    if (ik->nonstatic_oop_map_count() == 0) {
      assert(ik_first_omb_count() == 0, "mismatch");
      assert(ik_first_omb_offset() == 0, "mismatch");
    } else if (ik->nonstatic_oop_map_count() == 1) {
      assert(ik_first_omb_count() == ik->start_of_nonstatic_oop_maps()->count(), "mismatch");
      assert(ik_first_omb_offset() == (unsigned)ik->start_of_nonstatic_oop_maps()->offset(), "mismatch");
    } else {
      fatal("should have been an invalid entry");
    }
  }
} // KlassLUTEntry::verify_against

#endif // ASSERT

int KlassLUTEntry::build_from_0(uint32_t& value, const Klass* k) {

  const int kind = k->kind();
  const int lh = k->layout_helper();

  // We later expect TypeArrayKlassKind to be the dividing value between IK and AK
  STATIC_ASSERT(Klass::InstanceKlassKind            < Klass::TypeArrayKlassKind);
  STATIC_ASSERT(Klass::InstanceMirrorKlassKind      < Klass::TypeArrayKlassKind);
  STATIC_ASSERT(Klass::InstanceRefKlassKind         < Klass::TypeArrayKlassKind);
  STATIC_ASSERT(Klass::InstanceClassLoaderKlassKind < Klass::TypeArrayKlassKind);
  STATIC_ASSERT(Klass::InstanceStackChunkKlassKind  < Klass::TypeArrayKlassKind);
  STATIC_ASSERT(Klass::ObjArrayKlassKind            > Klass::TypeArrayKlassKind);

#define ERRBYE return (100000 + __LINE__);
  if (kind >= Klass::TypeArrayKlassKind) {
    assert(k->is_array_klass(), "sanity");
    assert(Klass::layout_helper_is_objArray(lh) || Klass::layout_helper_is_typeArray(lh), "unexpected");

    // ArrayKlass:         KKKL LLLL LLLL LLLL LLLL LLLL LLLL LLLL
    uint32_t lh29 = (uint32_t) lh;
    const uint32_t kind_mask = right_n_bits(bits_kind) << bits_ak_lh;
    lh29 &= ~kind_mask;

    U u(0);
    u.ake.kind = kind;
    u.ake.lh29 = lh29;

    value = u.raw;
    return 0; // ok

  } else {
    assert(k->is_instance_klass(), "unexpected");
    assert(Klass::layout_helper_is_instance(lh), "unexpected");

    // for the moment we don't handle InstanceStackChunkKlassKind
    if (kind == Klass::InstanceStackChunkKlassKind) {
      ERRBYE;
    }

    //                msb                                          lsb
    // InstanceKlass:      KKKS SSSS SSSS SSSS CCCC CCCC OOOO OOOO

    // Size not trivially computable?
    if (Klass::layout_helper_needs_slow_path(lh)) {
      ERRBYE;
    }

    // Size trivially computable but would not fit?
    const size_t wordsize = lh >> LogHeapWordSize;
    if (wordsize >= ik_wordsize_limit) {
      ERRBYE;
    }

    const InstanceKlass* const ik = InstanceKlass::cast(k);

    // Has more than one nonstatic OopMapBlock?
    const int num_omb = ik->nonstatic_oop_map_count();
    if (num_omb > 1) {
      ERRBYE;
    }

    unsigned first_omb_count = 0;
    int first_omb_offset = 0;

    if (num_omb == 1) {
      first_omb_count = ik->start_of_nonstatic_oop_maps()->count();
      first_omb_offset = ik->start_of_nonstatic_oop_maps()->offset();

      assert(first_omb_count > 0, "Unexpected omb count");
      assert(first_omb_offset >= (oopDesc::header_size() * BytesPerWord), "Unexpected omb offset");

      // Has one omb, but it won't fit into 16 bit?
      if (first_omb_count >= ik_omb_count_limit ||
          (size_t)first_omb_offset >= ik_omb_offset_limit ) {
        ERRBYE;
      }
    }

    // Okay, should fit.
    U u(0);
    u.ike.kind = kind;
    u.ike.wordsize = wordsize;
    u.ike.omb_count = first_omb_count;
    u.ike.omb_offset = first_omb_offset;

    value = u.raw;
    return 0; // ok

  }

  ShouldNotReachHere();

  return 0;
}

uint32_t KlassLUTEntry::build_from(const Klass* k) {
  uint32_t v = 0;
  const int rc = build_from_0(v, k);
  if (rc == 0) {
    return v;
  }
  ResourceMark rm;
  log_info(class, load)("%s: klute invalid (%d)", k->name()->as_C_string(), rc);
  return invalid_entry;
}

KlassLUTEntry::KlassLUTEntry(const Klass* k) : _v(build_from(k)) {
}

