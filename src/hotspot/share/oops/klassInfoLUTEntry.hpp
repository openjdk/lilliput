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

#ifndef SHARE_OOPS_KLASSINFOLUTENTRY_HPP
#define SHARE_OOPS_KLASSINFOLUTENTRY_HPP

// Included by oop.hpp, keep it short and sweet here
#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class OopMapBlock;
class Klass;
class outputStream;

//                msb                                          lsb
// not_in_table:       ---- all bits zero --------------------
//
// InstanceKlass:      KKKS SSSS SSSS SSSS CCCC CCCC OOOO OOOO
//
// ArrayKlass:         KKK- ---- hhhh hhhh tttt tttt eeee eeee
//                               |--- lower 24 bit of lh ----|
//
// Legend
// K : klass kind (3 bits)
// S : size in words (13 bits)
// C : count of first oopmap entry (8 bits)
// O : offset of first oopmap entry, in bytes (8 bits)
// h : hsz byte from layouthelper    ----------\
// t : element type byte from layouthelper      (lower 24 byte of layouthelper)
// e : log2 esz byte from layouthelper --------/
// - : unused

class KlassLUTEntry {

#define ALL_KLASS_KINDS_DO(what) \
		what(InstanceKlassKind) \
		what(InstanceRefKlassKind) \
    what(InstanceMirrorKlassKind) \
    what(InstanceClassLoaderKlassKind) \
    what(InstanceStackChunkKlassKind) \
    what(TypeArrayKlassKind) \
    what(ObjArrayKlassKind) \
		what(UnknownKlassKind)

  // Todo: move KlassKind out of Klass
  // Don't want to include it here for all the baggage it brings
  enum LocalKlassKind {
#define XX(name) name,
    ALL_KLASS_KINDS_DO(XX)
#undef XX
    LastKlassKind = ObjArrayKlassKind,
    FirstArrayKlassKind = TypeArrayKlassKind
  };

  static constexpr int bits_total      = 32;

  // All valid entries:  KK-- ---- ---- ---- ---- ---- ---- ----
  static constexpr int bits_kind       = 3;
  static constexpr int bits_common     = bits_kind; // extend if needed
  static constexpr int bits_specific   = bits_total - bits_common;

  // All entries
  struct U_K {
    // lsb
    unsigned other_bits : bits_specific;
    unsigned kind       : bits_kind;
    // msb
  };

  // InstanceKlass:      KKKS SSSS SSSS SSSS CCCC CCCC OOOO OOOO
  static constexpr int bits_ik_omb_offset = 8;
  static constexpr int bits_ik_omb_count  = 8;
  static constexpr int bits_ik_wordsize   = bits_specific - bits_ik_omb_count - bits_ik_omb_offset;
  struct UIK {
    // lsb
    unsigned omb_offset : bits_ik_omb_offset;
    unsigned omb_count  : bits_ik_omb_count;
    unsigned wordsize   : bits_ik_wordsize;
    unsigned other      : bits_common;
    // msb
  };

  // ArrayKlass:         KKKL LLLL LLLL LLLL LLLL LLLL LLLL LLLL
  static constexpr int bits_ak_lh_esz     = 8;
  static constexpr int bits_ak_lh_ebt     = 8;
  static constexpr int bits_ak_lh_hsz     = 8;
  static constexpr int bits_ak_lh         = bits_ak_lh_esz + bits_ak_lh_ebt + bits_ak_lh_hsz;
  struct UAK {
    // lsb
    // see klass.hpp
    unsigned lh_esz : bits_ak_lh_esz; // element size
    unsigned lh_ebt : bits_ak_lh_ebt; // element BasicType (currently unused)
    unsigned lh_hsz : bits_ak_lh_hsz; // header size (offset to first element)
    unsigned unused : bits_specific - bits_ak_lh_esz - bits_ak_lh_ebt - bits_ak_lh_hsz;
    unsigned other  : bits_common;
    // msb
  };

  union U {
    uint32_t raw;
    U_K common;
    UIK ike;
    UAK ake;
    U(uint32_t v) : raw(v) {}
  };

  const U _v;

  // returns explanation if invalid if error, nullptr if ok
  static const char* build_from_0(uint32_t& value, const Klass* k);
  static uint32_t build_from(const Klass* k);

  // The limits to what we can numerically represent in an (InstanceKlass) Entry
  static constexpr size_t ik_wordsize_limit = nth_bit(bits_ik_wordsize);
  static constexpr size_t ik_omb_offset_limit = nth_bit(bits_ik_omb_offset);
  static constexpr size_t ik_omb_count_limit = nth_bit(bits_ik_omb_count);

public:

  // Helper function. Returns true if k can be represented by a 32-bit entry, false if not.
  // Optionally, returns error string.
  static bool klass_is_representable(const Klass* ik, const char*& err);

  static constexpr uint32_t invalid_entry = 0;

  inline KlassLUTEntry() : _v(invalid_entry) {}
  inline KlassLUTEntry(uint32_t v) : _v(v) {}
  inline KlassLUTEntry(const KlassLUTEntry& other) : _v(other._v) {}

  KlassLUTEntry(const Klass* ik);

  bool valid() const      { return _v.raw != invalid_entry; }
  bool invalid() const    { return !valid(); }

#ifdef ASSERT
  void verify_against(const Klass* k) const;
#endif

  uint32_t value() const { return _v.raw; }

  // Following methods only if entry is valid:

  // Returns (our) kind
  inline unsigned kind() const { return _v.common.kind; }

  bool is_array() const     { return _v.common.kind >= FirstArrayKlassKind; }
  bool is_instance() const  { return !is_array(); }

  bool is_objArray() const  { return _v.common.kind == ObjArrayKlassKind; }
  bool is_typeArray() const { return _v.common.kind == TypeArrayKlassKind; }

  // Following methods only if IK:

  // Returns size, in words, of oops of this class
  inline size_t ik_wordsize() const;

  // Returns count of first OopMapBlock. Returns 0 if there is no OopMapBlock.
  inline unsigned ik_first_omb_count() const;

  // Returns offset of first OopMapBlock. Only call if count is > 0
  inline unsigned ik_first_omb_offset() const;

  // Following methods only if AK:

  // returns log2 element size
  inline unsigned ak_layouthelper_esz() const { return _v.ake.lh_esz; }

  // returns ebt byte
  inline unsigned ak_layouthelper_ebt() const { return _v.ake.lh_ebt; }

  // returns distance to first element
  inline unsigned ak_layouthelper_hsz() const { return _v.ake.lh_hsz; }

  // Valid for all valid entries:
  inline unsigned calculate_oop_wordsize_given_oop(oop obj) const;

}; // KlassInfoLUEntryIK

#endif // SHARE_OOPS_KLASSINFOLUTENTRY_HPP
