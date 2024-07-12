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

int KlassLUTEntry::kind_to_ourkind(int kind) {
  switch (kind) {
    case Klass::InstanceKlassKind: return kind_instance_klass;
    case Klass::InstanceRefKlassKind: return kind_instance_ref_klass;
    case Klass::ObjArrayKlassKind: return kind_objarray_klass;
    case Klass::TypeArrayKlassKind: return kind_typearray_klass;
    default: ShouldNotReachHere();
  }
  return -1;
}

int KlassLUTEntry::ourkind_to_kind(int kind) {
  switch (kind) {
    case kind_instance_klass: return Klass::InstanceKlassKind;
    case kind_instance_ref_klass: return Klass::InstanceRefKlassKind;
    case kind_objarray_klass: return Klass::ObjArrayKlassKind;
    case kind_typearray_klass: return Klass::TypeArrayKlassKind;
    default: ShouldNotReachHere();
  }
  return Klass::UnknownKlassKind;
}


bool KlassLUTEntry::klass_is_representable(const Klass* k, const char*& err) {

#define NOPE(s) err = s; return false;

  // Can always represent arrays (obj and type)
  if (k->is_array_klass()) {
    return true;
  }

  const int kind = k->kind();
  const int lh = k->layout_helper();

  const InstanceKlass* const ik = InstanceKlass::cast(k);

  if (kind != Klass::InstanceKlassKind &&
      kind != Klass::InstanceRefKlassKind) {
    NOPE("Unsupported InstanceKlass Kind");
  }

  if (Klass::layout_helper_needs_slow_path(lh)) {
    if (ik->is_abstract() || ik->is_interface()) {
      NOPE("Size not trivially computable (klass abstract or interface)");
    } else {
      NOPE("Size not trivially computable");
    }
  }

  const size_t wordsize = Klass::layout_helper_to_size_helper(lh);
  if (wordsize >= ik_wordsize_limit) {
    NOPE("Size too large");
  }

  // Has more than one nonstatic OopMapBlock?
  const int num_omb = ik->nonstatic_oop_map_count();
  if (num_omb > 1) {
    NOPE("More than 1 oop map blocks");
  }

  unsigned first_omb_count = 0;
  int first_omb_offset = 0;

  if (num_omb == 1) {
    first_omb_count = ik->start_of_nonstatic_oop_maps()->count();
    first_omb_offset = ik->start_of_nonstatic_oop_maps()->offset();

    assert(first_omb_count > 0, "Unexpected omb count");
    assert(first_omb_offset >= (oopDesc::header_size() * BytesPerWord), "Unexpected omb offset");

    if (first_omb_count >= ik_omb_count_limit) {
      NOPE("1 omb, but count too large");
    }

    if (first_omb_offset >= (int)ik_omb_offset_limit) {
      NOPE("1 omb, but offset too large");
    }
  }

  // otherwise, this should be fine
  return true;
}

#ifdef ASSERT

void KlassLUTEntry::verify_against(const Klass* k) const {

  // General static asserts that need access to private members, but I don't want
  // to place them in a header
  STATIC_ASSERT(bits_ak_lh == bits_ik_omb_offset + bits_ik_omb_count + bits_ik_wordsize);
  STATIC_ASSERT(sizeof(UAK) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(UIK) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(U) == sizeof(uint32_t));

  // We need some things to be true about the kind encoding
  STATIC_ASSERT(kind_instance_klass == 0);            // most common case, for performance reasons
  STATIC_ASSERT((kind_objarray_klass << 6)  == 0x80); // to match against lh for AK
  STATIC_ASSERT((kind_typearray_klass << 6) == 0xC0); // to match against lh for AK

  const char* err = nullptr;
  if (!klass_is_representable(k, err)) {
    assert(invalid(), "Klass is invalid (%s), but entry says its valid (%x)", err, _v.raw);
    return;
  }

  assert(valid(), "Entry should be valid (%x)", _v.raw);

  // kind
  const int real_kind = k->kind();
  const unsigned our_kind = kind();
  const int our_kind_translated = ourkind_to_kind((int)our_kind);

  assert(our_kind_translated == real_kind, "kind mismatch");

  const int real_lh = k->layout_helper();

  switch (real_kind) {
  case Klass::ObjArrayKlassKind:
  case Klass::TypeArrayKlassKind:
    // for AK we keep the layouthelper in the entry. It should match.
    assert(ak_layouthelper() == real_lh, "lh mismatch");
    break;

  case Klass::InstanceKlassKind:
  case Klass::InstanceRefKlassKind: {

    assert(k->is_instance_klass(), "unexpected");
    const InstanceKlass* const ik = InstanceKlass::cast(k);

    // word size
    const size_t our_wordsize = ik_wordsize();
    // (note: lh should not be slow-path, see klass_is_representable)
    const size_t real_wordsize = Klass::layout_helper_to_size_helper(real_lh);
    assert(our_wordsize == real_wordsize, "mismatch: wordsize");

    // omb
    if (ik->nonstatic_oop_map_count() == 0) {
      assert(ik_first_omb_count() == 0, "mismatch");
      assert(ik_first_omb_offset() == 0, "mismatch");
    } else {
      assert(ik->nonstatic_oop_map_count() == 1, "must be");
      assert(ik_first_omb_count() == ik->start_of_nonstatic_oop_maps()->count(), "mismatch");
      assert(ik_first_omb_offset() == (unsigned)ik->start_of_nonstatic_oop_maps()->offset(), "mismatch");
    }
  }
  break;
  default:
    ShouldNotReachHere();
  };

} // KlassLUTEntry::verify_against

