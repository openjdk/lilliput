/*
 * Copyright (c) 2024, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_SPACE_INLINE_HPP
#define SHARE_GC_SHARED_SPACE_INLINE_HPP

#include "gc/shared/space.hpp"

template<class CL>
void ContiguousSpace::object_iterate_sized(CL* blk) {
  HeapWord* addr = bottom();
  oop last = nullptr;
  while (addr < top()) {
    oop obj = cast_to_oop(addr);
    size_t size = blk->do_object(obj);
    assert(!UseCompactObjectHeaders || obj->mark().narrow_klass() != 0, "null narrow klass, mark: " INTPTR_FORMAT ", last mark: " INTPTR_FORMAT, obj->mark().value(), last->mark().value());
    addr += size;
    last = obj;
  }
}

#endif // SHARE_GC_SHARED_SPACE_INLINE_HPP
