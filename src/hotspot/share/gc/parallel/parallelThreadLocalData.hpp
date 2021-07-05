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

#ifndef SHARE_GC_PARALLEL_PARALLELTHREADLOCALDATA_HPP
#define SHARE_GC_PARALLEL_PARALLELTHREADLOCALDATA_HPP

#include "gc/shared/forwardTable.hpp"
#include "runtime/thread.hpp"

class ParallelThreadLocalData {
private:
  BasicForwardTableAllocator* _allocator;

  ParallelThreadLocalData() : _allocator(NULL) {}

  static ParallelThreadLocalData* data(Thread* thread) {
    return thread->gc_data<ParallelThreadLocalData>();
  }

public:
  static void create(Thread* thread) {
    new (data(thread)) ParallelThreadLocalData();
  }

  static void destroy(Thread* thread) {
    // Nothing to do
  }

  static BasicForwardTableAllocator* allocator(Thread* thread) {
    BasicForwardTableAllocator* allocator = data(thread)->_allocator;
    if (allocator == nullptr) {
      allocator = new BasicForwardTableAllocator();
      data(thread)->_allocator = allocator;
    }
    return allocator;
  }

  static void reset_allocator(Thread* thread) {
    BasicForwardTableAllocator* allocator = data(thread)->_allocator;
    if (allocator != nullptr) {
      allocator->reset();
    }
  }
};

STATIC_ASSERT(sizeof(ParallelThreadLocalData) <= sizeof(GCThreadLocalData));

#endif // SHARE_GC_PARALLEL_PARALLELTHREADLOCALDATA_HPP
