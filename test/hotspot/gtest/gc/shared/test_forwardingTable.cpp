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
 */

#include "precompiled.hpp"
#include "gc/shared/forwardingTable.inline.hpp"
#include "oops/oop.inline.hpp"
#include "unittest.hpp"

TEST_VM(ForwardingTable, empty) {
  ForwardingTable ft([](const void*) -> size_t { return 0; }, 1);
  ft.begin();
  ft.begin_region(0, 1);
  oopDesc obj;
  ASSERT_EQ(ft.forwardee(&obj), nullptr);
  ft.end();
}

TEST_VM(ForwardingTable, single) {
  ForwardingTable ft([](const void*) -> size_t { return 0; }, 1);
  ft.begin();
  ft.begin_region(0, 1);
  oopDesc obj1;
  oopDesc obj2;
  ft.forward_to(&obj1, &obj2);
  ASSERT_EQ(ft.forwardee(&obj1), cast_to_oop(&obj2));
  ft.end();
}

TEST_VM(ForwardingTable, ten) {
  ForwardingTable ft([](const void*) -> size_t { return 0; }, 1);
  ft.begin();
  ft.begin_region(0, 10);
  oopDesc obj0,  obj1,  obj2,  obj3,  obj4,  obj5,  obj6,  obj7,  obj8,  obj9;
  oopDesc obj0_, obj1_, obj2_, obj3_, obj4_, obj5_, obj6_, obj7_, obj8_, obj9_;
  ft.forward_to(&obj0, &obj0_);
  ft.forward_to(&obj1, &obj1_);
  ft.forward_to(&obj2, &obj2_);
  ft.forward_to(&obj3, &obj3_);
  ft.forward_to(&obj4, &obj4_);
  ft.forward_to(&obj5, &obj5_);
  ft.forward_to(&obj6, &obj6_);
  ft.forward_to(&obj7, &obj7_);
  ft.forward_to(&obj8, &obj8_);
  ft.forward_to(&obj9, &obj9_);
  ASSERT_EQ(ft.forwardee(&obj0), cast_to_oop(&obj0_));
  ASSERT_EQ(ft.forwardee(&obj1), cast_to_oop(&obj1_));
  ASSERT_EQ(ft.forwardee(&obj2), cast_to_oop(&obj2_));
  ASSERT_EQ(ft.forwardee(&obj3), cast_to_oop(&obj3_));
  ASSERT_EQ(ft.forwardee(&obj4), cast_to_oop(&obj4_));
  ASSERT_EQ(ft.forwardee(&obj5), cast_to_oop(&obj5_));
  ASSERT_EQ(ft.forwardee(&obj6), cast_to_oop(&obj6_));
  ASSERT_EQ(ft.forwardee(&obj7), cast_to_oop(&obj7_));
  ASSERT_EQ(ft.forwardee(&obj8), cast_to_oop(&obj8_));
  ASSERT_EQ(ft.forwardee(&obj9), cast_to_oop(&obj9_));
  ft.end();
}

// Inserting in reverse order should still work, albeit slow.
TEST_VM(ForwardingTable, ten_reverse) {
  ForwardingTable ft([](const void*) -> size_t { return 0; }, 1);
  ft.begin();
  ft.begin_region(0, 10);
  oopDesc obj0,  obj1,  obj2,  obj3,  obj4,  obj5,  obj6,  obj7,  obj8,  obj9;
  oopDesc obj0_, obj1_, obj2_, obj3_, obj4_, obj5_, obj6_, obj7_, obj8_, obj9_;
  ft.forward_to(&obj9, &obj9_);
  ft.forward_to(&obj8, &obj8_);
  ft.forward_to(&obj7, &obj7_);
  ft.forward_to(&obj6, &obj6_);
  ft.forward_to(&obj5, &obj5_);
  ft.forward_to(&obj4, &obj4_);
  ft.forward_to(&obj3, &obj3_);
  ft.forward_to(&obj2, &obj2_);
  ft.forward_to(&obj1, &obj1_);
  ft.forward_to(&obj0, &obj0_);
  ASSERT_EQ(ft.forwardee(&obj0), cast_to_oop(&obj0_));
  ASSERT_EQ(ft.forwardee(&obj1), cast_to_oop(&obj1_));
  ASSERT_EQ(ft.forwardee(&obj2), cast_to_oop(&obj2_));
  ASSERT_EQ(ft.forwardee(&obj3), cast_to_oop(&obj3_));
  ASSERT_EQ(ft.forwardee(&obj4), cast_to_oop(&obj4_));
  ASSERT_EQ(ft.forwardee(&obj5), cast_to_oop(&obj5_));
  ASSERT_EQ(ft.forwardee(&obj6), cast_to_oop(&obj6_));
  ASSERT_EQ(ft.forwardee(&obj7), cast_to_oop(&obj7_));
  ASSERT_EQ(ft.forwardee(&obj8), cast_to_oop(&obj8_));
  ASSERT_EQ(ft.forwardee(&obj9), cast_to_oop(&obj9_));
  ft.end();
}
