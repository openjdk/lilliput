/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_UTILITIES_ADDRESSSTABLEARRAY_HPP
#define SHARE_UTILITIES_ADDRESSSTABLEARRAY_HPP

#include "memory/allocation.hpp"
#include "memory/virtualspace.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/freeList.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

// An growable array of homogenous things, living in a pre-reserved address range
//  (and hence ultimately limited in size).
// It does not own the reserved memory range (does get handed in) but does its own
//  on-demand committing/uncommitting.

template <class T>
class AddressStableArray : public CHeapObj<mtInternal> {
  STATIC_ASSERT(sizeof(T) >= sizeof(T*));              // (1)
  STATIC_ASSERT(is_aligned(sizeof(T), sizeof(T*)));    // (2)

  // Underlying address range [_elements, _elements + _max_capacity). Gets
  // handed in from outside. Note that we commit and uncommit page wise and
  // therefore the start pointer has to be page aligned. End does not have to
  // be (we can curtail _max_capacity beyond what the last page could
  // accomodate) but nothing else should live in the remainder of the last page.
  T* const _elements;
  const uintx _max_capacity;      // max number of slots

  uintx _capacity;                // number of slots committed
  uintx _used;                    // number of slots allocated

  static uintx capacity_of(size_t bytes)  { return bytes / sizeof(T); }

  T* at(uintx idx) const                  { return _elements + idx; }

  static size_t bytes_needed(uintx n)               { return sizeof(T) * n; }
  static size_t page_align(size_t s)                { return align_up(s, os::vm_page_size()); }
  static size_t bytes_needed_page_aligned(uintx n)  { return page_align(bytes_needed(n)); }
  static size_t pages_needed(uintx n)               { return bytes_needed_page_aligned(n) / os::vm_page_size(); }

  // Enlarge committed capacity
  void enlarge_capacity(uintx min_needed_capacity);

  void check_index(uintx index) const {
    assert(index < _used, "invalid index (" UINTX_FORMAT ")", index);
  }

public:

  AddressStableArray(T* elements, uintx initial_capacity, uintx max_capacity) :
    _elements(elements),
    _max_capacity(max_capacity),
    _capacity(0), _used(0)
  {
    assert(_elements != NULL, "sanity");
    assert(is_aligned(_elements, os::vm_page_size()), "start address must be page aligned");
    assert(_max_capacity > 0, "empty range?");
    assert(_max_capacity >= initial_capacity, "sanity");
    if ((initial_capacity) > 0) {
      enlarge_capacity(initial_capacity);
    }
  }

  bool contains(const T* v) const {
    return _elements <= v && (_elements + _used) > v;
  }

  T* allocate() {
    if (_used == _capacity) {
      if (_capacity == _max_capacity) {
        return NULL;
      }
      enlarge_capacity(_capacity + 1);
    }
    assert(_used < _capacity, "enlarge failed?");
    T* p = at(_used);
    _used ++;
    return p;
  }

  size_t reserved_bytes() const {
    return bytes_needed_page_aligned(_max_capacity);
  }

  size_t committed_bytes() const {
    return bytes_needed_page_aligned(_capacity);
  }

  uintx capacity() const { return _capacity; }
  uintx used() const     { return _used; }

  // uncommit the underlying memory and reset the commit watermark.
  // (note: range stays reserved)
  void uncommit();

  DEBUG_ONLY(void verify() const;)
  void print_on(outputStream* st) const;

  // Base address (exposed to set NMT cat; TODO: this is annoying, should be done better
  const T* base() const { return _elements; }

}; // AddressStableArray

// Same, but with freelist supporting deallocation
template <class T>
class AddressStableArrayWithFreeList : public CHeapObj<mtInternal> {

  typedef AddressStableArray<T> ArrayType;
  typedef FreeList<T> FreeListType;

  ArrayType _array;
  FreeListType _freelist;

public:

  AddressStableArrayWithFreeList(T* elements, uintx initial_capacity, uintx max_capacity) :
    _array(elements, initial_capacity, max_capacity),
    _freelist()
  {}

  T* allocate() {
    T* p = _freelist.take_top();
    if (p == NULL) {
      p = _array.allocate();
    }
    return p;
  }

  void deallocate(T* t) {
    _freelist.prepend(t);
  }

  // Add all elements to freelist and empties out the donor list
  void bulk_deallocate(FreeListType& list) {
    _freelist.prepend_list(list);
  }

  // If all elements are free (there are no outstanding elements, all have been
  // returned to the freelist), uncommit the underlying memory range and reset the
  // freelist. Returns true if that worked, false otherwise.
  bool try_uncommit();

  size_t committed_bytes() const         { return _array.committed_bytes(); }
  bool contains(const T* v) const        { return _array.contains(v); }

  // Returns the committed capacity, in number of elements
  uintx capacity() const                  { return _array.capacity(); }
  // Returns the number of outstanding allocations
  uintx used() const                      { return _array.used() - free(); }
  // Returns the number of elements in the freelist waiting for reuse
  uintx free() const                      { return _freelist.count(); }

  DEBUG_ONLY(void verify(bool paranoid = false) const;)
  void print_on(outputStream* st) const;

};

#endif // SHARE_UTILITIES_ADDRESSSTABLEARRAY_HPP
