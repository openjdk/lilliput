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

/*
 * The actual per-region forwarding table.
 */
class PerRegionTable : public CHeapObj<mtGC> {
private:
  bool _used;
  intx _num_forwardings;
  intx _insertion_idx;
  FwdTableEntry* _table;

  inline intx lookup(HeapWord* from);
  inline void reforward(HeapWord* from, HeapWord* to);

public:
  PerRegionTable();
  void initialize(intx num_forwardings);
  ~PerRegionTable();
  inline void forward_to(HeapWord* from, HeapWord* to);
  inline HeapWord* forwardee(HeapWord* from);
};

/*
 * A forwarding-table implementation that's used by several full (sliding-) GCs.
 * It exploits a number of properties:
 * - Adding forwardings is single-threaded per region.
 * - Adding forwarding happens in sequential order, except in some exceptional situations.
 * - We know in advance how many forwardings we are going to insert.
 * - Forwardings are never removed, except wholesale when we're done.
*
 * We maintain a number of per-region forwarding tables, one per region. These can
 * easily be indexed by region-number. In order for this to work, the GC must pass
 * a function pointer to the fwd table which finds the region that contains a given
 * oop.
 *
 * Each per-region table is essentially a dense array of forwarding entries which are
 * (oop, oop) - tuples. Each table holds exactly N entries, where N is the number
 * of forwardings in the region. Entries in the table are always sorted by
 * original (from-) address of the forwarding entries.
 *
 * Insertion of new forwardings usually happens in sequential order, which makes
 * insertion trivial in these very common cases. The single exception is G1 serial
 * compaction, which is a last-last-ditch attempt to squeeze out some more space. In
 * this case we accept that we need to insert entries in the middle and copy all
 * subsequent entries upwards.
 *
 * Lookup is a straightforward binary search of entries.
 *
 * It is important that code that wishes to use this forwarding table follow this lifecycle:
 * - Allocate the ForwardingTable once, e.g. when the CollectedHeap is initialized.
 *   This establishes how many regions will be used at maximum and also establishes the
 *   fuction which maps heap addresses to region indices.
 * - When forwarding starts, call begin(). This initializes the array that holds the per-region
 *   tables.
 * - When forwarding of a particular region starts (usually by a GC worker thread), call
 *   begin_region(). This initializes the corresponding per-region table.
 * - Insert and look-up forwardings.
 * - When forwaring is finished, call end(). This will dispose all internal data structures.
 */
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
