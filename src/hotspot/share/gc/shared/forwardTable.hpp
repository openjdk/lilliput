/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHARED_FORWARDTABLE_HPP
#define SHARE_GC_SHARED_FORWARDTABLE_HPP

#include "memory/allocation.hpp"
#include "memory/arena.hpp"
#include "oops/oopsHierarchy.hpp"

template <typename CONFIG, MEMFLAGS F>
class ConcurrentHashTable;

class ForwardTableAllocator : public CHeapObj<mtGC> {
public:
  virtual void* allocate(size_t size) = 0;
  virtual void free(void* memory, size_t size) = 0;
  virtual void reset() = 0;
};

class BasicForwardTableAllocator : public ForwardTableAllocator {
private:
  Arena _arena;
public:
  BasicForwardTableAllocator() : _arena(Arena(mtGC)) {}

  void* allocate(size_t size) {
    return _arena.Amalloc_4(size);
  }

  void free(void* memory, size_t size) {
    _arena.Afree(memory, size);
  }

  void reset() {
    _arena.destruct_contents();
  }
};

class ForwardTableValue {
private:
  const HeapWord* const _object;
  const HeapWord* const _forwardee;

public:
  ForwardTableValue(const HeapWord* const object, const HeapWord* const forwardee) : _object(object), _forwardee(forwardee) {}

  inline const HeapWord* const object() const { return _object; }
  inline const HeapWord* const forwardee() const { return _forwardee; }
};

class ForwardTableConfig : public StackObj {
public:
  using Value = ForwardTableValue;
  static uintx get_hash(Value const& value, bool* is_dead);
  static void* allocate_node(void* context, size_t size, Value const& value) {
    return reinterpret_cast<ForwardTableAllocator*>(context)->allocate(size);
  }
  static void free_node(void* context, void* memory, Value const& value) {
    // We only ever free all memory all-at-once.
  }
};

class ForwardTable : public CHeapObj<mtGC> {
private:
  ConcurrentHashTable<ForwardTableConfig, mtGC>* const _table;
  ForwardTableAllocator* const _allocator;
public:
  ForwardTable(ForwardTableAllocator* const allocator);

  void clear();
  oop forward_to(oop from, oop to);
  oop forwardee(oop from) const;
};

#endif // SHARE_GC_SHARED_FORWARDABLE_HPP
