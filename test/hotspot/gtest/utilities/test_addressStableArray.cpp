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

 */

#include "precompiled.hpp"

#include "runtime/os.hpp"
#include "utilities/addressStableArray.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include "unittest.hpp"
#include "testutils.hpp"


// helper, calc expected committed range size for a given element number
template <class T>
static size_t expected_committed_bytes(uintx elems) {
  return align_up(sizeof(T) * elems, os::vm_page_size());
}

// capacity is difficult to predict, since it increases in larger steps and the increase
// depends on page size and cap increase steps. I just do some range checks here
#define ASSERT_CAP_EQ(array, n)                                       \
  ASSERT_EQ(array.capacity(), (uintx)n);                              \
  ASSERT_EQ(array.committed_bytes(), expected_committed_bytes<T>(n));

// Range check for cap. Note range is including on both ends ([])
#define ASSERT_CAP_IN_RANGE(array, n1, n2)                             \
	ASSERT_GE(array.capacity(), (uintx)n1);                              \
  ASSERT_LE(array.capacity(), (uintx)n2);                              \
  ASSERT_GE(array.committed_bytes(), expected_committed_bytes<T>(n1)); \
  ASSERT_LE(array.committed_bytes(), expected_committed_bytes<T>(n2)); \

#define ASSERT_USED(array, n)   ASSERT_EQ(array.used(), (uintx)n)
#define ASSERT_FREE(array, n)   ASSERT_EQ(array.free(), (uintx)n)

#define ASSERT_USED_FREE(array, used, free) \
	  ASSERT_USED(array, used);               \
	  ASSERT_FREE(array, free);

// Test expectation that heap is completely filled. Stats should reflect that.
// Allocation should return NULL and leave stats unchanged.
#define ASSERT_ARRAY_IS_FULL(a1)        \
		ASSERT_USED_FREE(a1, max_size, 0);  \
		ASSERT_CAP_EQ(a1, max_size);        \
		ASSERT_EQ(a1.allocate(), (T*)NULL); \
		ASSERT_USED_FREE(a1, max_size, 0);  \
		ASSERT_CAP_EQ(a1, max_size);

// Allocate from array a single element, and if not null, stamp it
template <class T> T* allocate_from_array(AddressStableHeap<T>& a) {
  T* p = a.allocate();
  if (p != NULL) {
    GtestUtils::mark_range(p, sizeof(T));
  }
  return p;
}

// Return an element to the array. Before doing that, check stamp.
template <class T> void deallocate_to_array(AddressStableHeap<T>& a, T* elem) {
  ASSERT_TRUE(GtestUtils::check_range(elem, sizeof(T)));
  a.deallocate(elem);
}

template <class T> class SimpleArray {
  T* const _v;
public:
  SimpleArray(uintx size) : _v(NEW_C_HEAP_ARRAY(T, size, mtTest)) {
    ::memset(_v, 0, sizeof(T) * size);
  }
  ~SimpleArray() { FREE_C_HEAP_ARRAY(T, _v); }
  T* v() { return _v; }
};

template <class T>
static void test_fill_empty_repeat(uintx initialsize, uintx max_size) {
  AddressStableHeap<T> a1(initialsize, max_size);
  ASSERT_USED_FREE(a1, 0, 0);
  ASSERT_CAP_IN_RANGE(a1, initialsize, max_size);

  SimpleArray<T*> elems_holder(max_size);
  T** elems = elems_holder.v();

  DEBUG_ONLY(a1.verify();)

  uintx expected_used = 0;
  uintx expected_free = 0;
  for (int cycle = 0; cycle < 3; cycle ++) {

    // fill completely
    for (uintx i = 0; i < max_size; i ++) {
      T* p = allocate_from_array(a1);
      ASSERT_NE(p, (T*)NULL);
      elems[i] = p;
      expected_used ++;
      if (expected_free > 0) {
        expected_free --;
      }
      // used increases, cap increases in spurts,
      // free stays 0 since freelist gets only filled after deallocs,
      ASSERT_USED_FREE(a1, expected_used, expected_free);
      ASSERT_CAP_IN_RANGE(a1, expected_used + expected_free, max_size);
    }

    ASSERT_EQ(expected_used, max_size);
    ASSERT_EQ(expected_free, (uintx)0);

    // We should be right at the edge now
    ASSERT_ARRAY_IS_FULL(a1)

    // Return all elements
    for (uintx i = 0; i < max_size; i ++) {
      deallocate_to_array(a1, elems[i]);
      expected_used --;
      expected_free ++;
      // used, free change, cap stays at max
      ASSERT_USED_FREE(a1, expected_used, expected_free);
      ASSERT_CAP_EQ(a1, max_size);
    }

    ASSERT_EQ(expected_used, (uintx)0);
    ASSERT_EQ(expected_free, max_size);

    DEBUG_ONLY(a1.verify();)
  }
}

