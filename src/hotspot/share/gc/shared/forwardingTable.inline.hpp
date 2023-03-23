/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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

#ifndef SHARE_GC_SHARED_FORWARDINGTABLE_INLINE_HPP
#define SHARE_GC_SHARED_FORWARDINGTABLE_INLINE_HPP

#include "gc/shared/forwardingTable.hpp"
#include "utilities/concurrentHashTable.inline.hpp"

inline uintx murmur3_hash(uintx k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdLLU;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53LLU;
  k ^= k >> 33;
  return k;
}

uintx ForwardingTableConfig::get_hash(ForwardingTableValue const& value, bool* is_dead) {
  *is_dead = false;
  return murmur3_hash(reinterpret_cast<uintx>(value.object()));
}

class ForwardingTableLookup : public StackObj {
private:
  const HeapWord* const _object;
public:
  inline ForwardingTableLookup(const HeapWord* const object) : _object(object) {}

  inline uintx get_hash() const {
    return murmur3_hash(reinterpret_cast<uintx>(_object));
  }

  inline bool equals(const ForwardingTableValue* const value, bool* is_dead) {
    *is_dead = false;
    return _object == value->object();
  }
};

class ForwardingTableFound : public StackObj {
private:
  ForwardingTableValue* _value;
public:
  void operator()(ForwardingTableValue* const value) {
    _value = value;
  }

  ForwardingTableValue* value() const {
    return _value;
  }
};

inline oop ForwardingTable::forward_to(oop from, oop to) {
  assert(to != NULL, "Must not forwarde to NULL");
  //tty->print_cr("forward_to: thread: " PTR_FORMAT ", from: " PTR_FORMAT ", to: " PTR_FORMAT, p2i(Thread::current()), p2i(cast_from_oop<HeapWord*>(from)), p2i(cast_from_oop<HeapWord*>(to)));
  ForwardingTableLookup lookup(cast_from_oop<HeapWord*>(from));
  ForwardingTableValue value(cast_from_oop<HeapWord*>(from), cast_from_oop<HeapWord*>(to));
  ForwardingTableFound found;
  bool grow;
  oop result;
  if (_table.insert_get(Thread::current(), lookup, value, found, &grow)) {
    assert(_table.get(Thread::current(), lookup, found), "should be inserted now, thread: " PTR_FORMAT ", from: " PTR_FORMAT ", found: " PTR_FORMAT ", forwardee: " PTR_FORMAT, p2i(Thread::current()), p2i(cast_from_oop<HeapWord*>(from)), p2i(found.value()->forwardee()), p2i(cast_from_oop<HeapWord*>(to)));
    assert(found.value()->forwardee() == cast_from_oop<HeapWord*>(to), "forwardee must be ours");
    result = nullptr;
  } else {
    found.value()->set_forwardee(cast_from_oop<HeapWord*>(to));
    result = nullptr;
    //result = cast_to_oop(found.value()->forwardee());
  }
  if (grow) {
    _table.grow(Thread::current());
  }
  return result;
}

inline oop ForwardingTable::forwardee(oop from) {
  ForwardingTableLookup lookup(cast_from_oop<HeapWord*>(from));
  ForwardingTableFound found;
  if (_table.get(Thread::current(), lookup, found)) {
    // tty->print_cr("forwardee: " PTR_FORMAT, p2i(found.value()->forwardee()));
    return cast_to_oop(found.value()->forwardee());
  } else {
    return nullptr;
  }
}

#endif // SHARE_GC_SHARED_FORWARDINGTABLE_INLINE_HPP
