
#ifndef SHARE_GC_SERIAL_SERIALCOMPRESSOR_HPP
#define SHARE_GC_SERIAL_SERIALCOMPRESSOR_HPP

#include "gc/shared/gcTrace.hpp"
#include "gc/shared/markBitMap.hpp"
#include "gc/shared/space.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/taskqueue.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "utilities/stack.hpp"

class ContiguousSpace;
class Generation;
class Space;
class STWGCTimer;

class SCBlockOffsetTable {
private:
  HeapWord** _table;
  MarkBitMap& _mark_bitmap;
  MemRegion _covered;

  inline size_t addr_to_block_idx(HeapWord* addr) const;

  void build_table_for_space(CompactPoint& cp, ContiguousSpace* space);
  void build_table_for_generation(CompactPoint& cp, Generation* generation);

public:
  static int words_per_block() {
    return BitsPerWord << LogMinObjAlignment;
  }

  SCBlockOffsetTable(MarkBitMap& mark_bitmap);
  ~SCBlockOffsetTable();

  void build_table();

  inline HeapWord* forwardee(HeapWord* addr) const;
};

class SerialCompressor : public StackObj {
private:

  MemRegion  _mark_bitmap_region;
  MarkBitMap _mark_bitmap;
  Stack<oop,mtGC> _marking_stack;
  Stack<ObjArrayTask, mtGC> _objarray_stack;

  SCBlockOffsetTable _bot;

  StringDedup::Requests _string_dedup_requests;

  STWGCTimer* _gc_timer;
  SerialOldTracer _gc_tracer;
  ReferenceProcessor* _ref_processor;

  void phase1_mark(bool clear_all_softrefs);
  void phase2_build_bot();
  void phase3_compact_and_update();

  bool mark_object(oop obj);
  void follow_array(objArrayOop array);
  void follow_array_chunk(objArrayOop array, int index);
  void follow_object(oop obj);
  void push_objarray(objArrayOop array, size_t index);

  void update_roots();
  void compact_and_update_space(ContiguousSpace* space);
  void compact_and_update_generation(Generation* generation);

public:
  SerialCompressor(STWGCTimer* gc_timer);
  ~SerialCompressor();

  ReferenceProcessor* ref_processor() {
    return _ref_processor;
  }

  void follow_stack();
  template<class T>
  void mark_and_push(T* p);
  void invoke_at_safepoint(bool clear_all_softrefs);
};

#endif // SHARE_GC_SERIAL_SERIALCOMPRESSOR_HPP
