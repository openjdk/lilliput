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

#include "precompiled.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/objectMonitorManager.hpp"

ObjectMonitor* ObjectMonitorManager::_space = NULL;
ObjectMonitor* volatile ObjectMonitorManager::_free_list = NULL;

void ObjectMonitorManager::initialize() {
  _space = NEW_C_HEAP_ARRAY(ObjectMonitor, MaxNumMonitors, mtSynchronizer);
  ObjectMonitor* previous = NULL;
  for (uint32_t idx = 0; idx < MaxNumMonitors; idx++) {
    _space[idx].set_next_om(previous);
    previous = &_space[idx];
  }
  _free_list = previous;
}

ObjectMonitor* ObjectMonitorManager::get_monitor(uint32_t idx) {
  assert(idx < MaxNumMonitors, "monitor index exceeds maximum number of monitors");
  return &_space[idx];
}

uint ObjectMonitorManager::get_index(ObjectMonitor* monitor) {
  ptrdiff_t idx = monitor - _space;
  assert(0 <= idx && idx < MaxNumMonitors, "monitor index exceeds maximum number of monitors");
  return static_cast<uint32_t>(idx);
}

ObjectMonitor* ObjectMonitorManager::allocate_monitor() {
  ObjectMonitor* monitor;
  ObjectMonitor* prev = Atomic::load_acquire(&_free_list);
  if (prev == NULL) {
    fatal("Out of memory in ObjectMonitor space");
  }
  do {
    monitor = prev;
    ObjectMonitor* next = monitor->next_om();
    prev = Atomic::cmpxchg(&_free_list, monitor, next);
  } while (prev != monitor);
  monitor->set_next_om(NULL);
  return monitor;
}

void ObjectMonitorManager::free_monitor(ObjectMonitor* monitor) {
  ObjectMonitor* prev;
  ObjectMonitor* witness = Atomic::load_acquire(&_free_list);
  do {
    prev = witness;
    monitor->set_next_om(prev);
    witness = Atomic::cmpxchg(&_free_list, prev, monitor);
  } while (prev != witness);
}
