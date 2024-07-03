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

#ifndef SHARE_OOPS_OOPMAPLUTABLE_INLINE_HPP
#define SHARE_OOPS_OOPMAPLUTABLE_INLINE_HPP

#include "oops/compressedKlass.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/oopMapLUTable.hpp"
#include "utilities/debug.hpp"

// InstanceKlassEntry

inline OopMapLUTable::InstanceKlassEntry::InstanceKlassEntry(uint32_t v) : _v(v) {
  assert(_v != invalid_entry, "invalid %x", _v);
  assert(!is_array(_v), "mysterious %x", _v);
}

inline uint32_t OopMapLUTable::InstanceKlassEntry::build_from(const InstanceKlass* ik) {

  const unsigned kind = (unsigned)ik->kind();
  assert(kind <= right_n_bits(kind_bits), "sanity");

  constexpr unsigned max_representable_lh = lh_is_not_representable - 1;
  assert(ik->layout_helper() > 0, "Sanity");
  const unsigned lh_orig = ik->layout_helper();
  const unsigned lh_condensed =
      lh_orig > max_representable_lh ? lh_is_not_representable : lh_orig;

  unsigned omb_condensed = 0;
  const int num_oopmap_entries = ik->nonstatic_oop_map_count();
  if (num_oopmap_entries == 0) {
    omb_condensed = omb_is_empty;
  } else if (num_oopmap_entries > 1) {
    omb_condensed = omb_is_not_representable;
  } else {
    const OopMapBlock* b = ik->start_of_nonstatic_oop_maps();
    assert(b->count() > 0, "zero-length oop map block?");
    assert(b->offset() >= 0, "negative oop map block offset?");
    const unsigned c = b->count();
    const unsigned o = (unsigned)b->offset();
    if (o > 0xFE || c > 0xFE) {
      omb_condensed = omb_is_not_representable;
    } else {
      omb_condensed = (o << 8) | c;
    }
  }

  assert(kind <= right_n_bits(kind_bits), "sanity");
  assert(lh_condensed <= right_n_bits(lh_bits), "sanity");
  assert(omb_condensed <= right_n_bits(omb_bits), "sanity");

  const uint32_t v =
      (0 << 31) // is_array = 0
      | (kind << kind_offset)
      | (lh_condensed << lh_offset)
      | (omb_condensed << omb_offset);

  return v;
}

inline OopMapLUTable::InstanceKlassEntry::InstanceKlassEntry(const InstanceKlass* ik) : _v(build_from(ik)) {
  assert(_v != invalid_entry, "invalid %x", _v);
  assert(!is_array(_v), "mysterious %x", _v);
}

inline Klass::KlassKind OopMapLUTable::InstanceKlassEntry::get_kind() const {
  const Klass::KlassKind kind = (Klass::KlassKind)get_kind_bits();
  assert(kind == Klass::InstanceClassLoaderKlassKind ||
         kind == Klass::InstanceKlassKind ||
         kind == Klass::InstanceMirrorKlassKind ||
         kind == Klass::InstanceRefKlassKind ||
         kind == Klass::InstanceStackChunkKlassKind, "weird");
  return kind;
}

// if layout helper is representable, returns true and the lh value, false otherwise
inline bool OopMapLUTable::InstanceKlassEntry::get_layouthelper(int& out) const {
  const unsigned lh = get_lh_bits();
  if (lh == lh_is_not_representable) {
    return false;
  }
  out = (int) lh;
  assert(out > 0, "odd");
  return true;
}

// returns:
// 0 if there is no oopmap
// 1 if it has only one entry, and it was representable. In that case, b contains the entry.
// 2 if it had more than one entries, or only one but it was unrepresentable.
inline int OopMapLUTable::InstanceKlassEntry::get_oopmapblock(OopMapBlock* b) const {
  const unsigned omb = get_omb_bits();
  if (omb == omb_is_not_representable) {
    return 2;
  } else if (omb == omb_is_empty) {
    return 0;
  } else {
    b->set_count(omb & 0xFF);
    b->set_offset((omb >> 8) & 0xFF);
    return 1;
  }
}

// ArrayKlassEntry

inline uint32_t OopMapLUTable::ArrayKlassEntry::build_from(const ArrayKlass* ak) {
  return (uint32_t)ak->layout_helper();
}

