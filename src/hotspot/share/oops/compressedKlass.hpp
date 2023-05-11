/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 *
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

#ifndef SHARE_OOPS_COMPRESSEDKLASS_HPP
#define SHARE_OOPS_COMPRESSEDKLASS_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;
class Klass;

// Narrow Klass pointer constants;
#ifdef _LP64
// All these depend on UseCompactObjectHeaders
extern int LogKlassAlignmentInBytes;
extern int KlassAlignmentInBytes;
extern int KlassAlignmentInWords;

// Max. allowed size of compressed class pointer, in bits
extern int MaxNarrowKlassPointerBits;

// Mask to mask in the bits which are valid to be set in a narrow Klass pointer
extern uint64_t NarrowKlassPointerBitMask;

// Maximal size of compressed class pointer encoding range
extern uint64_t KlassEncodingMetaspaceMax;

#else
// Why is this even needed in 32-bit? Todo: fix.
const int LogKlassAlignmentInBytes = 3; // traditional 64-bit alignment
const int KlassAlignmentInBytes    = 1 << LogKlassAlignmentInBytes;
const int KlassAlignmentInWords = KlassAlignmentInBytes / BytesPerWord;
const int MaxNarrowKlassPointerBits = 22; // should never be used.
const uint64_t  NarrowKlassPointerBitMask = ((((uint64_t)1) << MaxNarrowKlassPointerBits) - 1);
const uint64_t KlassEncodingMetaspaceMax = (uint64_t(max_juint) + 1) << LogKlassAlignmentInBytes;
#endif

// If compressed klass pointers then use narrowKlass.
typedef uint32_t narrowKlass;

class CompressedKlassPointers : public AllStatic {
  friend class VMStructs;
  friend class ArchiveBuilder;

  // A dense representation of values one often loads in quick succession, in order to fold
  // all of them into a single load:
  // - UseCompactObjectHeaders and UseCompressedClassPointers flags
  // - encoding base and encoding shift
  // We can encode everything (including the encoding base) in a 64-bit word. The encoding
  // base will always be page aligned, so we have a 12-bit alignment shadow to store the rest
  // of the data.

  static uintptr_t _config;
  static constexpr int useCompactObjectHeadersShift = 0;
  static constexpr int useCompressedClassPointersShift = 1;
  static constexpr int encodingShiftShift = 2;
  static constexpr int encodingShiftWidth = 5;
  static constexpr int baseAddressMask = ~right_n_bits(12);

  static void set_encoding_base(address base);
  static void set_encoding_shift(int shift);
  static void set_use_compact_headers(bool b);
  static void set_use_compressed_class_pointers(bool b);

  // These members hold copies of encoding base and shift and only exist for SA (see vmStructs.cpp and
  // sun/jvm/hotspot/oops/CompressedKlassPointers.java)
  static address _base_copy;
  static int _shift_copy;

  // The decode/encode versions taking an explicit base are for the sole use of CDS
  // (see ArchiveBuilder).
  static inline Klass* decode_raw(narrowKlass v, address base);
  static inline Klass* decode_not_null(narrowKlass v, address base);
  static inline narrowKlass encode_not_null(Klass* v, address base);
  DEBUG_ONLY(static inline void verify_klass_pointer(const Klass* v, address base));

  static void print_mode_pd(outputStream* st);

public:

  // Given an address p, return true if p can be used as an encoding base.
  //  (Some platforms have restrictions of what constitutes a valid base
  //   address).
  static bool is_valid_base(address p);

  // Given an address range [addr, addr+len) which the encoding is supposed to
  //  cover, choose base, shift and range.
  //  The address range is the expected range of uncompressed Klass pointers we
  //  will encounter (and the implicit promise that there will be no Klass
  //  structures outside this range).
  static void initialize(address addr, size_t len);

  static void print_mode(outputStream* st);

  // The encoding base. Note: this is not necessarily the base address of the
  // class space nor the base address of the CDS archive.
  static inline address base() {
    return (address)(_config & baseAddressMask);
  }

  // End of the encoding range.
  static inline address  end() {
    return base() + KlassEncodingMetaspaceMax;
  }

  // Shift == LogKlassAlignmentInBytes (TODO: unify)
  static inline int shift() {
    return (_config >> encodingShiftShift) & right_n_bits(encodingShiftWidth);
  }

  static inline bool use_compact_object_headers()    { return (_config >> useCompactObjectHeadersShift) & 1; }
  static inline bool use_compressed_class_pointers() { return (_config >> useCompressedClassPointersShift) & 1; }

  static bool is_null(Klass* v)      { return v == nullptr; }
  static bool is_null(narrowKlass v) { return v == 0; }

  static inline Klass* decode_raw(narrowKlass v);
  static inline Klass* decode_not_null(narrowKlass v);
  static inline Klass* decode(narrowKlass v);
  static inline narrowKlass encode_not_null(Klass* v);
  static inline narrowKlass encode(Klass* v);

  DEBUG_ONLY(static inline void verify_klass_pointer(const Klass* v));
  DEBUG_ONLY(static inline void verify_narrow_klass_pointer(narrowKlass v);)

};

#endif // SHARE_OOPS_COMPRESSEDOOPS_HPP
