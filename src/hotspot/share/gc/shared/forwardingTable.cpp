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
#include "gc/shared/forwardingTable.hpp"

const float PerRegionTable::LOAD_FACTOR = 1.3f;

PerRegionTable::PerRegionTable() : _used(false), _table_size(0), _table(nullptr) {
}

PerRegionTable::~PerRegionTable() {
  if (_table != nullptr) {
    FREE_C_HEAP_ARRAY(FwdTableEntry, _table);
  }
}

void PerRegionTable::initialize(intx num_forwardings) {
  assert(!_used, "init per-region table only once");
  _used = true;
  _table_size = (intx)(num_forwardings * LOAD_FACTOR);
  _table = NEW_C_HEAP_ARRAY(FwdTableEntry, _table_size, mtGC);
  for (intx i = 0; i < _table_size; i++) {
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
  assert(idx < _max_regions, "region index must be within bounds");
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