inline OopMapLUTable::ArrayKlassEntry::ArrayKlassEntry(uint32_t v) : _v(v) {
  assert(_v != invalid_entry, "bad %x", _v);
  assert(is_array(_v), "hmm %x", _v);
}

inline OopMapLUTable::ArrayKlassEntry::ArrayKlassEntry(const ArrayKlass* ak) : _v(build_from(ak)) {
  assert(_v != invalid_entry, "bad %x", _v);
  assert(is_array(_v), "not array? %x", _v);
}

inline Klass::KlassKind OopMapLUTable::ArrayKlassEntry::get_kind() const {
  const int lh = get_layouthelper();
  const bool is_objarray = Klass::layout_helper_is_objArray(lh);
  assert(is_objarray || Klass::layout_helper_is_typeArray(lh), "strange");
  return is_objarray ? Klass::ObjArrayKlassKind : Klass::TypeArrayKlassKind;
}

// OopMapLUTable

inline unsigned OopMapLUTable::num_entries() {
  return (unsigned) nth_bit(CompressedKlassPointers::narrow_klass_pointer_bits());
}

inline uint32_t* OopMapLUTable::entry_for_klass(const Klass* k) {
  const narrowKlass nk = CompressedKlassPointers::encode_not_null(const_cast<Klass*>(k));
  assert(nk < num_entries(), "nk oob (%u, max is %u)", nk, num_entries());
  return _entries + nk;
}

inline void OopMapLUTable::set_entry(const InstanceKlass* ik) {
  if (_entries == nullptr) {
    return;
  }
  uint32_t* const pe = OopMapLUTable::entry_for_klass(ik);
  InstanceKlassEntry ike(ik);
  *pe = ike.value();
  DEBUG_ONLY(ike.verify_against(ik));
}

inline void OopMapLUTable::set_entry(const ArrayKlass* ak) {
  if (_entries == nullptr) {
    return;
  }
  uint32_t* const pe = OopMapLUTable::entry_for_klass(ak);
  ArrayKlassEntry ake(ak);
  *pe = ake.value();
  DEBUG_ONLY(ake.verify_against(ak));
}

// returns:
// 0 if the klass has no oopmap entry
// 1 if the klass has just one entry, and the information in the table was sufficient
//   to restore it. In that case, b contains the one and only block.
// 2 if the klass has multiple entries, or a single entry that does not fit into the table.
//   In that case, caller needs to query the real oopmap.
inline int OopMapLUTable::try_get_oopmapblock(const InstanceKlass* ik, OopMapBlock* b) {
  if (_entries == nullptr) {
    return 2;
  }
  // note: very hot path. Do not dereference ik!
  const uint32_t* const pe = OopMapLUTable::entry_for_klass(ik);
  int rc = InstanceKlassEntry(*pe).get_oopmapblock(b);
#ifdef ASSERT
  if (rc == 0) {
    inc_hits_omb_zero();
  } else if (rc == 1) {
    inc_hits_omb_non_zero();
  } else {
    inc_misses_omb();
  }
#endif
  return rc;
}

inline bool OopMapLUTable::try_get_layouthelper(const Klass* k, int& out) {
  if (_entries == nullptr) {
    return false;
  }
  // note: very hot path. Do not dereference k!
  bool rc = false;
  const uint32_t* const pe = OopMapLUTable::entry_for_klass(k);
  const uint32_t v = *pe;
  if (is_array(v)) {
    out = ArrayKlassEntry(v).get_layouthelper();
    rc = true;
  } else {
    rc = InstanceKlassEntry(v).get_layouthelper(out);
  }
#ifdef ASSERT
  if (rc) {
    inc_hits_lh();
  } else {
    inc_misses_lh();
  }
#endif
  return rc;
}

inline bool OopMapLUTable::try_get_kind(const Klass* k, int& out) {
  // note: very hot path. Do not dereference k!
  if (_entries == nullptr) {
    return false;
  }
  const uint32_t* const pe = OopMapLUTable::entry_for_klass(k);
  const uint32_t v = *pe;
  if (is_array(v)) {
    return ArrayKlassEntry(v).get_kind();
  } else {
    return InstanceKlassEntry(v).get_kind();
  }
}

#endif // SHARE_OOPS_OOPMAPLUTABLE_INLINE_HPP
