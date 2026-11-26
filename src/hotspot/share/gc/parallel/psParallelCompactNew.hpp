/*
 * Copyright (c) 2005, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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

#ifndef SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_HPP
#define SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_HPP

#include "gc/parallel/mutableSpace.hpp"
#include "gc/parallel/objectStartArray.hpp"
#include "gc/parallel/parallelScavengeHeap.hpp"
#include "gc/parallel/parMarkBitMap.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/collectorCounters.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/taskTerminator.hpp"
#include "oops/oop.hpp"
#include "runtime/atomic.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/growableArray.hpp"

class ParallelScavengeHeap;
class PSAdaptiveSizePolicy;
class PSYoungGen;
class PSOldGen;
class ParCompactionManagerNew;
class PSParallelCompactNew;
class ParallelOldTracer;
class STWGCTimer;

class SpaceInfoNew
{
public:
  MutableSpace* space() const { return _space; }

  // The start array for the (generation containing the) space, or null if there
  // is no start array.
  ObjectStartArray* start_array() const { return _start_array; }

  void set_space(MutableSpace* s)           { _space = s; }
  void set_start_array(ObjectStartArray* s) { _start_array = s; }

private:
  MutableSpace*     _space;
  ObjectStartArray* _start_array;
};

typedef enum {
  old_space_id, eden_space_id,
  from_space_id, to_space_id, last_space_id
} SpaceId;

// The Parallel compaction collector is a stop-the-world garbage collector that
// does parts of the collection using parallel threads.  The collection includes
// the tenured generation and the young generation.
//
// A collection consists of the following phases.
//
//      - marking phase
//      - summary phase (single-threaded)
//      - forward (to new address) phase
//      - adjust pointers phase
//      - compacting phase
//      - clean up phase
//
// Roughly speaking these phases correspond, respectively, to
//
//      - mark all the live objects
//      - set-up temporary regions to enable parallelism in following phases
//      - calculate the destination of each object at the end of the collection
//      - adjust pointers to reflect new destination of objects
//      - move the objects to their destination
//      - update some references and reinitialize some variables
//
// The marking phase does a complete marking of all live objects in the heap.
// The marking phase uses multiple GC threads and marking is done in a bit
// array of type ParMarkBitMap.  The marking of the bit map is done atomically.
//
// The summary phase sets up the regions, such that regions cover roughly
// uniform memory regions.
// However, if that would result in an object crossing a region boundary, then
// the upper bound is adjusted such that the region ends after that object.
// This way we can ensure that a GC worker thread can fully 'own' a region
// during the forwarding, adjustment and compaction phases, without worrying
// about other threads messing with parts of the object. The summary phase
// also sets up an alternative set of regions, where each region covers
// a single space. This is used for a serial compaction mode which achieves
// maximum compaction at the expense of parallelism during the forwarding
// compaction phases.
//
// Large objects can hamper parallel compaction so we give them special treatment.
// 1. Initial parallel region creation isolates each large object in its own
//    region. These are ignored in the first forwarding pass, so other objects
//    can slide past these temporarily pinned objects.
// 2. After parallel forwarding, large objects are forwarded separately in a
//    dedicated serial pass.
// 3. After parallel compaction, large objects are compacted separately in a
//    dedicated serial pass.
//
// The forwarding phase calculates the new address of each live object and records
// old-addr-to-new-addr association. It does this using multiple GC threads.
// Each worker operates on its own independent set of staggered regions, thus
// can treat its own set as an independent serial compaction.
// Regions are not (necessarily) contiguous, so objects cannot cross region
// boundaries.
//
// The adjust pointers phase remaps all pointers to reflect the new address of each
// object. Again, this uses multiple GC worker threads. Each thread claims
// a region, processes all references in all live objects of that region. Then
// the thread proceeds to claim the next region from the global pool, until
// all regions have been processed.
//
// The compaction phase moves objects to their new location. Again, this uses
// multiple GC worker threads. Each worker processes its same set used for
// forwarding, copying each live object to its new location (which is guaranteed
// to be lower in that thread's parts of the heap, and thus would never overwrite
// other objects).
//
// This algorithm will usually leave gaps of non-fillable memory at the end
// of regions, and potentially whole empty regions at the end of compaction.
// The post-compaction phase fills those gaps with filler objects to ensure
// that the heap remains parsable.
//
// Parallel compaction can leave gaps. In some cases, so called maximum
// compaction is requested, which we implement as a serial mode.
// The 4 heap spaces are treated as a single set of regions, and forwarding
// and compaction handle these with a single thread, achieving maximum
// compaction.
// Maximum compaction triggers include System.gc() (when UseMaximumCompactionOnSystemGC
// is enabled), or when Parallel GC's heuristics determine it to be needed.
//
// Possible improvements to the algorithm include:
// - Identify and ignore a 'dense prefix'. This requires that we collect
//   liveness data during marking, or that we scan the prefix object-by-object
//   during the summary phase.
// - We compact around some large objects, but not all, so there can still be
//   significant waste at the end of regions that later objects could fit in.
// - Add parallelism to large object handling.
// - Don't move large objects if it is not worth it (e.g. sliding 100MB object a few bytes).
// - Instead of finding out *after* the whole compaction that an allocation
//   can still not be satisfied, and then re-running the whole compaction
//   serially, we could determine that after the forwarding phase, and
//   re-do only forwarding serially, thus avoiding running marking,
//   adjusting references and compaction twice.
// - Randomize region assignment between cycles to avoided perpetuating worker liveness
//   imbalance. Smaller regions also alleviates this somewhat.
class PCRegionData /*: public CHeapObj<mtGC> */ {
  // A region index
  size_t const _idx;
  // Which space this region sits it
  SpaceId const _space_id;

