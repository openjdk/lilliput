/*
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

#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "runtime/lockStack.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/ostream.hpp"

LockStack::LockStack() :
        _base(UseHeavyMonitors ? NULL : NEW_C_HEAP_ARRAY(oop, INITIAL_CAPACITY, mtSynchronizer)),
        _limit(_base + INITIAL_CAPACITY),
        _current(_base) {
}

LockStack::~LockStack() {
  if (!UseHeavyMonitors) {
    FREE_C_HEAP_ARRAY(oop, _base);
  }
}

#ifndef PRODUCT
void LockStack::validate(const char* msg) const {
  assert(!UseHeavyMonitors, "never use lock-stack when fast-locking is disabled");
  for (oop* loc1 = _base; loc1 < _current - 1; loc1++) {
    for (oop* loc2 = loc1 + 1; loc2 < _current; loc2++) {
      assert(*loc1 != *loc2, "entries must be unique: %s", msg);
    }
  }
}
#endif

void LockStack::push(oop o) {
  validate("pre-push");
  assert(!contains(o), "entries must be unique");
  if (_current >= _limit) {
    // Grow stack.
    assert(_limit > _base, "invariant");
    size_t capacity = _limit - _base;
    size_t index = _current - _base;
    size_t new_capacity = capacity * 2;
    oop* new_stack = NEW_C_HEAP_ARRAY(oop, new_capacity, mtSynchronizer);
    for (size_t i = 0; i < index; i++) {
      *(new_stack + i) = *(_base + i);
    }
    FREE_C_HEAP_ARRAY(oop, _base);
    _base = new_stack;
    _limit = _base + new_capacity;
    _current = _base + index;
    assert(_current < _limit, "must fit after growing");
  }
  *_current = o;
  _current++;
  validate("post-push");
}

oop LockStack::pop() {
  validate("pre-pop");
  oop* new_loc = _current - 1;
  assert(new_loc < _current, "underflow, probably unbalanced push/pop");
  _current = new_loc;
  oop o = *_current;
  assert(!contains(o), "entries must be unique");
  validate("post-pop");
  return o;
}

void LockStack::remove(oop o) {
  validate("pre-remove");
  assert(contains(o), "entry must be present");
  for (oop* loc = _base; loc < _current; loc++) {
    //tty->print_cr("checking lockstack idx: " SIZE_FORMAT ", _stack[i]: " PTR_FORMAT, i, p2i(_stack[i]));
    if (*loc == o) {
      oop* last = _current - 1;
      for (; loc < last; loc++) {
        *loc = *(loc + 1);
      }
      _current--;
      break;
    }
  }
  assert(!contains(o), "entries must be unique: " PTR_FORMAT, p2i(o));
  validate("post-remove");
}

bool LockStack::contains(oop o) const {
  validate("pre-contains");
  bool found = false;
  size_t i = 0;
  size_t found_i = 0;
  for (oop* loc = _current - 1; loc >= _base; loc--) {
    //tty->print_cr("checking lockstack idx: " SIZE_FORMAT ", _stack[i]: " PTR_FORMAT, i, p2i(_stack[i]));
    if (*loc == o) {
#ifdef PRODUCT
      return true;
#else
      if (found) {
        for (oop* l = _base; l < _current; l++) {
          tty->print_cr(PTR_FORMAT ": " PTR_FORMAT, p2i(l), p2i(*l));
        }
      }
      assert(!found, "must not contain oop twice: " PTR_FORMAT ", found-index: " SIZE_FORMAT ", current-index: " SIZE_FORMAT,
             p2i(o), found_i, i);
      found = true;
      found_i = i;
      i++;
#endif
    }
  }
  validate("post-contains");
  return found;
}

void LockStack::oops_do(OopClosure* cl) {
  validate("pre-oops-do");
  for (oop* loc = _base; loc < _current; loc++) {
    //tty->print_cr("applying cl on lockstack idx: " SIZE_FORMAT ", _stack[i]: " PTR_FORMAT, i, p2i(_stack[i]));
    cl->do_oop(loc);
  }
  validate("post-oops-do");
}
