/*
 * Copyright (c) 2021, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHARED_FORWARDINGTABLE_HPP
#define SHARE_GC_SHARED_FORWARDINGTABLE_HPP

#include "gc/shared/gcForwarding.hpp"
#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"

// TODO: Address range and num-regions permitting, we could switch to
// a more compact encoding:
// - N bits to encode number of words from start of from region
// - M bits to encode number of words from start of to-region
// - X bits to encode to-region index
// from-region index is known implicitely.
// For example, we could do M = N = 24 bits and X = 16 bits.
class FwdTableEntry {
private:
  HeapWord* _from;
  HeapWord* _to;
public:
  FwdTableEntry() : _from(nullptr), _to(nullptr) {}
  HeapWord* from() const { return _from; }
  HeapWord* to()   const { return _to;   }

  inline void forward_to(HeapWord* from, HeapWord* to);
};

class PerRegionTable : public CHeapObj<mtGC> {
private:
  bool _used;
  intx _num_forwardings;
  intx _insertion_idx;
  FwdTableEntry* _table;
public:
  PerRegionTable();
  void initialize(intx num_forwardings);
  ~PerRegionTable();
  inline void forward_to(HeapWord* from, HeapWord* to);
  inline HeapWord* forwardee(HeapWord* from);
};

class ForwardingTable : public CHeapObj<mtGC> {
private:
  AddrToIdxFn const _addr_to_idx;
  size_t const _max_regions;

  PerRegionTable* _table;

public:
  ForwardingTable(AddrToIdxFn addr_to_idx, size_t max_regions): _addr_to_idx(addr_to_idx), _max_regions(max_regions), _table(nullptr) {}

  void begin();
  void begin_region(size_t idx, size_t num_forwardings);
  void end();

  inline void forward_to(oop from, oop to);
  inline oop forwardee(oop from);
};

#endif // SHARE_GC_SHARED_FORWARDINGABLE_HPP
