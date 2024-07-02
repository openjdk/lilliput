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

inline OopMapLUTable::Entry OopMapLUTable::OopMapLUTable::encode_block(unsigned num_entries, const OopMapBlock* first) {
  Entry e;
  if (num_entries == 0) {
    e._count = e._offset = 0;
  } else if (num_entries > 1) {
    e._count = e._offset = 0xFF;
  } else {
    // Fit into uint8 each?
    unsigned count = first->count();
    int offset = first->offset();
    assert(count > 0, "Sanity");
    if (count <= 0xFF && offset < 0xFF) {
      e._count = (uint8_t) count;
      e._offset = (uint8_t) offset;
    }
  }
  return e;
}

inline int OopMapLUTable::decode_block(const Entry* e, OopMapBlock* b) {
  if (e->_count == 0 && e->_offset == 0) {
    return 0;
  } else if (e->_count == 0xFF && e->_offset == 0xFF) {
    return 2;
  } else {
    b->set_count(e->_count);
    b->set_offset(e->_offset);
    return 1;
  }
}


inline void OopMapLUTable::set_entry(const Klass* k, unsigned num_entries, const OopMapBlock* first) {
  const narrowKlass nk = CompressedKlassPointers::encode_not_null(const_cast<Klass*>(k));
  set_entry(nk, num_entries, first);
}

inline void OopMapLUTable::set_entry(narrowKlass nk, unsigned num_entries, const OopMapBlock* first) {
  assert(nk < max_index(), "Invalid index?");

  // what about concurrent class unloading and loading?
  _entries[nk] = encode_block(num_entries, first);
}

// returns:
// 0 if the klass has no oopmap entry
// 1 if the klass has just one entry, and the information in the table was sufficient
//   to restore it. In that case, b contains the one and only block.
// 2 if the klass has multiple entries, or a single entry that does not fit into the table.
//   In that case, caller needs to query the real oopmap.
inline int OopMapLUTable::get_entry(const Klass* k, OopMapBlock* b) {
  const narrowKlass nk = CompressedKlassPointers::encode_not_null(const_cast<Klass*>(k));
  return get_entry(nk, b);
}

inline int OopMapLUTable::get_entry(narrowKlass nk, OopMapBlock* b) {
  assert(nk < max_index(), "Invalid index?");
  int rc = decode_block(_entries + nk, b);
#ifdef ASSERT
  add_to_statistics(rc);
  verify_after_decode(CompressedKlassPointers::decode_not_null(nk), rc, b);
#endif
  return rc;
}


inline void KlassSizeLUTable::set_entry(const Klass* k, jint value) {
  const narrowKlass nk = CompressedKlassPointers::encode_not_null(const_cast<Klass*>(k));
  set_entry(nk, value);
}

inline void KlassSizeLUTable::set_entry(narrowKlass nk, jint value) {
  assert(nk < max_index(), "Invalid index?");
  // what about concurrent class unloading and loading?
  _entries[nk] = value;
}

inline jint KlassSizeLUTable::get_entry(const Klass* k) {
  const narrowKlass nk = CompressedKlassPointers::encode_not_null(const_cast<Klass*>(k));
  return get_entry(nk);
}

inline jint KlassSizeLUTable::get_entry(narrowKlass nk) {
  assert(nk < max_index(), "Invalid index?");
  jint rc = _entries[nk];
#ifdef ASSERT
  verify_after_decode(CompressedKlassPointers::decode_not_null(nk), rc);
#endif
  return rc;
}


#endif // SHARE_OOPS_OOPMAPLUTABLE_INLINE_HPP