  // The start of the region
  HeapWord* const _bottom;
  // The top of the region (the last word of the last live object in this region is somewhere below this word).
  // Not necessarily a tight upper bound (there may be dead space between live object and recorded top).
  HeapWord* const _top;
  // The end of the region (first word after last word of the region)
  HeapWord* const _end;

  // The next compaction address
  HeapWord* _new_top;

  // Parallel workers claiming protocol, used during adjust-references phase.
  volatile bool _claimed;

public:

  PCRegionData(size_t idx, SpaceId space_id, HeapWord* bottom, HeapWord* top, HeapWord* end) :
    _idx(idx), _space_id(space_id), _bottom(bottom), _top(top), _end(end), _new_top(bottom), _claimed(false) {}

  size_t idx() const { return _idx; }
  SpaceId space_id() const { return _space_id; }

  HeapWord* bottom() const { return _bottom; }
  HeapWord* top() const { return _top; }
  HeapWord* end()   const { return _end;   }

  HeapWord* new_top() const {
    return _new_top;
  }
  void set_new_top(HeapWord* new_top) {
    _new_top = new_top;
  }

  bool contains(oop obj) {
    auto* obj_start = cast_from_oop<HeapWord*>(obj);
    HeapWord* obj_end = obj_start + obj->size();
    return _bottom <= obj_start && obj_start < _end && _bottom < obj_end && obj_end <= _end;
  }

  bool claim() {
    bool claimed =  _claimed;
    if (claimed) {
      return false;
    }
    return !AtomicAccess::cmpxchg(&_claimed, false, true);
  }
};

class RegionSet {
  GrowableArray<PCRegionData*>* const _regions;
  uint const _num_workers;

public:
  RegionSet(GrowableArray<PCRegionData*>* regions, uint num_workers) : _regions(regions), _num_workers(num_workers) {}

  PCRegionData* region_for_worker(uint worker_id, uint idx) {
    size_t position = _num_workers * idx + worker_id;
    if (position >= (size_t)_regions->length()) {
      return nullptr;
    }
    return _regions->at(position);
  }

