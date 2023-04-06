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

#ifndef SHARE_GC_SHARED_GCFORWARDING_HPP
#define SHARE_GC_SHARED_GCFORWARDING_HPP

#include "memory/allStatic.hpp"
#include "oops/oopsHierarchy.hpp"

class ForwardingTable;

using AddrToIdxFn = size_t (*)(const void*);

class GCForwarding : public AllStatic {
private:
  static ForwardingTable* _forwarding_table;
public:

  static void initialize(AddrToIdxFn addr_to_idx, size_t max_regions);
  static void begin();
  static void begin_region(size_t idx, size_t num_forwardings);
  static void end();

  static inline bool is_forwarded(oop obj);
  static inline oop forwardee(oop obj);
  static inline void forward_to(oop obj, oop fwd);
};

#endif // SHARE_GC_SHARED_GCFORWARDING_HPP
