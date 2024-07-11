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
// InstanceKlass:      KKSS SSSS SSSS SSSS CCCC CCCC OOOO OOOO
// ArrayKlass:         KKLL LLLL LLLL LLLL LLLL LLLL LLLL LLLL
//
// K - klass kind (condensed)
// S - size in words (max 2^13)
// C - count of first oopmap entry (max 2^8)
// O - offset of first oopmap entry, in bytes (max 2^8)
// L - lower 30 bits of layouthelper.
//

class KlassLUTEntry {

public: // Dispatch needs this

  // Since not all kinds of Klass are representable, we omit some of the kinds and get a tighter representation
  // (2 bits instead of 3), a simpler dispatch, and we are able to form the kinds for AK in a way that it matches
  // layouthelper for AK.
  static constexpr int kind_instance_klass      = 0b00; // must be 0
  static constexpr int kind_instance_ref_klass  = 0b01;
  // kind_objarray_klass and kind_typearray_klass must match the two most significant bits of array-layouthelper
  static constexpr int kind_objarray_klass      = 0b10;  // 0x80
  static constexpr int kind_typearray_klass     = 0b11;  // 0xC0
  static constexpr int num_kinds = kind_typearray_klass + 1;

private:
  static int kind_to_ourkind(int kind);
  static int ourkind_to_kind(int kind);

  // All valid entries:  KK-- ---- ---- ---- ---- ---- ---- ----
  static constexpr int bits_kind       = 2;

  // InstanceKlass:      KKSS SSSS SSSS SSSS CCCC CCCC OOOO OOOO
  static constexpr int bits_ik_omb_offset = 8;
  static constexpr int bits_ik_omb_count  = 8;
  static constexpr int bits_ik_wordsize   = 16 - bits_kind; // 14 -> max representable ik instance size 16383 words = 131KB
  struct UIK {
    // lsb
    unsigned omb_offset : bits_ik_omb_offset;
    unsigned omb_count  : bits_ik_omb_count;
    unsigned wordsize   : bits_ik_wordsize;
    unsigned kind       : bits_kind;
    // msb
  };

  // ArrayKlass:         KKLL LLLL LLLL LLLL LLLL LLLL LLLL LLLL
  static constexpr int bits_ak_lh         = 32 - bits_kind;
  struct UAK {
    // lsb
    unsigned lh30       : bits_ak_lh;
    unsigned kind       : bits_kind;
    // msb
  };

  union U {
    uint32_t raw;
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

  // Helper function. Returns true if k can be represented by a 32-bit entry, false if not.
  // Optionally, returns error string.
  static bool klass_is_representable(const Klass* ik, const char*& err);

public:

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
  inline unsigned kind() const { return _v.ake.kind; }

  bool is_array() const     { return (_v.raw >> 31) != 0; }
  bool is_instance() const  { return (_v.raw >> 31) == 0; }

  // Following methods only if IK:

  // Returns size, in words, of oops of this class
  inline size_t ik_wordsize() const;

  // Returns count of first OopMapBlock. Returns 0 if there is no OopMapBlock.
  inline unsigned ik_first_omb_count() const;

  // Returns offset of first OopMapBlock. Only call if count is > 0
  inline unsigned ik_first_omb_offset() const;

  // Following methods only if AK:

  // returns layouthelper
  inline int ak_layouthelper() const;

  // Valid for all valid entries:
  inline unsigned calculate_oop_wordsize_given_oop(oop obj) const;

}; // KlassInfoLUEntryIK

#endif // SHARE_OOPS_KLASSINFOLUTENTRY_HPP
