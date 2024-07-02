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

#ifndef SHARE_OOPS_OOPMAPLUTABLE_HPP
#define SHARE_OOPS_OOPMAPLUTABLE_HPP

#include "memory/allStatic.hpp"
#include "oops/compressedKlass.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class OopMapBlock;
class Klass;
class outputStream;

class OopMapLUTable : public AllStatic {

  struct Entry {
    uint8_t _offset;
    uint8_t _count;
  };

#ifdef ASSERT
  static volatile uint64_t hits_0;
  static volatile uint64_t hits_non_0;
  static volatile uint64_t misses;
  static void add_to_statistics(int rc);
  static void verify_after_decode(Klass* k, int rc, const OopMapBlock* cached);
#endif

  static Entry* _entries;

  static unsigned max_index() {
    return (unsigned) nth_bit(CompressedKlassPointers::narrow_klass_pointer_bits());
  }

  static inline Entry encode_block(unsigned num_entries, const OopMapBlock* first);
  static inline int decode_block(const Entry* e, OopMapBlock* b);

public:

  static void initialize();

  static inline void set_entry(const Klass* k, unsigned num_entries, const OopMapBlock* first);
  static inline void set_entry(narrowKlass k, unsigned num_entries, const OopMapBlock* first);

  // returns:
  // 0 if the klass has no oopmap entry
  // 1 if the klass has just one entry, and the information in the table was sufficient
  //   to restore it. In that case, b contains the one and only block.
  // 2 if the klass has multiple entries, or a single entry that does not fit into the table.
  //   In that case, caller needs to query the real oopmap.
  static inline int get_entry(const Klass* k, OopMapBlock* b);
  static inline int get_entry(narrowKlass k, OopMapBlock* b);

#ifdef ASSERT
  static void print_statistics(outputStream* st);
#endif

};

class KlassSizeLUTable : public AllStatic {

#ifdef ASSERT
  static void verify_after_decode(Klass* k, jint cached);
#endif

  static jint* _entries;

  static unsigned max_index() {
    return (unsigned) nth_bit(CompressedKlassPointers::narrow_klass_pointer_bits());
  }

public:

  static void initialize();

  static inline void set_entry(const Klass* k, jint value);
  static inline void set_entry(narrowKlass k, jint value);

  static inline jint get_entry(const Klass* k);
  static inline jint get_entry(narrowKlass k);

};

#endif // SHARE_OOPS_OOPMAPLUTABLE_HPP
