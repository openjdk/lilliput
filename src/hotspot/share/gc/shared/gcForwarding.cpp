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
#include "gc/shared/gcForwarding.hpp"
#include "gc/shared/forwardingTable.hpp"
#include "runtime/globals.hpp"

ForwardingTable* GCForwarding::_forwarding_table = nullptr;

void GCForwarding::initialize(AddrToIdxFn addr_to_idx, size_t max_regions) {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table == nullptr, "only call this once");
    _forwarding_table = new ForwardingTable(addr_to_idx, max_regions);
  }
}

void GCForwarding::begin() {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table != nullptr, "expect forwarding table initialized");
    _forwarding_table->begin();
  }
}

void GCForwarding::begin_region(size_t idx, size_t num_forwardings) {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table != nullptr, "expect forwarding table initialized");
    _forwarding_table->begin_region(idx, num_forwardings);
  }
}

void GCForwarding::end() {
  if (UseCompactObjectHeaders) {
    assert(_forwarding_table != nullptr, "expect forwarding table initialized");
    _forwarding_table->end();
  }
}
