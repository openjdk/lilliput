/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_LIGHTWEIGHTSYNCHRONIZER_HPP
#define SHARE_RUNTIME_LIGHTWEIGHTSYNCHRONIZER_HPP

#include "memory/allStatic.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/synchronizer.hpp"

class ObjectMonitorWorld;

class LightweightSynchronizer : AllStatic {
private:
  static ObjectMonitorWorld* _omworld;

  static ObjectMonitor* get_or_insert_monitor_from_table(oop object, JavaThread* current, bool try_read, bool* inserted);
  static ObjectMonitor* get_or_insert_monitor(oop object, JavaThread* current, const ObjectSynchronizer::InflateCause cause, bool try_read);

  static ObjectMonitor* add_monitor(JavaThread* current, ObjectMonitor* monitor, oop obj);
  static bool remove_monitor(Thread* current, oop obj, ObjectMonitor* monitor);

  static void deflate_mark_word(oop object);

  static void ensure_lock_stack_space(JavaThread* locking_thread, JavaThread* current);

 public:
  static void initialize();

  static bool needs_resize(JavaThread* current);
  static bool resize_table(JavaThread* current);
  static void set_table_max(JavaThread* current);

  static void enter_for(Handle obj, BasicLock* lock, JavaThread* locking_thread);
  static void enter(Handle obj, BasicLock* lock,  JavaThread* locking_thread, JavaThread* current);
  static void exit(oop object, JavaThread* current);

  static ObjectMonitor* inflate_locked_or_imse(oop object, const ObjectSynchronizer::InflateCause cause, TRAPS);
  static ObjectMonitor* inflate_fast_locked_object(oop object, JavaThread* locking_thread, JavaThread* current, const ObjectSynchronizer::InflateCause cause);
  static bool inflate_and_enter(oop object, BasicLock* lock, JavaThread* locking_thread, JavaThread* current, const ObjectSynchronizer::InflateCause cause);

  static void deflate_monitor(Thread* current, oop obj, ObjectMonitor* monitor);
  static void deflate_anon_monitor(Thread* current, oop obj, ObjectMonitor* monitor);

  static ObjectMonitor* read_monitor(Thread* current, oop obj);

  static bool contains_monitor(Thread* current, ObjectMonitor* monitor);

  // NOTE: May not cause monitor inflation
  static intptr_t FastHashCode(Thread* current, oop obj);
};

#endif // SHARE_RUNTIME_LIGHTWEIGHTSYNCHRONIZER_HPP
