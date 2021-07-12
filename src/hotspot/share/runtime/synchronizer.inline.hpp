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

template <DecoratorSet MO>
markWord ObjectSynchronizer::read_stable_mark(const oop obj) {
  markWord mark = obj->mark<MO>();
  if (!mark.is_being_inflated()) {
    return mark;       // normal fast-path return
  }

  int its = 0;
  for (;;) {
    markWord mark = obj->mark<MO>();
    if (!mark.is_being_inflated()) {
      return mark;    // normal fast-path return
    }

    // The object is being inflated by some other thread.
    // The caller of read_stable_mark() must wait for inflation to complete.
    // Avoid live-lock.

    ++its;
    if (its > 10000 || !os::is_MP()) {
      if (its & 1) {
        os::naked_yield();
      } else {
        // Note that the following code attenuates the livelock problem but is not
        // a complete remedy.  A more complete solution would require that the inflating
        // thread hold the associated inflation lock.  The following code simply restricts
        // the number of spinners to at most one.  We'll have N-2 threads blocked
        // on the inflationlock, 1 thread holding the inflation lock and using
        // a yield/park strategy, and 1 thread in the midst of inflation.
        // A more refined approach would be to change the encoding of INFLATING
        // to allow encapsulation of a native thread pointer.  Threads waiting for
        // inflation to complete would use CAS to push themselves onto a singly linked
        // list rooted at the markword.  Once enqueued, they'd loop, checking a per-thread flag
        // and calling park().  When inflation was complete the thread that accomplished inflation
        // would detach the list and set the markword to inflated with a single CAS and
        // then for each thread on the list, set the flag and unpark() the thread.

        // Index into the lock array based on the current object address.
        static_assert(is_power_of_2(NINFLATIONLOCKS), "must be");
        int ix = (cast_from_oop<intptr_t>(obj) >> 5) & (NINFLATIONLOCKS-1);
        int YieldThenBlock = 0;
        assert(ix >= 0 && ix < NINFLATIONLOCKS, "invariant");
        gInflationLocks[ix]->lock();
        while (obj->mark<MO>() == markWord::INFLATING()) {
          // Beware: naked_yield() is advisory and has almost no effect on some platforms
          // so we periodically call current->_ParkEvent->park(1).
          // We use a mixed spin/yield/block mechanism.
          if ((YieldThenBlock++) >= 16) {
            Thread::current()->_ParkEvent->park(1);
          } else {
            os::naked_yield();
          }
        }
        gInflationLocks[ix]->unlock();
      }
    } else {
      SpinPause();       // SMP-polite spinning
    }
  }
}

template <DecoratorSet MO, bool INFLATE_HEADER>
markWord ObjectSynchronizer::stable_header(const oop obj) {
  markWord mark = read_stable_mark<MO>(obj);
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
