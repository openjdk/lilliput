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

#ifndef SHARE_RUNTIME_OBJECTMONITORSTORAGE_HPP
#define SHARE_RUNTIME_OBJECTMONITORSTORAGE_HPP

#include "memory/allocation.hpp"
#include "memory/allStatic.hpp"
#include "oops/oop.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/thread.hpp"
#include "utilities/addressStableArray.inline.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

typedef FreeList<ObjectMonitor> OMFreeListType;

typedef uint32_t OMRef;
#define INVALID_OMREF ((OMRef)-1)

class ObjectMonitorStorage : public AllStatic {

  typedef AddressStableArrayWithFreeList<ObjectMonitor> ArrayType;
  static ArrayType _array;

  static ReservedSpace _rs;

  // Precalculated for quick access
  static address _base;
  static uintx _max_capacity;

  // re-build a new list of newly allocated free monitors and return its head
  static void bulk_allocate_new_list(OMFreeListType& freelist_to_fill);

  // Return the current thread's om freelist
  static OMFreeListType& current_omlist() {
    // Note: monitors in this list are not initialized.
    return Thread::current()->_om_freelist;
  }

  static void log_with_state(const char* fmt, ...) ATTRIBUTE_PRINTF(1, 2);

public:

  // On behalf of the current thread allocate a single monitor, preferably from
  // thread local freelist
  static ObjectMonitor* allocate_monitor(oop object) {
    OMFreeListType& tl_list = current_omlist();
    ObjectMonitor* om = tl_list.take_top();
    if (om == NULL) {
      bulk_allocate_new_list(tl_list);
      om = tl_list.take_top();
      assert(om != NULL, "sanity");
    }
    om = new (om) ObjectMonitor(object);
    assert(decode(encode(om)) == om, "Decoding problem.");
    return om; // done
  }

  // On behalf of the current thread deallocate a single monitor
  static void deallocate_monitor(ObjectMonitor* m) {
    m->~ObjectMonitor(); // de-initialize.
    OMFreeListType& tl_list = current_omlist();
    tl_list.prepend(m);
  }

  // deallocate a list of monitors; empties out the donor list.
  static void bulk_deallocate(OMFreeListType& omlist);

  static void cleanup_before_thread_death(Thread* t);

  static void initialize();

  static void print(outputStream* st);

  DEBUG_ONLY(static void verify();)

  // Encoding, decoding
  static address base()           { return _base; }
  static ObjectMonitor* ombase()  { return (ObjectMonitor*) base(); }

  static ObjectMonitor* decode(OMRef ref) {
    assert(ref < _max_capacity, "OM ref oob (%u)", ref);
    return ombase() + ref;
  }

  static OMRef encode(const ObjectMonitor* om) {
    assert(om >= ombase() && om < (ombase() + _max_capacity),
           "OM oob (" PTR_FORMAT ")", p2i(om));
    assert(is_aligned(om, BytesPerWord), "om unaligned or has tag bits (" PTR_FORMAT ")", p2i(om));
    return (OMRef)(om - ombase());
  }

};

#endif // SHARE_RUNTIME_OBJECTMONITORSTORAGE_HPP
