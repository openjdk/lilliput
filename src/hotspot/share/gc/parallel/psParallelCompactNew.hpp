/*
 * Copyright (c) 2005, 2024, Oracle and/or its affiliates. All rights reserved.
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

// Abstract closure for use with ParMarkBitMap::iterate(), which will invoke the
// do_addr() method.
//
// The closure is initialized with the number of heap words to process
// (words_remaining()), and becomes 'full' when it reaches 0.  The do_addr()
// methods in subclasses should update the total as words are processed.  Since
// only one subclass actually uses this mechanism to terminate iteration, the
// default initial value is > 0.  The implementation is here and not in the
// single subclass that uses it to avoid making is_full() virtual, and thus
// adding a virtual call per live object.


// The Parallel collector is a stop-the-world garbage collector that
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
//      - calculating destination-region for each region for better parallellism in following phases
//      - calculate the destination of each object at the end of the collection
//      - adjust pointers to reflect new destination of objects
//      - move the objects to their destination
//      - update some references and reinitialize some variables
//
// A space that is being collected is divided into regions and with each region
// is associated an object of type ParallelCompactData.  Each region is of a
// fixed size and typically will contain more than 1 object and may have parts
// of objects at the front and back of the region.
//
// region            -----+---------------------+----------
// objects covered   [ AAA  )[ BBB )[ CCC   )[ DDD     )
//
// The marking phase does a complete marking of all live objects in the heap.
// The marking also compiles the size of the data for all live objects covered
// by the region.  This size includes the part of any live object spanning onto
// the region (part of AAA if it is live) from the front, all live objects
// contained in the region (BBB and/or CCC if they are live), and the part of
// any live objects covered by the region that extends off the region (part of
// DDD if it is live).  The marking phase uses multiple GC threads and marking
// is done in a bit array of type ParMarkBitMap.  The marking of the bit map is
// done atomically as is the accumulation of the size of the live objects
// covered by a region.
//
// The summary phase calculates the total live data to the left of each region
// XXX.  Based on that total and the bottom of the space, it can calculate the
// starting location of the live data in XXX.  The summary phase calculates for
// each region XXX quantities such as
//
//      - the amount of live data at the beginning of a region from an object
//        entering the region.
//      - the location of the first live data on the region
//      - a count of the number of regions receiving live data from XXX.
//
// See ParallelCompactData for precise details.  The summary phase also
// calculates the dense prefix for the compaction.  The dense prefix is a
// portion at the beginning of the space that is not moved.  The objects in the
// dense prefix do need to have their object references updated.  See method
// summarize_dense_prefix().
//
// The forward (to new address) phase calculates the new address of each
// objects and records old-addr-to-new-addr asssociation.
//
// The adjust pointers phase remap all pointers to reflect the new address of each object.
//
// The compaction phase moves objects to their new location.
//
// Compaction is done on a region basis.  A region that is ready to be filled is
// put on a ready list and GC threads take region off the list and fill them.  A
// region is ready to be filled if it empty of live objects.  Such a region may
// have been initially empty (only contained dead objects) or may have had all
// its live objects copied out already.  A region that compacts into itself is
// also ready for filling.  The ready list is initially filled with empty
// regions and regions compacting into themselves.  There is always at least 1
// region that can be put on the ready list.  The regions are atomically added
// and removed from the ready list.
//
// During compaction, there is a natural task dependency among regions because
// destination regions may also be source regions themselves.  Consequently, the
// destination regions are not available for processing until all live objects
// within them are evacuated to their destinations.  These dependencies lead to
// limited thread utilization as threads spin waiting on regions to be ready.
// Shadow regions are utilized to address these region dependencies.  The basic
// idea is that, if a region is unavailable because it still contains live
// objects and thus cannot serve as a destination momentarily, the GC thread
// may allocate a shadow region as a substitute destination and directly copy
// live objects into this shadow region.  Live objects in the shadow region will
// be copied into the target destination region when it becomes available.
//
// For more details on shadow regions, please refer to §4.2 of the VEE'19 paper:
// Haoyu Li, Mingyu Wu, Binyu Zang, and Haibo Chen.  2019.  ScissorGC: scalable
// and efficient compaction for Java full garbage collection.  In Proceedings of
// the 15th ACM SIGPLAN/SIGOPS International Conference on Virtual Execution
// Environments (VEE 2019).  ACM, New York, NY, USA, 108-121.  DOI:
// https://doi.org/10.1145/3313808.3313820

class PSRegionData /*: public CHeapObj<mtGC> */ {
  // A region index
  size_t const _idx;

  // The start of the region
  HeapWord* const _bottom;
  // The top of the region. (first word after last live object in containing space)
  HeapWord* const _top;
  // The end of the region (first word after last word of the region)
  HeapWord* const _end;

  // The next compaction address
  HeapWord* _new_top;

  // Points to the next region in the GC-worker-local work-list
  PSRegionData* _local_next;

  // Parallel workers claiming protocol, used during adjust-references phase.
  volatile bool _claimed;

public:

  PSRegionData(size_t idx, HeapWord* bottom, HeapWord* top, HeapWord* end) :
    _idx(idx), _bottom(bottom), _top(top), _end(end), _new_top(bottom),
          _local_next(nullptr), _claimed(false) {}

  size_t idx() const { return _idx; };

  HeapWord* bottom() const { return _bottom; }
  HeapWord* top() const { return _top; }
  HeapWord* end()   const { return _end;   }

  PSRegionData*  local_next() const { return _local_next; }
  PSRegionData** local_next_addr() { return &_local_next; }

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
    return !Atomic::cmpxchg(&_claimed, false, true);
  }
};

class PSParallelCompactNew : AllStatic {
public:
  typedef enum {
    old_space_id, eden_space_id,
    from_space_id, to_space_id, last_space_id
  } SpaceId;

public:
  // Inline closure decls
  //
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
  static PSRegionData*        _region_data_array;
  static PSRegionData**       _per_worker_region_data;

  static size_t               _num_regions_serial;
  static PSRegionData*        _region_data_array_serial;
  static bool                 _serial;

  // Reference processing (used in ...follow_contents)
  static SpanSubjectToDiscoveryClosure  _span_based_discoverer;
  static ReferenceProcessor*  _ref_processor;

  static uint get_num_workers() { return _serial ? 1 : ParallelScavengeHeap::heap()->workers().active_workers(); }
  static size_t get_num_regions() { return _serial ? _num_regions_serial : _num_regions; }
  static PSRegionData* get_region_data_array() { return _serial ? _region_data_array_serial : _region_data_array; }

public:
  static ParallelOldTracer* gc_tracer() { return &_gc_tracer; }

private:

  static void initialize_space_info();

  // Clear the marking bitmap and summary data that cover the specified space.
  static void clear_data_covering_space(SpaceId id);

  static void pre_compact();

  static void post_compact();

  static bool check_maximum_compaction();

  // Mark live objects
  static void marking_phase(ParallelOldTracer *gc_tracer);

  static void summary_phase();
  static void setup_regions_parallel();
  static void setup_regions_serial();

  static void adjust_pointers();
  static void forward_to_new_addr();

  // Move objects to new locations.
  static void compact();

public:
  static bool invoke(bool maximum_heap_compaction, bool serial);
  static bool invoke_no_policy(bool maximum_heap_compaction, bool serial);

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

  static void print_on_error(outputStream* st);
};

void steal_marking_work_new(TaskTerminator& terminator, uint worker_id);

#endif // SHARE_GC_PARALLEL_PSPARALLELCOMPACTNEW_HPP
