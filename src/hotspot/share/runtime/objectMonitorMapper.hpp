/*
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

#ifndef SHARE_RUNTIME_OBJECTMONITORMAPPER_HPP
#define SHARE_RUNTIME_OBJECTMONITORMAPPER_HPP

#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/concurrentHashTable.hpp"

class ObjectMonitor;

using ObjectMonitorTableValue = ObjectMonitor*;

class ObjectMonitorTableConfig : public StackObj {
public:
  using Value = ObjectMonitorTableValue;
  static void* allocate_node(void* context, size_t size, Value const& value);
  static void free_node(void* context, void* memory, Value const& value);
};

using ObjectMonitorTable = ConcurrentHashTable<ObjectMonitorTableConfig, mtSynchronizer>;

class ObjectMonitorMapper : public AllStatic {
private:
  static ObjectMonitorTable* _table;
  static ObjectMonitorTable* table();

public:
  static void initialize();
  static ObjectMonitor* get_monitor(oop object);
  static bool map_monitor(ObjectMonitor* monitor, oop obj, markWord mark);
  static void remove_monitor(ObjectMonitor* monitor);
};

#endif // SHARE_RUNTIME_OBJECTMONITORMAPPER_HPP
