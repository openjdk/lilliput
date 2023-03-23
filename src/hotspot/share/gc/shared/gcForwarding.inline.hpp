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

#ifndef SHARE_GC_SHARED_GCFORWARDING_INLINE_HPP
#define SHARE_GC_SHARED_GCFORWARDING_INLINE_HPP

#include "gc/shared/forwardingTable.inline.hpp"
#include "gc/shared/gcForwarding.hpp"
#include "oops/oop.inline.hpp"

inline bool GCForwarding::is_forwarded(oop obj) {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table != nullptr, "expect forwarding table initialized");
    return _forwarding_table->forwardee(obj) != nullptr;
  } else {
    return obj->is_forwarded();
  }
}

inline oop GCForwarding::forwardee(oop obj) {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table != nullptr, "expect forwarding table initialized");
    return _forwarding_table->forwardee(obj);
  } else {
    return obj->forwardee();
  }
}

inline void GCForwarding::forward_to(oop obj, oop fwd) {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table != nullptr, "expect forwarding table initialized");
    _forwarding_table->forward_to(obj, fwd);
    assert(is_forwarded(obj), "must be forwarded");
    assert(forwardee(obj) == fwd, "must be forwarded to correct forwardee");
  } else {
    obj->forward_to(fwd);
  }
}

#endif // SHARE_GC_SHARED_GCFORWARDING_INLINE_HPP