#endif // ASSERT

uint32_t KlassLUTEntry::build_from(const Klass* k) {

  // First weed out invalid cases
  const char* err = nullptr;
  if (!klass_is_representable(k, err)) {
    ResourceMark rm;
    log_info(klut)("registering: %s: klute invalid (%s)", k->name()->as_C_string(), err);
    return invalid_entry;
  }

  uint32_t v = invalid_entry;

  const int kind = k->kind();
  const int lh = k->layout_helper();

  constexpr const char* const ok = nullptr;

  switch (kind) {
    case Klass::TypeArrayKlassKind:
    case Klass::ObjArrayKlassKind: {
      assert(k->is_array_klass(), "sanity");
      assert(Klass::layout_helper_is_objArray(lh) || Klass::layout_helper_is_typeArray(lh), "unexpected");

      // ArrayKlass:         KKLL LLLL LLLL LLLL LLLL LLLL LLLL LLLL

      // ArrayKlass is always representable with 32 bit. The content is just the klute kind bits overlayed
      // over the layouthelper. Since for arrays the klute bits are 10 (objarray kind) or 11 (typearray kind),
      // respectively, and that coincides with 0x80/0xC0 for OAK/TAK, it is really just the layouthelper.

#ifdef ASSERT
      // Double check that the encoding for our kind is correct - it should match
      // the upper two bits of the layout helper
      U u(0);
      u.raw = (uint32_t)lh;
      if (k->is_typeArray_klass()) {
        assert(u.ake.kind == kind_typearray_klass, "mismatch");
      } else {
        assert(k->is_objArray_klass(), "sanity");
        assert(u.ake.kind == kind_objarray_klass, "mismatch");
      }
#endif // ASSERT

      v = (uint32_t) lh;
    }
    break;

    case Klass::InstanceKlassKind:
    case Klass::InstanceRefKlassKind: {

      // Not every IK is representable as klute, but since we already filtered out invalid
      // cases, everything should fit now. We just assert here.
      const InstanceKlass* const ik = InstanceKlass::cast(k);

      assert(k->is_instance_klass(), "unexpected");
      assert(Klass::layout_helper_is_instance(lh), "unexpected");

      assert(!Klass::layout_helper_needs_slow_path(lh), "Did check already");
      const size_t wordsize = lh >> LogHeapWordSize;
      assert(wordsize < ik_wordsize_limit, "Did check already");

      // Has more than one nonstatic OopMapBlock?
      const int num_omb = ik->nonstatic_oop_map_count();
      assert(num_omb <= 1, "Did check already");

      unsigned first_omb_count = 0;
      int first_omb_offset = 0;

      if (num_omb == 1) {
        first_omb_count = ik->start_of_nonstatic_oop_maps()->count();
        assert(first_omb_count > 0, "Unexpected omb count");
        assert(first_omb_count < ik_omb_count_limit, "Did check already");

        first_omb_offset = ik->start_of_nonstatic_oop_maps()->offset();
        assert(first_omb_offset >= (oopDesc::header_size() * BytesPerWord), "Unexpected omb offset");
        assert(first_omb_offset < (int)ik_omb_offset_limit, "Did check already");
      }

      // InstanceKlass:      KKSS SSSS SSSS SSSS CCCC CCCC OOOO OOOO
      //
      // K - klass kind (condensed)
      // S - size in words (max 2^13)
      // C - count of first oopmap entry (max 2^8)
      // O - offset of first oopmap entry, in bytes (max 2^8)
      //

      U u(0);
      u.ike.kind = kind_to_ourkind(kind);
      u.ike.wordsize = wordsize;
      u.ike.omb_count = first_omb_count;
      u.ike.omb_offset = first_omb_offset;

      v = u.raw;
    }
    break;

    default:
      assert(false, "Did check already");
  };

  return v;
}

KlassLUTEntry::KlassLUTEntry(const Klass* k) : _v(build_from(k)) {
}

