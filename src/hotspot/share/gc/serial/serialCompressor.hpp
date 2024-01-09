
#ifndef SHARE_GC_SERIAL_SERIALCOMPRESSOR_HPP
#define SHARE_GC_SERIAL_SERIALCOMPRESSOR_HPP

#include "gc/shared/gcTrace.hpp"
#include "gc/shared/markBitMap.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"
#include "utilities/stack.hpp"

class ContiguousSpace;
class Generation;
class Space;
class STWGCTimer;

class SCBlockOffsetTable {
private:
  static const int WORDS_PER_BLOCK = 64;

  HeapWord** _table;
  MarkBitMap& _mark_bitmap;
  MemRegion _covered;

  inline size_t addr_to_block_idx(HeapWord* addr) const;

  void build_table_for_space(ContiguousSpace* space);
  void build_table_for_generation(Generation* generation);

public:
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

  SCBlockOffsetTable _bot;

  STWGCTimer* _gc_timer;
  SerialOldTracer _gc_tracer;

  void phase1_mark(bool clear_all_softrefs);
  void phase2_build_bot();
  void phase3_compact_and_update();

  bool mark_object(oop obj);
  void follow_object(oop obj);

  void update_roots();
  void compact_and_update_space(ContiguousSpace* space);
  void compact_and_update_generation(Generation* generation);

public:
  SerialCompressor(STWGCTimer* gc_timer);
  ~SerialCompressor();

  // TODO: better scoping?
  void follow_stack();
  template<class T>
  void mark_and_push(T* p);

  void invoke_at_safepoint(bool clear_all_softrefs);
};

#endif // SHARE_GC_SERIAL_SERIALCOMPRESSOR_HPP
