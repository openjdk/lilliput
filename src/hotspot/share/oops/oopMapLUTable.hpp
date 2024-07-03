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

#ifndef SHARE_OOPS_OOPMAPLUTABLE_HPP
#define SHARE_OOPS_OOPMAPLUTABLE_HPP

#include "memory/allStatic.hpp"
#include "oops/compressedKlass.hpp"
#include "oops/klass.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class OopMapBlock;
class Klass;
class outputStream;

class OopMapLUTable : public AllStatic {

  static constexpr uint32_t invalid_entry = right_n_bits(32);

  class InstanceKlassEntry {

    // a 32-bit value:
    // Bit 31: 1 array 0 object
    // For arrays:
    // value is layouthelper (so msb is 1). Kind can only be either
    //  TypeArrayKlassKind or ObjArrayKlassKind and it is deduced from layouthelper.
    // For objects:
    // 31-16: a condensed form of layouthelper. Upper bit is 0, since lh for objects is zero.
    //  Otherwise, if lh value <= 0xfffe, its lh, otherwise its not representable.
    // 15-0: a condensed form of the first nonstatic OopMapBlock:
    //       15 [count][offset] 0
    // with 0x0 meaning "no entries" and 0xFFFF meaning "not representable"

    static constexpr unsigned reserved_bits = 1; // "is_array" bit, must be 0

    static constexpr unsigned kind_bits = 3;
    static constexpr unsigned kind_offset = 32 - reserved_bits - kind_bits;

    static constexpr unsigned lh_bits = 16 - reserved_bits - kind_bits;
    static constexpr unsigned lh_offset = 16;
    static constexpr unsigned lh_is_not_representable = right_n_bits(lh_bits);

    static constexpr unsigned omb_bits = 16;
    static constexpr unsigned omb_offset = 0;
    static constexpr unsigned omb_is_not_representable = right_n_bits(omb_bits);
    static constexpr unsigned omb_is_empty = 0;

    const uint32_t _v;

    STATIC_ASSERT(reserved_bits + kind_bits + lh_bits + omb_bits ==
                  sizeof(_v) * BitsPerByte);

    unsigned get_kind_bits() const {
      return (_v >> kind_offset) & right_n_bits(kind_bits);
    }

    unsigned get_lh_bits() const {
      return (_v >> lh_offset) & right_n_bits(lh_bits);
    }

    unsigned get_omb_bits() const {
      return (_v >> omb_offset) & right_n_bits(omb_bits);
    }

    static inline uint32_t build_from(const InstanceKlass* ik);

  public:

    inline InstanceKlassEntry(uint32_t v);
    inline InstanceKlassEntry(const InstanceKlass* ik);

    inline Klass::KlassKind get_kind() const;

    // if layout helper is representable, returns true and the lh value, false otherwise
    inline bool get_layouthelper(int& out) const;

    // returns:
    // 0 if there is no oopmap
    // 1 if it has only one entry, and it was representable. In that case, b contains the entry.
    // 2 if it had more than one entries, or only one but it was unrepresentable.
    inline int get_oopmapblock(OopMapBlock* b) const;

#ifdef ASSERT
    void verify_against(const InstanceKlass* k) const;
#endif

    uint32_t value() const { return _v; }

  }; // InstanceKlassEntry

  class ArrayKlassEntry {
    const uint32_t _v;
    static inline uint32_t build_from(const ArrayKlass* ak);
  public:
    inline ArrayKlassEntry(uint32_t v);
    inline ArrayKlassEntry(const ArrayKlass* ak);
    int get_layouthelper() const { return (int) _v; }
    inline Klass::KlassKind get_kind() const;

#ifdef ASSERT
    void verify_against(const ArrayKlass* k) const;
#endif

    uint32_t value() const { return _v; }
  }; // ArrayKlassEntry


  static uint32_t* _entries;

  static inline unsigned num_entries();

  static inline uint32_t* entry_for_klass(const Klass* k);

  static bool is_array(uint32_t e) { return e >> 31 == 1; }

#ifdef ASSERT
  // statistics
#define STATS_DO(f)     \
  f(hits_omb_zero)      \
  f(hits_omb_non_zero)  \
  f(misses_omb)         \
  f(hits_lh)            \
  f(misses_lh)
#define XX(xx) static void inc_##xx();
  STATS_DO(XX)
#undef XX
#endif // ASSERT

public:

  static void initialize();

  static inline void set_entry(const InstanceKlass* k);
  static inline void set_entry(const ArrayKlass* k);

  // returns:
  // 0 if the klass has no oopmap entry
  // 1 if the klass has just one entry, and the information in the table was sufficient
  //   to restore it. In that case, b contains the one and only block.
  // 2 if the klass has multiple entries, or a single entry that does not fit into the table.
  //   In that case, caller needs to query the real oopmap.
  static inline int try_get_oopmapblock(const InstanceKlass* k, OopMapBlock* b);

  // returns:
  // true if we can get the lh. lh is in out, then
  // false if we cannot get the lh.
  static inline bool try_get_layouthelper(const Klass* k, int& out);

  static inline bool try_get_kind(const Klass* k, int& out);

#ifdef ASSERT
  static void print_statistics(outputStream* out);
#endif // ASSERT

};


#endif // SHARE_OOPS_OOPMAPLUTABLE_HPP
