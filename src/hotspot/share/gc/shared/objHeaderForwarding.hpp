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

#ifndef SHARE_GC_SHARED_OBJHEADERFORWARDING_HPP
#define SHARE_GC_SHARED_OBJHEADERFORWARDING_HPP

#include "gc/shared/gcForwarding.hpp"

class ObjHeaderForwarding : public GCForwardingImpl {
private:
  // It's a singleton.
  ObjHeaderForwarding() {}

public:
  static ObjHeaderForwarding& instance() {
    static ObjHeaderForwarding instance;
    return instance;
  }

  void begin() override;
  void end() override;
  bool is_forwarded(oop obj) override;
  oop forwardee(oop obj) override;
  void forward_to(oop obj, oop fwd) override;
  void forward_to_self(oop obj) override;
  oop forward_to_atomic(oop obj, oop fwd) override;
  oop forward_to_self_atomic(oop obj) override;

  // Make sure it remains a singleton.
  ObjHeaderForwarding(ObjHeaderForwarding const&) = delete;
  void operator=(ObjHeaderForwarding const&)      = delete;
};

#endif // SHARE_GC_SHARED_OBJHEADERFORWARDING_HPP
