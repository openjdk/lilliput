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

#include "gc/shared/objHeaderForwarding.hpp"
#include "oops/oop.inline.hpp"

static inline oop forwardee_impl(oop obj, markWord mark) {
  assert(mark.is_marked(), "must be forwarded");
#ifdef _LP64
  if (mark.self_forwarded()) {
    return cast_to_oop(obj);
  } else
#endif
  {
    assert(mark.is_marked(), "only decode when actually forwarded");
    return cast_to_oop(mark.decode_pointer());
  }
}

void ObjHeaderForwarding::begin() {
  // Set-up preserved marks here.
}

void ObjHeaderForwarding::end() {
  // Restore preserved marks here.
}

bool ObjHeaderForwarding::is_forwarded(oop obj) {
  return obj->is_forwarded();
}

oop ObjHeaderForwarding::forwardee(oop obj) {
  return obj->forwardee();
}

void ObjHeaderForwarding::forward_to(oop obj, oop fwd) {
  obj->forward_to(fwd);
}

void ObjHeaderForwarding::forward_to_self(oop obj) {
  obj->forward_to_self();
}

oop ObjHeaderForwarding::forward_to_atomic(oop obj, oop fwd) {
  markWord mark = obj->mark_acquire();
  if (mark.is_marked()) {
    return forwardee_impl(obj, mark);
  }
  return obj->forward_to_atomic(fwd, mark);
}

oop ObjHeaderForwarding::forward_to_self_atomic(oop obj) {
  markWord mark = obj->mark_acquire();
  if (mark.is_marked()) {
    return forwardee_impl(obj, mark);
  }
  return obj->forward_to_self_atomic(mark);
}
