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

#ifndef SHARE_RUNTIME_OBJECTMONITORMANAGER_HPP
#define SHARE_RUNTIME_OBJECTMONITORMANAGER_HPP

#include "memory/allocation.hpp"

class ObjectMonitor;

class ObjectMonitorManager : public AllStatic {
private:
  // This is the total available space for ObjectMonitor instances. ObjectMonitor instances
  // are allocated from that space using a simple free-list allocator.
  static ObjectMonitor* _space;

  // A linked list of free object monitors.
  static ObjectMonitor* volatile _free_list;

public:
  static void initialize();

  static ObjectMonitor* get_monitor(uint32_t idx);
  static uint32_t get_index(ObjectMonitor* monitor);

  static ObjectMonitor* allocate_monitor();
  static void free_monitor(ObjectMonitor* monitor);
};

#endif // SHARE_RUNTIME_OBJECTMONITORMANAGER_HPP
