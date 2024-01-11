
#include "precompiled.hpp"

#include "classfile/classLoaderData.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc/serial/cardTableRS.hpp"
#include "gc/serial/generation.hpp"
#include "gc/serial/serialCompressor.hpp"
#include "gc/serial/serialGcRefProcProxyTask.hpp"
#include "gc/serial/serialHeap.hpp"
#include "gc/serial/serialStringDedup.hpp"
#include "gc/shared/classUnloadingContext.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shared/referenceProcessor.hpp"
#include "gc/shared/referenceProcessorPhaseTimes.hpp"
#include "gc/shared/space.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "gc/shared/taskqueue.hpp"
#include "gc/shared/weakProcessor.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "utilities/copy.hpp"
#include "utilities/stack.inline.hpp"

#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#endif

SCBlockOffsetTable::SCBlockOffsetTable(MarkBitMap& mark_bitmap) :
  _table(nullptr),
  _mark_bitmap(mark_bitmap),
  _covered(SerialHeap::heap()->reserved_region()) { }

SCBlockOffsetTable::~SCBlockOffsetTable() {
  FREE_C_HEAP_ARRAY(HeapWord*, _table);
}

inline size_t SCBlockOffsetTable::addr_to_block_idx(HeapWord* addr) const {
  assert(addr >= _covered.start() && addr <= _covered.end(), "address must be in heap");
  return (addr - _covered.start()) / words_per_block();
}

void SCBlockOffsetTable::build_table_for_space(CompactPoint& cp, ContiguousSpace* space) {
  HeapWord* bottom = space->bottom();
  HeapWord* top = space->top();

  // We're sure to be here before any objects are compacted into this
  // space, so this is a good time to initialize this:
  space->set_compaction_top(bottom);

  if (cp.space == nullptr) {
    assert(cp.gen != nullptr, "need a generation");
    assert(cp.gen->first_compaction_space() == space, "just checking");
    cp.space = cp.gen->first_compaction_space();
    cp.space->set_compaction_top(cp.space->bottom());
  }

  size_t bottom_block = addr_to_block_idx(bottom);
  //tty->print_cr("covered-start: " PTR_FORMAT ", covered-end: " PTR_FORMAT ", bottom: " PTR_FORMAT ", top: " PTR_FORMAT ", end: " PTR_FORMAT ", addr: " PTR_FORMAT, p2i(_covered.start()), p2i(_covered.end()), p2i(bottom), p2i(top), p2i(space->end()), p2i(align_up(space->top(), words_per_block() * BytesPerWord)));
  size_t top_block = addr_to_block_idx(MIN2(space->end(), align_up(space->top(), words_per_block() * BytesPerWord)));
  Copy::fill_to_words(reinterpret_cast<HeapWord*>(&_table[bottom_block]), top_block - bottom_block);

  HeapWord* compact_top = cp.space->compaction_top();
  HeapWord* current = _mark_bitmap.get_next_marked_addr(bottom, top);
  while (current < top) {
    size_t idx = addr_to_block_idx(current);
    assert(_table[idx] == nullptr, "must be new block");
    HeapWord* block_end = MIN2(top, align_up(current + 1, words_per_block() * BytesPerWord));
    size_t live_in_block = 0;
    HeapWord* first_in_block = current;
    HeapWord* compact = compact_top;
    while (current < block_end) {
      oop obj = cast_to_oop(current);
      assert(oopDesc::is_oop(obj), "must be oop start");
      size_t obj_size = obj->size();
      live_in_block += obj_size;

      // Advance to next live object.
      current = _mark_bitmap.get_next_marked_addr(current + obj_size, top);
    }

    // Check if block fits into current compaction space, and switch to next,
    // if necessary. The compaction space must have enough space left to
    // accomodate all objects that start in the block.
    while (live_in_block > pointer_delta(cp.space->end(), compact_top)) {
      cp.space->set_compaction_top(compact_top);
      cp.space = cp.space->next_compaction_space();
      if (cp.space == nullptr) {
        cp.gen = GenCollectedHeap::heap()->young_gen();
        assert(cp.gen != nullptr, "compaction must succeed");
        cp.space = cp.gen->first_compaction_space();
        assert(cp.space != nullptr, "generation must have a first compaction space");
      }
      compact_top = cp.space->bottom();
      cp.space->set_compaction_top(compact_top);
    }

    // Record address of the first live word in this block.
    HeapWord* block_start = align_down(first_in_block, words_per_block() * BytesPerWord);
    size_t num_live = _mark_bitmap.count_marked_words(block_start, first_in_block);
    _table[idx] = compact_top - num_live;
    assert(forwardee(first_in_block) == compact_top, "must match");

    compact_top += live_in_block;
  }
  cp.space->set_compaction_top(compact_top);
}

