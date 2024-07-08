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

#ifndef SHARE_OOPS_KLASSINFOLUTENTRY_INLINE_HPP
#define SHARE_OOPS_KLASSINFOLUTENTRY_INLINE_HPP

#include "oops/instanceKlass.hpp"
#include "oops/klass.hpp" // for KlassKind
#include "oops/klassInfoLUTEntry.hpp"
#include "oops/oop.hpp"
#include "utilities/debug.hpp"

inline KlassLUTEntry::KlassLUTEntry(uint32_t v) : _v(v) {}


// Returns kind
inline int KlassLUTEntry::kind() const {
  U u;
  u.raw = _v;
  return u.ake.kind;
}

// Following methods only if IK:

// Returns size, in words, of oops of this class
inline size_t KlassLUTEntry::ik_wordsize() const {
  assert(kind() < Klass::TypeArrayKlassKind, "only for ik entries");
  U u;
  u.raw = _v;
  return u.ike.wordsize;
}

// Returns count of first OopMapBlock. Returns 0 if there is no OopMapBlock.
inline unsigned KlassLUTEntry::ik_first_omb_count() const {
  assert(kind() < Klass::TypeArrayKlassKind, "only for ik entries");
  U u;
  u.raw = _v;
  return u.ike.omb_count;
}

// Returns offset of first OopMapBlock. Only call if count is > 0
inline unsigned KlassLUTEntry::ik_first_omb_offset() const {
  assert(kind() < Klass::TypeArrayKlassKind, "only for ik entries");
  U u;
  u.raw = _v;
  return u.ike.omb_offset;
}

// Following methods only if AK:

// returns layouthelper, restored to its full value
inline int KlassLUTEntry::ak_layouthelper_full() const {
  assert(kind() >= Klass::TypeArrayKlassKind, "only for ik entries");
  // We could probably shave some instructions here, since we essentially
  // just need to swap bits 30 and 29 (translate the upper nibble from
  // ("0x4/0x5" aka XXXArrayKlassKind to "C"|"8")
  uint32_t x = _v;
  x &= right_n_bits(bits_ak_lh);
  const uint32_t y = kind() == Klass::TypeArrayKlassKind ? 0xC0000000 : 0x80000000;
  x |= y;
  return (int)x;
}

// Valid for all valid entries:
inline unsigned KlassLUTEntry::calculate_oop_wordsize_given_oop(oop obj) const {
  assert(valid(), "must be valid");
  size_t rc = 0;
  if (kind() < Klass::TypeArrayKlassKind) {
    // IK is easy.
    rc = ik_wordsize();
  } else {
    // For AK, we calculate it from the layout helper. Note that we here ignore the upper three
    // bits containing kind. For the size calculation they do not matter. We only need element size
    // (lowest byte) and header size (byte 2).
    // See also oopDesk::size_given_klass
    const union {
      uint32_t raw;
      uint8_t u[4];
    } u = { _v };
    const int l2esz = u[0]; // Klass::layout_helper_log2_element_size
    const int hsz = u[2];   // Klass::layout_helper_header_size
    const size_t array_length = (size_t) ((arrayOop)obj)->length();
    const size_t size_in_bytes = (array_length << l2esz) + hsz;
    rc = align_up(size_in_bytes, MinObjAlignmentInBytes) / HeapWordSize;
  }
  return rc;
}

#endif // SHARE_OOPS_KLASSINFOLUTENTRY_INLINE_HPP
