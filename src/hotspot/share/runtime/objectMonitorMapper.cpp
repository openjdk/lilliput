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

#include "precompiled.hpp"

#include "oops/oop.inline.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/objectMonitorMapper.hpp"
#include "utilities/concurrentHashTable.inline.hpp"

ObjectMonitorTable* ObjectMonitorMapper::_table = nullptr;

static uintx hash(const oop obj) {
  return 42;
}

void* ObjectMonitorTableConfig::allocate_node(void* context, size_t size, Value const& value) {
  return NEW_C_HEAP_ARRAY(char, size, mtSynchronizer);
}

void ObjectMonitorTableConfig::free_node(void* context, void* memory, Value const& value) {
  FREE_C_HEAP_ARRAY(char, memory);
}

class ObjectMonitorTableLookup : public StackObj {
private:
  oop _object;
public:
  ObjectMonitorTableLookup(oop object) : _object(object) {}

  uintx get_hash() const {
    return hash(_object);
  }

  bool equals(ObjectMonitorTableValue* value) {
    return (*value)->object_peek() == _object;
  }

  bool is_dead(ObjectMonitorTableValue* value) {
    return (*value)->object_peek() == nullptr;
  }
};

class ObjectMonitorTableFound : public StackObj {
private:
  ObjectMonitor* _monitor;
public:
  ObjectMonitorTableFound() : _monitor(nullptr) {}

  void operator()(ObjectMonitorTableValue* value) {
    _monitor = *value;
  }

  ObjectMonitor* monitor() {
    return _monitor;
  }
};

ObjectMonitorTable* ObjectMonitorMapper::table() {
  assert(_table != nullptr, "must be initialized");
  return _table;
}

void ObjectMonitorMapper::initialize() {
  assert(_table == nullptr, "must not yet have been initialized");
  if (UseCompactObjectHeaders) {
    _table = new ObjectMonitorTable();
  }
}

ObjectMonitor* ObjectMonitorMapper::get_monitor(oop object) {
  assert(object->mark().has_monitor(), "object must be monitor-locked");
  if (UseCompactObjectHeaders) {
    ObjectMonitorTableLookup lookup(object);
    ObjectMonitorTableFound found;
    _table->get(Thread::current(), lookup, found);
    //tty->print_cr("finding monitor for obj: " PTR_FORMAT ", monitor: " PTR_FORMAT, p2i(object), p2i(found.monitor()));
    return found.monitor();
  } else {
    return object->mark().monitor();
  }
}

bool ObjectMonitorMapper::map_monitor(ObjectMonitor* monitor, oop object, markWord mark) {
  if (UseCompactObjectHeaders) {
    // tty->print_cr("mapping monitor for obj: " PTR_FORMAT ", monitor: " PTR_FORMAT, p2i(object), p2i(monitor));
    ObjectMonitorTableLookup lookup(monitor->object_peek());
    ObjectMonitorTableFound found;
    bool inserted = _table->insert_get(Thread::current(), lookup, monitor, found);
    if (!inserted) {
      // Somebody else won.
      return false;
    }
    while (true) {
      markWord monitor_mark = mark.set_has_monitor();;
      markWord old_mark = object->cas_set_mark(monitor_mark, mark);
      if (old_mark == mark) {
        break;
      }
      assert(!old_mark.has_monitor(), "should not happen");
      mark = old_mark;
    }
    return true;
  } else {
    markWord monitor_mark = markWord::encode(monitor);
    markWord old_mark = object->cas_set_mark(monitor_mark, mark);
    return old_mark == mark;
  }
}

void ObjectMonitorMapper::remove_monitor(ObjectMonitor* monitor) {
  ObjectMonitorTableLookup lookup(monitor->object_peek());
  bool removed = _table->remove(Thread::current(), lookup);
  assert(removed, "object monitor must have been removed");
}
