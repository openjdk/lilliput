/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_SYNCHRONIZER_INLINE_HPP
#define SHARE_RUNTIME_SYNCHRONIZER_INLINE_HPP

#include "oops/accessDecorators.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/synchronizer.hpp"

template <bool INFLATE_HEADER>
markWord ObjectSynchronizer::stable_header(const oop obj) {
  markWord mark = read_stable_mark(obj);
  if (mark.is_neutral() || mark.is_marked()) {
    return mark;
  } else if (mark.has_monitor()) {
    ObjectMonitor* monitor = mark.monitor();
    mark = monitor->header();
    assert(mark.is_neutral(), "invariant: header=" INTPTR_FORMAT, mark.value());
    return mark;
  } else if (SafepointSynchronize::is_at_safepoint() || Thread::current()->is_lock_owned((address) mark.locker())) {
    // This is a stack lock owned by the calling thread so fetch the
    // displaced markWord from the BasicLock on the stack.
    mark = mark.displaced_mark_helper();
    assert(mark.is_neutral(), "invariant: header=" INTPTR_FORMAT, mark.value());
    return mark;
  } else {
    if (INFLATE_HEADER) {
      ObjectMonitor* monitor = inflate(Thread::current(), obj, inflate_cause_vm_internal);
      mark = monitor->header();
      assert(mark.is_neutral(), "invariant: header=" INTPTR_FORMAT, mark.value());
      assert(!mark.is_marked(), "no forwarded objects here");
      return mark;
    } else {
      return markWord(0);
    }
  }
}

#endif // SHARE_RUNTIME_SYNCHRONIZER_INLINE_HPP