template <class T>
static void test_fill_empty_randomly(uintx initialsize, uintx max_size) {
  AddressStableHeap<T> a1(initialsize, max_size);
  ASSERT_USED_FREE(a1, 0, 0);
  ASSERT_CAP_IN_RANGE(a1, initialsize, max_size);

  SimpleArray<T*> elems_holder(max_size);
  T** elems = elems_holder.v();

  DEBUG_ONLY(a1.verify();)

  // randomly alloc or dealloc a number of times and observe stats
  uintx expected_used = 0;
  uintx expected_free = 0;

  for (uintx iter = 0; iter < MAX2(max_size * 10, (uintx)256); iter ++) {
    const int idx = os::random() % max_size;
    if (elems[idx] == NULL) {
      T* p = allocate_from_array(a1);
      ASSERT_NE(p, (T*)NULL);
      elems[idx] = p;
      expected_used ++;
      if (expected_free > 0) {
        expected_free --;
      }
    } else {
      deallocate_to_array(a1, elems[idx]);
      elems[idx] = NULL;
      expected_free ++;
      expected_used --;
    }
    ASSERT_USED_FREE(a1, expected_used, expected_free);
    ASSERT_CAP_IN_RANGE(a1, expected_used + expected_free, max_size);
    if ((iter % 256) == 0) {
      DEBUG_ONLY(a1.verify(false);)
    }
  }
  DEBUG_ONLY(a1.verify(true);)

  // Now allocate fully
  for (uintx i = 0; i < max_size; i++) {
    if (elems[i] == 0) {
      T* p = allocate_from_array(a1);
      ASSERT_NE(p, (T*)NULL);
      elems[i] = p;
      expected_used ++;
      if (expected_free > 0) {
        expected_free --;
      }
      ASSERT_USED_FREE(a1, expected_used, expected_free);
      ASSERT_CAP_IN_RANGE(a1, expected_used + expected_free, max_size);
    }
  }

  // We should be right at the edge now
  ASSERT_ARRAY_IS_FULL(a1)

  DEBUG_ONLY(a1.verify(true);)
}

template <class T>
static void test_commit_and_uncommit(uintx initialsize, uintx max_size) {
  AddressStableHeap<T> a1(initialsize, max_size);
  ASSERT_USED_FREE(a1, 0, 0);
  ASSERT_CAP_IN_RANGE(a1, initialsize, max_size);

  SimpleArray<T*> elems_holder(max_size);
  T** elems = elems_holder.v();

  for (int cycle = 0; cycle < 5; cycle ++) {

    // fill completely
    for (uintx i = 0; i < max_size; i ++) {
      T* p = allocate_from_array(a1);
      ASSERT_NE(p, (T*)NULL);
      elems[i] = p;
      // used increases, cap increases in spurts,
      // free stays 0 since freelist gets only filled after deallocs,
      ASSERT_USED_FREE(a1, i + 1, 0);
      ASSERT_CAP_IN_RANGE(a1, i + 1, max_size);
    }

    ASSERT_ARRAY_IS_FULL(a1);

    // uncommiting should fail, since elements are not free, and should
    // leave array unchanged
    ASSERT_FALSE(a1.try_uncommit());
    ASSERT_ARRAY_IS_FULL(a1);

    // release all but one
    // fill array completely
    for (uintx i = 0; i < max_size - 1; i ++) {
      deallocate_to_array(a1, elems[i]);
    }

    // capacity is max, but almost all are free now:
    ASSERT_USED_FREE(a1, 1, max_size - 1);
    ASSERT_CAP_EQ(a1, max_size);

    // uncommiting should still fail, since elements are not free, and should
    // leave array unchanged
    ASSERT_FALSE(a1.try_uncommit());
    ASSERT_USED_FREE(a1, 1, max_size - 1);
    ASSERT_CAP_EQ(a1, max_size);

    // release the last element
    deallocate_to_array(a1, elems[max_size - 1]);

    // now all should be free, still fully committed though
    ASSERT_USED_FREE(a1, 0, max_size);
    ASSERT_CAP_EQ(a1, max_size);

    // release should work and reset the whole array
    ASSERT_TRUE(a1.try_uncommit());
    ASSERT_USED_FREE(a1, 0, 0);
    ASSERT_CAP_EQ(a1, 0);

    // a second release on an empty array should work too and be a noop
    ASSERT_TRUE(a1.try_uncommit());
    ASSERT_USED_FREE(a1, 0, 0);
    ASSERT_CAP_EQ(a1, 0);
  }
}

static const size_t max_memory = 10 * M; // a single test should not use more than that

#define xstr(s) str(s)
#define str(s) #s

#define TEST_single(T, function, initialsize, max_size)                         \
TEST_VM(AddressStableArray, function##_##T##_##initialsize##_##max_size)        \
{                                                                               \
	ASSERT_LT(expected_committed_bytes<T>(max_size), max_memory);                 \
	function<T>(initialsize, max_size);                                           \
}

#define TEST_all_functions(T, initialsize, max_size)                            \
  TEST_single(T, test_fill_empty_repeat, initialsize, max_size)                 \
  TEST_single(T, test_fill_empty_randomly, initialsize, max_size)               \
  TEST_single(T, test_commit_and_uncommit, initialsize, max_size)

#define TEST_all_functions_small_sizes(T)                                       \
		TEST_all_functions(T, 0, 1)                                                 \
    TEST_all_functions(T, 1, 1)                                                 \
    TEST_all_functions(T, 0, 100)                                               \
    TEST_all_functions(T, 10, 100)


// This we only execute for small types
#define TEST_all_functions_all_sizes(T)                                         \
		TEST_all_functions_small_sizes(T)                                           \
		TEST_all_functions(T, 0, 10000)                                             \
		TEST_all_functions(T, 1000, 10000)

struct s3 { void* p[3]; };

#ifndef _LP64
TEST_all_functions_all_sizes(uint32_t)
#endif

TEST_all_functions_all_sizes(uint64_t)
TEST_all_functions_all_sizes(s3)

// Some larger types

struct s216 { char p[216]; };

// almost, but not quite, a page (note: sizeof all types for AddressStableArray must be pointer aligned)
struct almost4k { char m[4096 - sizeof(intptr_t)]; };

// large
struct s64k { char m[64 * 1024]; };

TEST_all_functions_small_sizes(s216)
TEST_all_functions_small_sizes(almost4k)
TEST_all_functions_small_sizes(s64k)
