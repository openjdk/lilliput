
/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
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
#include "gc/shared/forwardingTable.hpp"

PerRegionTable::PerRegionTable() : _used(false), _num_forwardings(0), _insertion_idx(0), _table(nullptr) {
}

PerRegionTable::~PerRegionTable() {
  if (_table != nullptr) {
    FREE_C_HEAP_ARRAY(FwdTableEntry, _table);
  }
}

void PerRegionTable::initialize(intx num_forwardings) {
  _used = true;
  _num_forwardings = num_forwardings;
  _insertion_idx = 0;
  _table = NEW_C_HEAP_ARRAY(FwdTableEntry, num_forwardings, mtGC);
  for (intx i = 0; i < num_forwardings; i++) {
    _table[i] = FwdTableEntry();
  }
}

void ForwardingTable::begin() {
  assert(_table == nullptr, "must not have been initialized");
  _table = NEW_C_HEAP_ARRAY(PerRegionTable, _max_regions, mtGC);
  for (size_t i = 0; i < _max_regions; i++) {
    _table[i] = PerRegionTable();
  }
}

void ForwardingTable::begin_region(size_t idx, size_t num_forwardings) {
  assert(_table != nullptr, "must have been initialized");
  _table[idx].initialize(num_forwardings);
}

void ForwardingTable::end() {
  assert(_table != nullptr, "must have been initialized");
  for (size_t i = 0; i < _max_regions; i++) {
    _table[i].~PerRegionTable();
  }
  FREE_C_HEAP_ARRAY(PerRegionTable, _table);
  _table = nullptr;
}
