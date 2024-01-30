/*
 * Copyright (c) 2022, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_RUNTIME_LOCKSTACK_INLINE_HPP
#define SHARE_RUNTIME_LOCKSTACK_INLINE_HPP

#include "runtime/lockStack.hpp"

#include "memory/iterator.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/stackWatermark.hpp"
#include "runtime/stackWatermarkSet.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/placeholderSynchronizer.hpp"

inline int LockStack::to_index(uint32_t offset) {
  assert(is_aligned(offset, oopSize), "Bad alignment: %u", offset);
  return (offset - lock_stack_base_offset) / oopSize;
}

JavaThread* LockStack::get_thread() const {
  char* addr = reinterpret_cast<char*>(const_cast<LockStack*>(this));
  return reinterpret_cast<JavaThread*>(addr - lock_stack_offset);
}

inline bool LockStack::is_full() const {
  return to_index(_top) == CAPACITY;
}

inline bool LockStack::can_push(int n) const {
  return (CAPACITY - to_index(_top)) >= n;
}

inline bool LockStack::is_owning_thread() const {
  Thread* current = Thread::current();
  if (current->is_Java_thread()) {
    JavaThread* thread = JavaThread::cast(current);
    bool is_owning = &thread->lock_stack() == this;
    assert(is_owning == (get_thread() == thread), "is_owning sanity");
    return is_owning;
  }
  return false;
}

inline void LockStack::push(oop o) {
  verify("pre-push");
  assert(oopDesc::is_oop(o), "must be");
  assert(!contains(o), "entries must be unique");
  assert(!is_full(), "must have room");
  assert(_base[to_index(_top)] == nullptr, "expect zapped entry");
  _base[to_index(_top)] = o;
  _top += oopSize;
  verify("post-push");
}

inline oop LockStack::bottom() const {
  assert(to_index(_top) > 0, "must contain an oop");
  return _base[0];
}

inline oop LockStack::top() {
  assert(to_index(_top) > 0, "may only call with at least one element in the stack");
  return _base[to_index(_top) - 1];
}

inline bool LockStack::is_empty() const {
  return to_index(_top) == 0;
}

inline bool LockStack::is_recursive(oop o) const {
  if (LockingMode != LM_PLACEHOLDER) {
    return false;
  }

  assert(contains(o), "entries must exist");
  int end = to_index(_top);
  for (int i = 1; i < end; i++) {
    if (_base[i - 1] == o && _base[i] == o) {
      return true;
    }
  }

  return false;
}

inline bool LockStack::try_recursive_enter(oop o) {
  if (LockingMode != LM_PLACEHOLDER) {
    return false;
  }

  assert(!is_full(), "precond");

  int end = to_index(_top);
  if (end == 0 || _base[end - 1] != o) {
    // Topmost oop does not match o.
    return false;
  }

  _base[end] = o;
  _top += oopSize;
  return true;
}

inline bool LockStack::try_recursive_exit(oop o) {
  if (LockingMode != LM_PLACEHOLDER) {
    return false;
  }

  assert(contains(o), "entries must exist");

  int end = to_index(_top);
  if (end <= 1 || _base[end - 1] != o ||  _base[end - 2] != o) {
    // The two topmost oops do not match o.
    return false;
  }

  _top -= oopSize;
  DEBUG_ONLY(_base[to_index(_top)] = nullptr;)
  return true;
}

inline size_t LockStack::remove(oop o) {
  verify("pre-remove");
  assert(contains(o), "entry must be present: " PTR_FORMAT, p2i(o));

  int end = to_index(_top);
  int inserted = 0;
  for (int i = 0; i < end; i++) {
    if (_base[i] != o) {
      if (inserted != i) {
        _base[inserted] = _base[i];
      }
      inserted++;
    }
  }

#ifdef ASSERT
  for (int i = inserted; i < end; i++) {
    _base[i] = nullptr;
  }
#endif

  uint32_t removed = end - inserted;
  _top -= removed * oopSize;
  assert(!contains(o), "entry must have been removed: " PTR_FORMAT, p2i(o));
  verify("post-remove");
  return removed;
}

inline bool LockStack::contains(oop o) const {
  assert(o != nullptr, "Catch me!");
  verify("pre-contains");

  // Can't poke around in thread oops without having started stack watermark processing.
  assert(StackWatermarkSet::processing_started(get_thread()), "Processing must have started!");

  int end = to_index(_top);
  for (int i = end - 1; i >= 0; i--) {
    if (_base[i] == o) {
      verify("post-contains");
      return true;
    }
  }
  verify("post-contains");
  return false;
}

inline void LockStack::oops_do(OopClosure* cl) {
  verify("pre-oops-do");
  int end = to_index(_top);
  for (int i = 0; i < end; i++) {
    cl->do_oop(&_base[i]);
  }
  verify("post-oops-do");
}

inline void OMCache::set_monitor(ObjectMonitor *monitor) {
  const int end = OMCacheSize - 1;
  if (end < 0) {
    return;
  }

  oop obj = monitor->object_peek();
  assert(obj != nullptr, "must be alive");
  assert(monitor == PlaceholderSynchronizer::read_monitor(JavaThread::current(), obj), "must be exist in table");

  oop cmp_obj = obj;
  for (int i = 0; i < end; ++i) {
    if (_oops[i] == cmp_obj ||
        _monitors[i] == nullptr ||
        _monitors[i]->is_being_async_deflated()) {
      _oops[i] = obj;
      _monitors[i] = monitor;
      return;
    }
    // Remember Most Recent Values
    oop tmp_oop = obj;
    ObjectMonitor* tmp_mon = monitor;
    // Set next pair to the next most recent
    obj = _oops[i];
    monitor = _monitors[i];
    // Store most recent values
    _oops[i] = tmp_oop;
    _monitors[i] = tmp_mon;
  }
  _oops[end] = obj;
  _monitors[end] = monitor;
}

inline ObjectMonitor* OMCache::get_monitor(oop o) {
  for (int i = 0; i < OMCacheSize; ++i) {
    if (_oops[i] == o) {
      assert(_monitors[i] != nullptr, "monitor must exist");
      if (_monitors[i]->is_being_async_deflated()) {
        // Bad monitor
        // Shift down rest
        for (; i < OMCacheSize - 1; ++i) {
          _oops[i] = _oops[i + 1];
          _monitors[i] =  _monitors[i + 1];
        }
        // i == CAPACITY - 1
        _oops[i] = nullptr;
        _monitors[i] = nullptr;
        return nullptr;
      }
      return _monitors[i];
    }
  }
  return nullptr;
}

inline void OMCache::clear() {
  for (size_t i = 0 , r = 0; i < CAPACITY; ++i) {
    _oops[i] = nullptr;
    _monitors[i] = nullptr;
  }
}

#endif // SHARE_RUNTIME_LOCKSTACK_INLINE_HPP
