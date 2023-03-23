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

#ifndef SHARE_GC_SHARED_FORWARDINGTABLE_HPP
#define SHARE_GC_SHARED_FORWARDINGTABLE_HPP

#include "memory/allocation.hpp"
#include "memory/arena.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/concurrentHashTable.hpp"

// A data-structure that maps oops to their forwarded locations. It is used
// during full-GCs, which are sliding objects to the bottom of the heap (or some region),
// and would loose their header information when storing the forwardee in the
// object header.
//
// The implementation currently uses a generic concurrent hash-table. However, GC forwarding
// comes with several constraints which may be possible to exploit for more efficient
// implementation.
//
// - It is only used while mutators are paused.
// - Forwarding happens in three distinct phases:
//   - Forwardings are filled into the table. This is lots of inserts and some lookups
//   - At some point, calculating forward locations is complete, from there on, only
//     lookups are happening.
//   - At the end, the whole forwarding information can be released all at once.
// - Forwarding entries are never changed or deleted, except wholesale at the end.
// - Inserting forwardees happens in a somewhat linear fashion: GC threads scan the heap
//   (or regions of it) from bottom to top and insert forward locations for each reachable object.
// - Conversely, lookups during adjust-references phase happens in a random-access manner.
// - No two threads insert forwardings for the same object concurrently. Quite the contrary:
//   worker threads divide up the heap into sub-regions and each worker forwards only objects
//   that are in its assigned regions.
//
// The backing-store allocator of the forwarding table is using stupid malloc/free. This
// could and should be improved to use a sort of arena allocator, where each GC thread
// has its own arena to allocate from, which can be de-allocated wholesale at the end.

class ForwardingTableValue {
private:
  const HeapWord* const _object;
  const HeapWord*       _forwardee;

public:
  ForwardingTableValue(const HeapWord* const object, const HeapWord* const forwardee) : _object(object), _forwardee(forwardee) {}

  inline const HeapWord* const object() const { return _object; }
  inline const HeapWord* const forwardee() const { return _forwardee; }
  inline void set_forwardee(HeapWord* fwd) { _forwardee = fwd; }
};

class ForwardingTableConfig : public StackObj {
public:
  using Value = ForwardingTableValue;
  static inline uintx get_hash(Value const& value, bool* is_dead);
  static void* allocate_node(void* context, size_t size, Value const& value) {
    return NEW_C_HEAP_ARRAY(char, size, mtGC);
  }
  static void free_node(void* context, void* memory, Value const& value) {
    FREE_C_HEAP_ARRAY(char, memory);
  }
};

class ForwardingTable : public CHeapObj<mtGC> {
private:
  ConcurrentHashTable<ForwardingTableConfig, mtGC> _table;
public:
  ForwardingTable();

  void clear();
  inline oop forward_to(oop from, oop to);
  inline oop forwardee(oop from);
};

#endif // SHARE_GC_SHARED_FORWARDINGABLE_HPP