  uint num_workers() const {
    return _num_workers;
  }
};

class PSParallelCompactNew : AllStatic {
public:
  class IsAliveClosure: public BoolObjectClosure {
  public:
    bool do_object_b(oop p) final;
  };

private:
  static STWGCTimer           _gc_timer;
  static ParallelOldTracer    _gc_tracer;
  static elapsedTimer         _accumulated_time;
  static unsigned int         _maximum_compaction_gc_num;
  static CollectorCounters*   _counters;
  static ParMarkBitMap        _mark_bitmap;
  static IsAliveClosure       _is_alive_closure;
  static SpaceInfoNew         _space_info[last_space_id];

  // The head of the global region data list.
  static size_t               _num_regions;
  static PCRegionData*        _region_data_array;
  static GrowableArray<PCRegionData*>* _parallel_regions;
  static GrowableArray<PCRegionData*>* _large_object_regions;

  static size_t               _num_regions_serial;
  static PCRegionData*        _region_data_array_serial;
  static GrowableArray<PCRegionData*>* _serial_regions;
  static PCRegionData**       _top_parallel_region_array;
  static bool                 _serial;

  // Reference processing (used in ...follow_contents)
  static SpanSubjectToDiscoveryClosure  _span_based_discoverer;
  static ReferenceProcessor*  _ref_processor;

  static uint get_num_workers() { return _serial ? 1 : ParallelScavengeHeap::heap()->workers().active_workers(); }
  static size_t get_num_regions() { return _serial ? _num_regions_serial : _num_regions; }
  static PCRegionData* get_region_data_array() { return _serial ? _region_data_array_serial : _region_data_array; }

public:
  static ParallelOldTracer* gc_tracer() { return &_gc_tracer; }

private:

  static void initialize_space_info();

  // Clear the marking bitmap and summary data that cover the specified space.
  static void clear_data_covering_space(SpaceId id);

  static void pre_compact();

  static void synchronize_regions();

  static void post_compact();

  static bool check_maximum_compaction(bool should_do_max_compaction);

  // Mark live objects
  static void marking_phase(ParallelOldTracer *gc_tracer);

  static void summary_phase();
  static void setup_regions_parallel();
  static size_t calculate_region_size();
  static void setup_regions_serial();

  static void adjust_pointers();
  static void forward_normal_objects(RegionSet* regions);
  static void forward_large_objects();

  // Move objects to new locations.
  static void compact(RegionSet* regions, const char* phase_name);

public:
  static bool invoke(bool maximum_heap_compaction, bool should_do_max_compaction);

  static void adjust_pointers_in_spaces(uint worker_id);

  static void post_initialize();
  // Perform initialization for PSParallelCompactNew that requires
  // allocations.  This should be called during the VM initialization
  // at a pointer where it would be appropriate to return a JNI_ENOMEM
  // in the event of a failure.
  static bool initialize_aux_data();

  // Closure accessors
  static BoolObjectClosure* is_alive_closure()     { return &_is_alive_closure; }

  // Public accessors
  static elapsedTimer* accumulated_time() { return &_accumulated_time; }

  static CollectorCounters* counters()    { return _counters; }

  static inline bool is_marked(oop obj);

  template <class T> static inline void adjust_pointer(T* p);

  // Convenience wrappers for per-space data kept in _space_info.
  static inline MutableSpace*     space(SpaceId space_id);
  static inline ObjectStartArray* start_array(SpaceId space_id);

  static ParMarkBitMap* mark_bitmap() { return &_mark_bitmap; }

  // Reference Processing
  static ReferenceProcessor* ref_processor() { return _ref_processor; }

  static STWGCTimer* gc_timer() { return &_gc_timer; }

  // Return the SpaceId for the given address.
  static SpaceId space_id(HeapWord* addr);

  static void print_on(outputStream* st);
};

void steal_marking_work_new(TaskTerminator& terminator, uint worker_id);

#endif // SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_HPP
