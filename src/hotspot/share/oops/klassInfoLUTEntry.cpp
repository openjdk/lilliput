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

// See klass.hpp
union LayoutHelperHelper {
  unsigned raw;
  struct {
    // lsb
    unsigned lh_esz : 8; // element size
    unsigned lh_ebt : 8; // element BasicType (currently unused)
    unsigned lh_hsz : 8; // header size (offset to first element)
    unsigned lh_tag : 8; // 0x80 or 0xc0
    // msb
  } bytes;
};

uint32_t KlassLUTEntry::build_from_ik(const InstanceKlass* ik, const char*& not_encodable_reason) {

  assert(ik->is_instance_klass(), "sanity");

  const int kind = ik->kind();
  const int lh = ik->layout_helper();

  U value(0);

  // Set common bits, these are always present
  assert(kind < 0b111, "sanity");
  value.common.kind = kind;

  // We may not be able to encode the IK-specific info; if we can't, those bits are left zero
  // and we return an error string for logging
  #define NOPE(s) { not_encodable_reason = s; return value.raw; }

  if (Klass::layout_helper_needs_slow_path(lh)) {
    if (ik->is_abstract() || ik->is_interface()) {
      NOPE("Size not trivially computable (klass abstract or interface)");
      // Please note that we could represent abstract or interface classes, but atm there is not
      // much of a point.
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

#undef NOPE
  // Okay, we are good.

  value.ike.wordsize = wordsize;
  value.ike.omb_count = first_omb_count;
  value.ike.omb_offset = first_omb_offset;

  return value.raw;

}

uint32_t KlassLUTEntry::build_from_ak(const ArrayKlass* ak) {

  assert(ak->is_array_klass(), "sanity");

  const int kind = ak->kind();
  const int lh = ak->layout_helper();

  assert(Klass::layout_helper_is_objArray(lh) || Klass::layout_helper_is_typeArray(lh), "unexpected");

  LayoutHelperHelper lhu = { (unsigned) lh };
  U value(0);
  value.common.kind = kind;
  value.ake.lh_ebt = lhu.bytes.lh_ebt;
  value.ake.lh_esz = lhu.bytes.lh_esz;
  value.ake.lh_hsz = lhu.bytes.lh_hsz;
  return value.raw;

}

uint32_t KlassLUTEntry::build_from(const Klass* k) {

  uint32_t value = invalid_entry;
  if (k->is_array_klass()) {
    value = build_from_ak(ArrayKlass::cast(k));
  } else {
    assert(k->is_instance_klass(), "sanity");
    const char* not_encodable_reason = nullptr;
    value = build_from_ik(InstanceKlass::cast(k), not_encodable_reason);
    if (not_encodable_reason != nullptr) {
      log_info(klut)("klass klute register: %s cannot be encoded because: %s.", k->name()->as_C_string(), not_encodable_reason);
    }
  }
  return value;

}

#ifdef ASSERT

void KlassLUTEntry::verify_against(const Klass* k) const {

  // General static asserts that need access to private members, but I don't want
  // to place them in a header
  STATIC_ASSERT(bits_common + bits_specific == bits_total);
  STATIC_ASSERT(32 == bits_total);
  STATIC_ASSERT(bits_ik_omb_offset + bits_ik_omb_count + bits_ik_wordsize <= bits_specific);
  STATIC_ASSERT(bits_ak_lh <= bits_specific);

  STATIC_ASSERT(sizeof(KE) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(AKE) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(IKE) == sizeof(uint32_t));
  STATIC_ASSERT(sizeof(U) == sizeof(uint32_t));

  // Kind must fit into 3 bits
  STATIC_ASSERT(Klass::KLASS_KIND_COUNT < nth_bit(bits_kind));

#define XX(name) STATIC_ASSERT((int)name == (int)Klass::name);
  ALL_KLASS_KINDS_DO(XX)
#undef XX

  assert(!is_invalid(), "Entry should be valid (%x)", _v.raw);

  // kind
  const unsigned real_kind = (unsigned)k->kind();
  const unsigned our_kind = kind();
  const int real_lh = k->layout_helper();

  assert(our_kind == real_kind, "kind mismatch (%d vs %d) (%x)", real_kind, our_kind, _v.raw);

  if (k->is_array_klass()) {

    // compare our (truncated) lh with the real one
    const LayoutHelperHelper lhu = { (unsigned) real_lh };
    assert(lhu.bytes.lh_ebt == ak_layouthelper_ebt() &&
           lhu.bytes.lh_esz == ak_layouthelper_esz() &&
           lhu.bytes.lh_hsz == ak_layouthelper_hsz() &&
           ( (lhu.bytes.lh_tag == 0xC0 && real_kind == Klass::TypeArrayKlassKind) ||
             (lhu.bytes.lh_tag == 0x80 && real_kind == Klass::ObjArrayKlassKind) ),
             "layouthelper mismatch (0x%x vs 0x%x)", real_lh, _v.raw);

  } else {

    assert(k->is_instance_klass(), "unexpected");
    const InstanceKlass* const ik = InstanceKlass::cast(k);

    if (ik_carries_infos()) {

      // check wordsize
      assert(Klass::layout_helper_needs_slow_path(real_lh) == false, "LH needs slow path?  (%x)", _v.raw);
      const int real_wordsize = Klass::layout_helper_to_size_helper(real_lh);
      assert(real_wordsize == ik_wordsize(), "wordsize mismatch? (%d vs %d) (%x)", real_wordsize, ik_wordsize(), _v.raw);

      // check omb info
      if (ik->nonstatic_oop_map_count() == 0) {
        assert(ik_first_omb_offset() == 0 && ik_first_omb_count() == 0, "omb should not be present (%x)", _v.raw);
      } else if (ik->nonstatic_oop_map_count() == 1) {
        assert(ik_first_omb_offset() == (unsigned)ik->start_of_nonstatic_oop_maps()[0].offset(), "first omb offset mismatch (%x)", _v.raw);
        assert(ik_first_omb_count() == ik->start_of_nonstatic_oop_maps()[0].count(), "first omb count mismatch (%x)", _v.raw);
      } else {
        fatal("More than one oop maps, IKE should not be encodable");
      }

    } else {
      // Check if this Klass should, in fact, have been encodable
      const bool lh_slow_path = Klass::layout_helper_needs_slow_path(real_lh);
      const int word_size = (lh_slow_path == false) ? Klass::layout_helper_to_size_helper(real_lh) : -1;
      const int oop_map_count = ik->nonstatic_oop_map_count();
      const int first_omb_offset = (oop_map_count == 1) ? ik->start_of_nonstatic_oop_maps()[0].offset() : -1;
      const int first_omb_count = (oop_map_count == 1) ? ik->start_of_nonstatic_oop_maps()[0].count() : -1;

      assert( lh_slow_path ||
              (word_size >= (int)ik_wordsize_limit) ||
              (oop_map_count > 1) ||
              (first_omb_offset >= (int)ik_omb_offset_limit) ||
              (first_omb_count >= (int)ik_omb_count_limit),
              "Klass should have been encodable" );
    }
  }

} // KlassLUTEntry::verify_against

#endif // ASSERT

KlassLUTEntry::KlassLUTEntry(const Klass* k) : _v(build_from(k)) {
}

