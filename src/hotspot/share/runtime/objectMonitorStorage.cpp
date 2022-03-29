/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
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

#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.hpp"
#include "runtime/globals.hpp"
#include "runtime/objectMonitorStorage.hpp"
#include "services/memTracker.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

//static const bool be_paranoid = true;
static const bool be_paranoid = false;

ReservedSpace ObjectMonitorStorage::_rs;
ObjectMonitorStorage::ArrayType ObjectMonitorStorage::_array;
address ObjectMonitorStorage::_base = NULL;
uintx ObjectMonitorStorage::_max_capacity = 0;

// re-build a new list of newly allocated free monitors and return its head
void ObjectMonitorStorage::bulk_allocate_new_list(OMFreeListType& freelist_to_fill) {

  MutexLocker ml(ObjectMonitorStorage_lock, Mutex::_no_safepoint_check_flag);

  for (int i = 0; i < (int)PreallocatedObjectMonitors - 1; i ++) {
    ObjectMonitor* m = _array.allocate();
    if (m == NULL) {
      fatal("Maximum number of object monitors allocated (" UINTX_FORMAT "), increase MonitorStorageSize.",
            _array.capacity());
    }
    freelist_to_fill.prepend(m);
  }
  DEBUG_ONLY(freelist_to_fill.verify(be_paranoid);)
  DEBUG_ONLY(verify();)
  log_with_state("bulk_allocate_new_list: " UINTX_FORMAT " new monitors", PreallocatedObjectMonitors);
}

// When a thread dies, return OMs left unused to the global store.
void ObjectMonitorStorage::cleanup_before_thread_death(Thread* t) {
  // Note that the ObjectMonitors we are about to return to the storage are
  // not yet initialized, so no need to destroy them.
  OMFreeListType& tl_list = t->_om_freelist;
  if (tl_list.empty() == false) {
    MutexLocker ml(ObjectMonitorStorage_lock, Mutex::_no_safepoint_check_flag);
    log_with_state("cleanup_before_thread_death: returning " UINTX_FORMAT " unused monitors",
                   tl_list.count());
    _array.bulk_deallocate(tl_list);
    DEBUG_ONLY(verify();)
  }
  assert(tl_list.empty(), "thread local list should now be empty");
}

// deallocate a list of monitors
void ObjectMonitorStorage::bulk_deallocate(OMFreeListType& omlist) {
  if (omlist.empty() == false) {
    MutexLocker ml(ObjectMonitorStorage_lock, Mutex::_no_safepoint_check_flag);
    log_with_state("bulk_deallocate: returning " UINTX_FORMAT " deflated monitors",
                   omlist.count());
    _array.bulk_deallocate(omlist);
    DEBUG_ONLY(verify();)
  }
}

void ObjectMonitorStorage::initialize() {

  // Calc size of underlying address range
  const uintx min_object_monitors = 1024;
  const uintx max_object_monitors = (uintx)UINT_MAX - 1; // lets say, for now, it should fit into 32 bits
  const size_t range_size = align_up(
      clamp(MonitorStorageSize,
            min_object_monitors * sizeof(ObjectMonitor),
            max_object_monitors * sizeof(ObjectMonitor)),
            os::vm_page_size());
  const uintx max_capacity = range_size / sizeof(ObjectMonitor);

  // Reserve space
  _rs = ReservedSpace(range_size);
  if (_rs.is_reserved()) {
    log_with_state("Reserved: [" PTR_FORMAT "-" PTR_FORMAT "), " SIZE_FORMAT " bytes (" UINTX_FORMAT " monitors).",
                   p2i(_rs.base()), p2i(_rs.end()), _rs.size(), max_capacity);
    _array.initialize((ObjectMonitor*)_rs.base(), min_object_monitors, max_capacity);
    _base = (address) _rs.base();
    _max_capacity = max_capacity;
  } else {
    vm_exit_out_of_memory(range_size, OOM_MMAP_ERROR, "Failed to reserve Object Monitor Store");
  }

  // Register with NMT
  MemTracker::record_virtual_memory_type(_rs.base(), mtObjectMonitor);
}

void ObjectMonitorStorage::print(outputStream* st) {
  _array.print_on(st);
  st->cr();
}

#ifdef ASSERT
void ObjectMonitorStorage::verify() {
  assert_lock_strong(ObjectMonitorStorage_lock);
  _array.verify(be_paranoid);
}
#endif

void ObjectMonitorStorage::log_with_state(const char* fmt, ...) {
  LogTarget(Info, monitorinflation) lt;
  if (lt.is_enabled()) {
    va_list va;
    va_start(va, fmt);
    LogTarget(Info, monitorinflation) lt;
    assert(lt.is_enabled(), "only call via LOG macro");
    LogStream ls(lt);
    ls.print("OM Store: ");
    ls.vprint_cr(fmt, va);
    va_end(va);
    ls.print("OM Store: state now: ");
    print(&ls);
    ls.cr();
  }
}