void SCBlockOffsetTable::build_table_for_generation(CompactPoint& cp, Generation* generation) {
  ContiguousSpace* space = generation->first_compaction_space();
  while (space != nullptr) {
    build_table_for_space(cp, space);
    space = space->next_compaction_space();
  }
}

void SCBlockOffsetTable::build_table() {
  SerialHeap* heap = SerialHeap::heap();
  HeapWord* start = _covered.start();
  HeapWord* end = _covered.end();
  size_t num_blocks = align_up(end - start, words_per_block()) / words_per_block();
  _table = NEW_C_HEAP_ARRAY(HeapWord*, num_blocks, mtGC);

  CompactPoint cp(heap->old_gen());
  build_table_for_generation(cp, heap->old_gen());
  build_table_for_generation(cp, heap->young_gen());
}

inline HeapWord* SCBlockOffsetTable::forwardee(HeapWord* addr) const {
  assert(_mark_bitmap.is_marked(addr), "must be marked");
  HeapWord* block_base = align_down(addr, words_per_block() * BytesPerWord);
  size_t block = addr_to_block_idx(addr);
  assert(_table[block] != nullptr, "must have initialized BOT entry");
  return _table[block] + _mark_bitmap.count_marked_words(block_base, addr);
}

SerialCompressor::SerialCompressor(STWGCTimer* gc_timer):
  _mark_bitmap(),
  _marking_stack(),
  _objarray_stack(),
  _bot(_mark_bitmap),
  _string_dedup_requests(),
  _gc_timer(gc_timer),
  _gc_tracer() {
  SerialHeap* heap = SerialHeap::heap();
  MemRegion reserved = heap->reserved_region();
  size_t bitmap_size = MarkBitMap::compute_size(reserved.byte_size());
  ReservedSpace bitmap(bitmap_size, MAX2(os::vm_page_size(), (size_t)SCBlockOffsetTable::words_per_block() * BytesPerWord));
  _mark_bitmap_region = MemRegion((HeapWord*) bitmap.base(), bitmap.size() / HeapWordSize);
  os::commit_memory_or_exit((char *)_mark_bitmap_region.start(), _mark_bitmap_region.byte_size(), false,
                            "Cannot commit bitmap memory");
  _mark_bitmap.initialize(heap->reserved_region(), _mark_bitmap_region);
}

SerialCompressor::~SerialCompressor() {
  os::release_memory((char*)_mark_bitmap_region.start(), _mark_bitmap_region.byte_size());
}

void SerialCompressor::invoke_at_safepoint(bool clear_all_softrefs) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at a safepoint");

  SerialHeap* gch = SerialHeap::heap();
#ifdef ASSERT
  if (gch->soft_ref_policy()->should_clear_all_soft_refs()) {
    assert(clear_all_softrefs, "Policy should have been checked earlier");
  }
#endif

  gch->trace_heap_before_gc(&_gc_tracer);

  // Increment the invocation count
  //_total_invocations++;

  // Capture used regions for each generation that will be
  // subject to collection, so that card table adjustments can
  // be made intelligently (see clear / invalidate further below).
  gch->save_used_regions();

  phase1_mark(clear_all_softrefs);
  phase2_build_bot();

  // Don't add any more derived pointers during phase3
#if COMPILER2_OR_JVMCI
  assert(DerivedPointerTable::is_active(), "Sanity");
  DerivedPointerTable::set_active(false);
#endif

  phase3_compact_and_update();

  // Set saved marks for allocation profiler (and other things? -- dld)
  // (Should this be in general part?)
  gch->save_marks();

  bool is_young_gen_empty = (gch->young_gen()->used() == 0);
  gch->rem_set()->maintain_old_to_young_invariant(gch->old_gen(), is_young_gen_empty);

  gch->prune_scavengable_nmethods();

  // Update heap occupancy information which is used as
  // input to soft ref clearing policy at the next gc.
  Universe::heap()->update_capacity_and_used_at_gc();

  // Signal that we have completed a visit to all live objects.
  Universe::heap()->record_whole_heap_examined_timestamp();

  gch->trace_heap_after_gc(&_gc_tracer);

  delete _ref_processor;
}

template<class T>
void SerialCompressor::mark_and_push(T* p) {
  T heap_oop = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(heap_oop)) {
    oop obj = CompressedOops::decode_not_null(heap_oop);
    if (mark_object(obj)) {
      _marking_stack.push(obj);
    }
  }
}

class SCMarkAndPushClosure: public ClaimMetadataVisitingOopIterateClosure {
private:
  SerialCompressor& _compressor;

  template<typename T>
  void do_oop_work(T* p) {
    _compressor.mark_and_push(p);
  }

public:
  SCMarkAndPushClosure(int claim, SerialCompressor& compressor) :
    ClaimMetadataVisitingOopIterateClosure(claim),
    _compressor(compressor) {
    set_ref_discoverer_internal(compressor.ref_processor());
  }

  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

class SCFollowRootClosure: public BasicOopIterateClosure {
private:
  SerialCompressor& _compressor;

  template<class T>
  void follow_root(T* p) {
    assert(!Universe::heap()->is_in(p), "roots shouldn't be things within the heap");
    _compressor.mark_and_push(p);
    _compressor.follow_stack();
  }

public:
  SCFollowRootClosure(SerialCompressor& compressor) :
    _compressor(compressor) { }

  void do_oop(oop* p)       { follow_root(p); }
  void do_oop(narrowOop* p) { follow_root(p); }
};

class SCFollowStackClosure: public VoidClosure {
private:
  SerialCompressor& _compressor;
public:
  SCFollowStackClosure(SerialCompressor& compressor) :
    _compressor(compressor) { }
  void do_void() {
    _compressor.follow_stack();
  }
};

class SCIsAliveClosure: public BoolObjectClosure {
  MarkBitMap& _mark_bitmap;
public:
  SCIsAliveClosure(MarkBitMap& mark_bitmap) :
    _mark_bitmap(mark_bitmap) { }
  bool do_object_b(oop p) {
    return _mark_bitmap.is_marked(p);
  }
};

class SCKeepAliveClosure: public OopClosure {
private:
  SerialCompressor& _compressor;
  template<class T>
  void do_oop_work(T* p) {
    _compressor.mark_and_push(p);
  }
public:
  SCKeepAliveClosure(SerialCompressor& compressor) :
    _compressor(compressor) { }
  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

bool SerialCompressor::mark_object(oop obj) {
  HeapWord* addr = cast_from_oop<HeapWord*>(obj);
  if (!_mark_bitmap.is_marked(addr)) {
    if (StringDedup::is_enabled() &&
        java_lang_String::is_instance(obj) &&
        SerialStringDedup::is_candidate_from_mark(obj)) {
      _string_dedup_requests.add(obj);
    }

    // Do the transform while we still have the header intact,
    // which might include important class information.
    ContinuationGCSupport::transform_stack_chunk(obj);

    _mark_bitmap.mark_range(addr, obj->size());
    return true;
  } else {
    return false;
  }
}

void SerialCompressor::push_objarray(objArrayOop array, size_t index) {
  ObjArrayTask task(array, index);
  assert(task.is_valid(), "bad ObjArrayTask");
  _objarray_stack.push(task);
}

void SerialCompressor::follow_array(objArrayOop array) {
  SCMarkAndPushClosure mark_and_push_closure(ClassLoaderData::_claim_stw_fullgc_mark, *this);
  mark_and_push_closure.do_klass(array->klass());

  if (array->length() > 0) {
    push_objarray(array, 0);
  }
}

void SerialCompressor::follow_object(oop obj) {
  assert(_mark_bitmap.is_marked(obj), "p must be marked");
  if (obj->is_objArray()) {
    follow_array((objArrayOop)obj);
  } else {
    SCMarkAndPushClosure mark_and_push_closure(ClassLoaderData::_claim_stw_fullgc_mark, *this);
    obj->oop_iterate(&mark_and_push_closure);
  }
}

void SerialCompressor::follow_array_chunk(objArrayOop array, int index) {
  const int len = array->length();
  const int beg_index = index;
  assert(beg_index < len || len == 0, "index too large");

  const int stride = MIN2(len - beg_index, (int) ObjArrayMarkingStride);
  const int end_index = beg_index + stride;

  SCMarkAndPushClosure mark_and_push_closure(ClassLoaderData::_claim_stw_fullgc_mark, *this);
  array->oop_iterate_range(&mark_and_push_closure, beg_index, end_index);

  if (end_index < len) {
    push_objarray(array, end_index); // Push the continuation.
  }
}

void SerialCompressor::follow_stack() {
  do {
    while (!_marking_stack.is_empty()) {
      oop obj = _marking_stack.pop();
      assert(_mark_bitmap.is_marked(obj), "p must be marked");
      follow_object(obj);
    }
    // Process ObjArrays one at a time to avoid marking stack bloat.
    if (!_objarray_stack.is_empty()) {
      ObjArrayTask task = _objarray_stack.pop();
      follow_array_chunk(objArrayOop(task.obj()), task.index());
    }
  } while (!_marking_stack.is_empty() || !_objarray_stack.is_empty());
}

void SerialCompressor::phase1_mark(bool clear_all_softrefs) {
  // Recursively traverse all live objects and mark them
  GCTraceTime(Info, gc, phases) tm("Phase 1: Mark live objects", _gc_timer);

  SerialHeap* gch = SerialHeap::heap();

  ClassLoaderDataGraph::verify_claimed_marks_cleared(ClassLoaderData::_claim_stw_fullgc_mark);

  AlwaysTrueClosure always_true_closure;
  _ref_processor = new ReferenceProcessor(&always_true_closure);
  _ref_processor->start_discovery(clear_all_softrefs);

  {
    StrongRootsScope srs(0);
    SCMarkAndPushClosure mark_and_push_closure(ClassLoaderData::_claim_stw_fullgc_mark, *this);
    CLDToOopClosure follow_cld_closure(&mark_and_push_closure, ClassLoaderData::_claim_stw_fullgc_mark);
    SCFollowRootClosure follow_root_closure(*this);

    CLDClosure* weak_cld_closure = ClassUnloading ? nullptr : &follow_cld_closure;
    MarkingCodeBlobClosure mark_code_closure(&follow_root_closure, !CodeBlobToOopClosure::FixRelocations, true);
    gch->process_roots(SerialHeap::SO_None,
                       &follow_root_closure,
                       &follow_cld_closure,
                       weak_cld_closure,
                       &mark_code_closure);
  }

  SCIsAliveClosure is_alive(_mark_bitmap);

  // Process reference objects found during marking
  {
    GCTraceTime(Debug, gc, phases) tm_m("Reference Processing", _gc_timer);

    SCKeepAliveClosure keep_alive(*this);
    SCFollowStackClosure follow_stack_closure(*this);
    ReferenceProcessorPhaseTimes pt(_gc_timer, _ref_processor->max_num_queues());
    SerialGCRefProcProxyTask task(is_alive, keep_alive, follow_stack_closure);
    const ReferenceProcessorStats& stats = _ref_processor->process_discovered_references(task, pt);
    pt.print_all_references();
    _gc_tracer.report_gc_reference_stats(stats);
  }

  // This is the point where the entire marking should have completed.
  assert(_marking_stack.is_empty(), "Marking should have completed");

  {
    GCTraceTime(Debug, gc, phases) tm_m("Weak Processing", _gc_timer);
    WeakProcessor::weak_oops_do(&is_alive, &do_nothing_cl);
  }

  {
    GCTraceTime(Debug, gc, phases) tm_m("Class Unloading", _gc_timer);

    ClassUnloadingContext* ctx = ClassUnloadingContext::context();

    bool unloading_occurred;
    {
      CodeCache::UnlinkingScope scope(&is_alive);

      // Unload classes and purge the SystemDictionary.
      unloading_occurred = SystemDictionary::do_unloading(_gc_timer);

      // Unload nmethods.
      CodeCache::do_unloading(unloading_occurred);
    }

    {
      GCTraceTime(Debug, gc, phases) t("Purge Unlinked NMethods", _gc_timer);
      // Release unloaded nmethod's memory.
      ctx->purge_nmethods();
    }
    {
      GCTraceTime(Debug, gc, phases) t("Free Code Blobs", _gc_timer);
      ctx->free_code_blobs();
    }

    // Prune dead klasses from subklass/sibling/implementor lists.
    Klass::clean_weak_klass_links(unloading_occurred);

    // Clean JVMCI metadata handles.
    JVMCI_ONLY(JVMCI::do_unloading(unloading_occurred));
  }

  {
    GCTraceTime(Debug, gc, phases) tm_m("Report Object Count", _gc_timer);
    _gc_tracer.report_object_count_after_gc(&is_alive, nullptr);
  }
}

void SerialCompressor::phase2_build_bot() {
  GCTraceTime(Info, gc, phases) tm("Phase 2: Build block-offset-table", _gc_timer);
  _bot.build_table();
}

class SCUpdateRefsClosure : public BasicOopIterateClosure {
private:
  SCBlockOffsetTable& _bot;

  template<class T>
  void do_oop_work(T* p) {
    T heap_oop = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_raw_not_null(heap_oop);
      assert(SerialHeap::heap()->is_in_reserved(obj), "should be in heap");
      oop forwardee = cast_to_oop(_bot.forwardee(cast_from_oop<HeapWord*>(obj)));
      RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);
    }
  }
public:
  SCUpdateRefsClosure(SCBlockOffsetTable& bot) :
    _bot(bot) {}

  void do_oop(oop* p) {
    do_oop_work(p);
  }
  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

static void clear_empty_region(ContiguousSpace* space) {
  // Let's remember if we were empty before we did the compaction.
  bool was_empty = space->used_region().is_empty();
  // Reset space after compaction is complete
  space->reset_after_compaction();
  // We do this clear, below, since it has overloaded meanings for some
  // space subtypes.  For example, TenuredSpace's that were
  // compacted into will have had their offset table thresholds updated
  // continuously, but those that weren't need to have their thresholds
  // re-initialized.  Also mangles unused area for debugging.
  if (space->used_region().is_empty()) {
    if (!was_empty) space->clear(SpaceDecorator::Mangle);
  } else {
    if (ZapUnusedHeapArea) space->mangle_unused_area();
  }
}

ContiguousSpace* space_containing(HeapWord* addr) {
  SerialHeap* heap = SerialHeap::heap();
  Generation* gen = heap->old_gen();
  if (!gen->is_in_reserved(addr)) {
    gen = heap->young_gen();
  }
  assert(gen->is_in_reserved(addr), "must be");
  ContiguousSpace* space = gen->first_compaction_space();
  while (!space->is_in_reserved(addr)) {
    assert(space != nullptr, "must succeed");
    space = space->next_compaction_space();
  }
  return space;
}

void SerialCompressor::compact_and_update_space(ContiguousSpace* space) {
  HeapWord* start = space->bottom();
  HeapWord* end = space->top();
  HeapWord* current = _mark_bitmap.get_next_marked_addr(start, end);
  HeapWord* new_top = start;
  SCUpdateRefsClosure cl(_bot);
  //HeapWord* next_fwd = start;
  while (current < end) {
    oop obj = cast_to_oop(current);
    assert(oopDesc::is_oop(obj), "must be oop");
    size_t size_in_words = obj->size();
    obj->oop_iterate(&cl);
    HeapWord* forwardee = _bot.forwardee(current);
    //assert(next_fwd == forwardee, "incorrect forwarwdee");
    //next_fwd = align_up(forwardee + size_in_words, MinObjAlignment);
    if (current != forwardee) {
      Copy::aligned_conjoint_words(current, forwardee, size_in_words);
    }

    // We need to update the offset table so that the beginnings of objects can be
    // found during scavenge.  Note that we are updating the offset table based on
    // where the object will be once the compaction phase finishes.
    space_containing(forwardee)->update_for_block(forwardee, forwardee + size_in_words);

    current = _mark_bitmap.get_next_marked_addr(current + size_in_words, end);
    new_top += size_in_words;
  }
  clear_empty_region(space);
}

void SerialCompressor::compact_and_update_generation(Generation* generation) {
  ContiguousSpace* space = generation->first_compaction_space();
  while (space != nullptr) {
    compact_and_update_space(space);
    space = space->next_compaction_space();
  }
}

void SerialCompressor::update_roots() {
  SerialHeap* heap = SerialHeap::heap();
  SCUpdateRefsClosure adjust_pointer_closure(_bot);
  CLDToOopClosure adjust_cld_closure(&adjust_pointer_closure, ClassLoaderData::_claim_stw_fullgc_adjust);
  CodeBlobToOopClosure code_closure(&adjust_pointer_closure, CodeBlobToOopClosure::FixRelocations);
  heap->process_roots(SerialHeap::SO_AllCodeCache,
                      &adjust_pointer_closure,
                      &adjust_cld_closure,
                      &adjust_cld_closure,
                      &code_closure);

  heap->gen_process_weak_roots(&adjust_pointer_closure);
}

void SerialCompressor::phase3_compact_and_update() {
  GCTraceTime(Info, gc, phases) tm("Phase 3: Compact heap", _gc_timer);
  ClassLoaderDataGraph::verify_claimed_marks_cleared(ClassLoaderData::_claim_stw_fullgc_adjust);
  update_roots();
  SerialHeap* heap = SerialHeap::heap();
  compact_and_update_generation(heap->old_gen());
  compact_and_update_generation(heap->young_gen());
}
