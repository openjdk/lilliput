/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#include "classfile/vmSymbols.hpp"
#include "javaThread.inline.hpp"
#include "jfrfiles/jfrEventClasses.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/basicLock.inline.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/lightweightSynchronizer.hpp"
#include "runtime/lockStack.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/perfData.inline.hpp"
#include "runtime/safepointMechanism.inline.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/synchronizer.hpp"
#include "utilities/concurrentHashTable.inline.hpp"
#include "utilities/globalDefinitions.hpp"


//
// Lightweight synchronization.
//
// When the lightweight synchronization needs to use a monitor the link
// between the object and the monitor is stored in a concurrent hash table
// instead of in the mark word. This has the benefit that it further decouples
// the mark word from the synchronization code.
//

// ConcurrentHashTable storing links from objects to ObjectMonitors
class ObjectMonitorWorld : public CHeapObj<mtOMWorld> {
  struct Config {
    using Value = ObjectMonitor*;
    static uintx get_hash(Value const& value, bool* is_dead) {
      return (uintx)value->hash();
    }
    static void* allocate_node(void* context, size_t size, Value const& value) {
      return AllocateHeap(size, mtOMWorld);
    };
    static void free_node(void* context, void* memory, Value const& value) {
      FreeHeap(memory);
    }
  };
  using ConcurrentTable = ConcurrentHashTable<Config, mtOMWorld>;

  ConcurrentTable* _table;
  volatile bool _resize;
  uint32_t _shrink_count;

  class Lookup : public StackObj {
    oop _obj;

  public:
    Lookup(oop obj) : _obj(obj) {}

    uintx get_hash() const {
      uintx hash = _obj->mark().hash();
      assert(hash != 0, "should have a hash");
      return hash;
    }

    bool equals(ObjectMonitor** value) {
      // The entry is going to be removed soon.
      assert(*value != nullptr, "must be");
      return (*value)->object_refers_to(_obj);
    }

    bool is_dead(ObjectMonitor** value) {
      assert(*value != nullptr, "must be");
      return (*value)->object_is_cleared();
    }
  };

  class LookupMonitor : public StackObj {
    ObjectMonitor* _monitor;

  public:
    LookupMonitor(ObjectMonitor* monitor) : _monitor(monitor) {}

    uintx get_hash() const {
      return _monitor->hash();
    }

    bool equals(ObjectMonitor** value) {
      return (*value) == _monitor;
    }

    bool is_dead(ObjectMonitor** value) {
      assert(*value != nullptr, "must be");
      return (*value)->object_is_dead();
    }
  };

  static size_t max_log_size() {
    // TODO[OMWorld]: Evaluate the max size.
    // TODO[OMWorld]: Need to fix init order to use Universe::heap()->max_capacity();
    //                Using MaxHeapSize directly this early may be wrong, and there
    //                are definitely rounding errors (alignment).
    const size_t max_capacity = MaxHeapSize;
    const size_t min_object_size = CollectedHeap::min_dummy_object_size() * HeapWordSize;
    const size_t max_objects = max_capacity / MAX2(MinObjAlignmentInBytes, checked_cast<int>(min_object_size));
    const size_t log_max_objects = log2i_graceful(max_objects);

    return MAX2(MIN2<size_t>(SIZE_BIG_LOG2, log_max_objects), min_log_size());
  }

  static size_t min_log_size() {
    // TODO[OMWorld]: Evaluate the min size, currently ~= log(AvgMonitorsPerThreadEstimate default)
    return 10;
  }

  template<typename V>
  static size_t clamp_log_size(V log_size) {
    return MAX2(MIN2(log_size, checked_cast<V>(max_log_size())), checked_cast<V>(min_log_size()));
  }

  static size_t initial_log_size() {
    const size_t estimate = log2i(MAX2(os::processor_count(), 1)) + log2i(MAX2(AvgMonitorsPerThreadEstimate, size_t(1)));
    return clamp_log_size(estimate);
  }

  static size_t grow_hint () {
    // TODO[OMWorld]: Evaluate why 4 is a good grow hint.
    //                Have seen grow hint hits when lower with a
    //                load factor as low as 0.1. (Grow Hint = 3)
    // TODO[OMWorld]: Evaluate the hash code used, are large buckets
    //                expected even with a low load factor. Or is it
    //                something with the hashing used.
    return ConcurrentTable::DEFAULT_GROW_HINT;
  }

  static size_t log_shrink_difference() {
    // TODO[OMWorld]: Evaluate shrink heuristics, currently disabled by
    //                default, and only really shrinks if AvgMonitorsPerThreadEstimate
    //                is also set to a none default value
    return 2;
  }

public:
  ObjectMonitorWorld()
  : _table(new ConcurrentTable(initial_log_size(), max_log_size(), grow_hint())),
    _resize(false),
    _shrink_count(0) {}

  void verify_monitor_get_result(oop obj, ObjectMonitor* monitor) {
#ifdef ASSERT
    if (SafepointSynchronize::is_at_safepoint()) {
      bool has_monitor = obj->mark().has_monitor();
      assert(has_monitor == (monitor != nullptr),
          "Inconsistency between markWord and OMW table has_monitor: %s monitor: " PTR_FORMAT,
          BOOL_TO_STR(has_monitor), p2i(monitor));
    }
#endif
  }

  ObjectMonitor* monitor_get(Thread* current, oop obj) {
    ObjectMonitor* result = nullptr;
    Lookup lookup_f(obj);
    auto found_f = [&](ObjectMonitor** found) {
      assert((*found)->object_peek() == obj, "must be");
      result = *found;
    };
    _table->get(current, lookup_f, found_f);
    verify_monitor_get_result(obj, result);
    return result;
  }

  void try_notify_grow() {
    if (!_table->is_max_size_reached() && !Atomic::load(&_resize)) {
      Atomic::store(&_resize, true);
      if (MonitorDeflation_lock->try_lock()) {
        MonitorDeflation_lock->notify();
        MonitorDeflation_lock->unlock();
      }
    }
  }

  void set_table_max(JavaThread* current) {
    while (!_table->is_max_size_reached()) {
      _table->grow(current);
    }
  }

  bool needs_shrink(size_t log_target, size_t log_size) {
    return OMShrinkCHT && log_target + log_shrink_difference() <= log_size;
  }

  bool needs_grow(size_t log_target, size_t log_size) {
    return log_size < log_target;
  }

  bool needs_resize(JavaThread* current, size_t ceiling, size_t count, size_t max) {
    const size_t log_size = _table->get_size_log2(current);
    const int log_ceiling = log2i_graceful(ceiling);
    const int log_max = log2i_graceful(max);
    const size_t log_count = log2i(MAX2(count, size_t(1)));
    const size_t log_target = clamp_log_size(MAX2(log_ceiling, log_max) + 2);

    return needs_grow(log_target, log_size) || needs_shrink(log_target, log_size) || Atomic::load(&_resize);
  }

  bool resize(JavaThread* current, size_t ceiling, size_t count, size_t max) {
    const size_t log_size = _table->get_size_log2(current);
    const int log_ceiling = log2i_graceful(ceiling);
    const int log_max = log2i_graceful(max);
    const size_t log_count = log2i(MAX2(count, size_t(1)));
    const size_t log_target = clamp_log_size(MAX2(log_ceiling, log_max) + 2);
    LogTarget(Info, monitorinflation) lt;

    auto print_table_stats = [&]() {
      ResourceMark rm;
      LogStream ls(lt);
      auto vs_f = [](Config::Value* v) { return sizeof(Config::Value); };
      _table->statistics_to(current, vs_f, &ls, "ObjectMonitorWorld");
    };

    bool success = true;

    if (needs_grow(log_target, log_size)) {
      // Grow
      lt.print("Growing to %02zu->%02zu", log_size, log_target);
      success = _table->grow(current, log_target);
      print_table_stats();
    } else if (!_table->is_max_size_reached() && Atomic::load(&_resize)) {
      lt.print("WARNING: Getting resize hints with Size: %02zu Ceiling: %2i Target: %02zu", log_size, log_ceiling, log_target);
      print_table_stats();
      success = false;
    }

    if (needs_shrink(log_target, log_size)) {
      _shrink_count++;
      // Shrink
      lt.print("Shrinking to %02zu->%02zu", log_size, log_target);
      success = _table->shrink(current, log_target);
      print_table_stats();
    }

    if (success) {
      Atomic::store(&_resize, _table->is_max_size_reached());
    }

    return success;
  }

  ObjectMonitor* monitor_put_get(Thread* current, ObjectMonitor* monitor, oop obj) {
    // Enter the monitor into the concurrent hashtable.
    ObjectMonitor* result = monitor;
    Lookup lookup_f(obj);
    auto found_f = [&](ObjectMonitor** found) {
      assert((*found)->object_peek() == obj, "must be");
      result = *found;
    };
    bool grow;
    _table->insert_get(current, lookup_f, monitor, found_f, &grow);
    verify_monitor_get_result(obj, result);
    if (grow) {
      try_notify_grow();
    }
    return result;
  }

  bool remove_monitor_entry(Thread* current, ObjectMonitor* monitor) {
    LookupMonitor lookup_f(monitor);
    return _table->remove(current, lookup_f);
  }

  bool contains_monitor(Thread* current, ObjectMonitor* monitor) {
    LookupMonitor lookup_f(monitor);
    bool result = false;
    auto found_f = [&](ObjectMonitor** found) {
      result = true;
    };
    _table->get(current, lookup_f, found_f);
    return result;
  }

  void print_on(outputStream* st) {
    auto printer = [&] (ObjectMonitor** entry) {
       ObjectMonitor* om = *entry;
       oop obj = om->object_peek();
       st->print("monitor " PTR_FORMAT " ", p2i(om));
       st->print("object " PTR_FORMAT, p2i(obj));
       assert(obj->mark().hash() == om->hash(), "hash must match");
       st->cr();
       return true;
    };
    if (SafepointSynchronize::is_at_safepoint()) {
      _table->do_safepoint_scan(printer);
    } else {
      _table->do_scan(Thread::current(), printer);
    }
  }
};

ObjectMonitorWorld* LightweightSynchronizer::_omworld = nullptr;

ObjectMonitor* LightweightSynchronizer::get_or_insert_monitor_from_table(oop object, JavaThread* current, bool try_read, bool* inserted) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");

  if (try_read) {
    ObjectMonitor* monitor = read_monitor(current, object);
    if (monitor != nullptr) {
      *inserted = false;
      return monitor;
    }
  }

  ObjectMonitor* alloced_monitor = new ObjectMonitor(object);
  alloced_monitor->set_owner_anonymous();

  // Try insert monitor
  ObjectMonitor* monitor = add_monitor(current, alloced_monitor, object);

  *inserted = alloced_monitor == monitor;
  if (!*inserted) {
    delete alloced_monitor;
  }

  return monitor;
}

static void log_inflate(Thread* current, oop object, const ObjectSynchronizer::InflateCause cause) {
  if (log_is_enabled(Trace, monitorinflation)) {
    ResourceMark rm(current);
    log_info(monitorinflation)("inflate(has_locker): object=" INTPTR_FORMAT ", mark="
                               INTPTR_FORMAT ", type='%s' cause %s", p2i(object),
                               object->mark().value(), object->klass()->external_name(),
                               ObjectSynchronizer::inflate_cause_name(cause));
  }
}

static void post_monitor_inflate_event(EventJavaMonitorInflate* event,
                                       const oop obj,
                                       ObjectSynchronizer::InflateCause cause) {
  assert(event != nullptr, "invariant");
  event->set_monitorClass(obj->klass());
  event->set_address((uintptr_t)(void*)obj);
  event->set_cause((u1)cause);
  event->commit();
}

ObjectMonitor* LightweightSynchronizer::get_or_insert_monitor(oop object, JavaThread* current, const ObjectSynchronizer::InflateCause cause, bool try_read) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");

  EventJavaMonitorInflate event;

  bool inserted;
  ObjectMonitor* monitor = get_or_insert_monitor_from_table(object, current, try_read, &inserted);

  if (inserted) {
    // Hopefully the performance counters are allocated on distinct
    // cache lines to avoid false sharing on MP systems ...
    OM_PERFDATA_OP(Inflations, inc());
    log_inflate(current, object, cause);
    if (event.should_commit()) {
      post_monitor_inflate_event(&event, object, cause);
    }

    // The monitor has an anonymous owner so it is safe from async deflation.
    ObjectSynchronizer::_in_use_list.add(monitor);
  }

  return monitor;
}

// Add the hashcode to the monitor to match the object and put it in the hashtable.
ObjectMonitor* LightweightSynchronizer::add_monitor(JavaThread* current, ObjectMonitor* monitor, oop obj) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  assert(obj == monitor->object(), "must be");

  intptr_t hash = obj->mark().hash();
  assert(hash != 0, "must be set when claiming the object monitor");
  monitor->set_hash(hash);

  return _omworld->monitor_put_get(current, monitor, obj);
}

bool LightweightSynchronizer::remove_monitor(Thread* current, oop obj, ObjectMonitor* monitor) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  assert(monitor->object_peek() == obj, "must be, cleared objects are removed by is_dead");

  return _omworld->remove_monitor_entry(current, monitor);
}

void LightweightSynchronizer::deflate_mark_word(oop obj) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must use lightweight locking");

  markWord mark = obj->mark_acquire();
  assert(!mark.has_no_hash(), "obj with inflated monitor must have had a hash");

  while (mark.has_monitor()) {
    const markWord new_mark = mark.clear_lock_bits().set_unlocked();
    mark = obj->cas_set_mark(new_mark, mark);
  }
}

void LightweightSynchronizer::initialize() {
  _omworld = new ObjectMonitorWorld();

  if (!FLAG_IS_CMDLINE(AvgMonitorsPerThreadEstimate)) {
    // This is updated after ceiling is set and ObjectMonitorWorld is created;
    // TODO[OMWorld]: Clean this up and find a good initial ceiling,
    //                and initial HashTable size
    FLAG_SET_ERGO(AvgMonitorsPerThreadEstimate, 0);
  }
}

void LightweightSynchronizer::set_table_max(JavaThread* current) {
  if (LockingMode != LM_LIGHTWEIGHT) {
    return;
  }
  _omworld->set_table_max(current);
}

bool LightweightSynchronizer::needs_resize(JavaThread *current) {
  if (LockingMode != LM_LIGHTWEIGHT) {
    return false;
  }
  return _omworld->needs_resize(current,
                                  ObjectSynchronizer::in_use_list_ceiling(),
                                  ObjectSynchronizer::_in_use_list.count(),
                                  ObjectSynchronizer::_in_use_list.max());
}

bool LightweightSynchronizer::resize_table(JavaThread* current) {
  if (LockingMode != LM_LIGHTWEIGHT) {
    return true;
  }
  return _omworld->resize(current,
                          ObjectSynchronizer::in_use_list_ceiling(),
                          ObjectSynchronizer::_in_use_list.count(),
                          ObjectSynchronizer::_in_use_list.max());
}

class LockStackInflateContendedLocks : private OopClosure {
 private:
  oop _contended_oops[LockStack::CAPACITY];
  int _length;

  void do_oop(oop* o) final {
    oop obj = *o;
    if (obj->mark_acquire().has_monitor()) {
      if (_length > 0 && _contended_oops[_length-1] == obj) {
        // assert(VM_Version::supports_recursive_lightweight_locking(), "must be");
        // Recursive
        return;
      }
      _contended_oops[_length++] = obj;
    }
  }

  void do_oop(narrowOop* o) final {
    ShouldNotReachHere();
  }

 public:
  LockStackInflateContendedLocks() :
    _contended_oops(),
    _length(0) {};

  void inflate(JavaThread* locking_thread, JavaThread* current) {
    locking_thread->lock_stack().oops_do(this);
    for (int i = 0; i < _length; i++) {
      LightweightSynchronizer::
        inflate_fast_locked_object(_contended_oops[i], locking_thread, current, ObjectSynchronizer::inflate_cause_vm_internal);
    }
  }
};

void LightweightSynchronizer::ensure_lock_stack_space(JavaThread* current) {
  assert(current == JavaThread::current(), "must be");
  LockStack& lock_stack = current->lock_stack();

  // Make room on lock_stack
  if (lock_stack.is_full()) {
    // Inflate contented objects
    LockStackInflateContendedLocks().inflate(current, current);
    if (lock_stack.is_full()) {
      // Inflate the oldest object
      inflate_fast_locked_object(lock_stack.bottom(), current, current, ObjectSynchronizer::inflate_cause_vm_internal);
    }
  }
}

class LightweightSynchronizer::CacheSetter : StackObj {
  JavaThread* const _thread;
  BasicLock* const _lock;
  ObjectMonitor* _monitor;

  NONCOPYABLE(CacheSetter);

public:
  CacheSetter(JavaThread* thread, BasicLock* lock) :
    _thread(thread),
    _lock(lock),
    _monitor(nullptr) {}

  ~CacheSetter() {
    if (_monitor != nullptr) {
      _thread->om_set_monitor_cache(_monitor);
      _lock->set_object_monitor_cache(_monitor);
    } else {
      _lock->clear_object_monitor_cache();
    }
  }

  void set_monitor(ObjectMonitor* monitor) {
    assert(_monitor == nullptr, "only set once");
    _monitor = monitor;
  }

};

class VerifyThreadState {
  bool _no_safepoint;
  union {
    struct {} _dummy;
    NoSafepointVerifier _nsv;
  };

public:
  VerifyThreadState(JavaThread* locking_thread, JavaThread* current) : _no_safepoint(locking_thread != current) {
    assert(current == Thread::current(), "must be");
    assert(locking_thread == current || locking_thread->is_obj_deopt_suspend(), "locking_thread may not run concurrently");
    if (_no_safepoint) {
      ::new (&_nsv) NoSafepointVerifier();
    }
  }
  ~VerifyThreadState() {
    if (_no_safepoint){
      _nsv.~NoSafepointVerifier();
    }
  }
};

bool LightweightSynchronizer::fast_lock_spin_enter(oop obj, JavaThread* current, bool observed_deflation) {
  // Will spin with exponential backoff with an accumulative O(2^spin_limit) spins.
  const int log_spin_limit = os::is_MP() ? OMSpins : 1;
  const int log_min_safepoint_check_interval = 10;

  LockStack& lock_stack = current->lock_stack();

  markWord mark = obj->mark();
  const auto should_spin = [&]() {
    if (!mark.has_monitor()) {
      // Spin while not inflated.
      return true;
    } else if (observed_deflation) {
      // Spin while monitor is being deflated.
      ObjectMonitor* monitor = LightweightSynchronizer::read_monitor(current, obj);
      return monitor == nullptr || monitor->is_being_async_deflated();
    }
    // Else stop spinning.
    return false;
  };
  // Always attempt to lock once even when safepoint synchronizing.
  bool should_process = false;
  for (int i = 0; should_spin() && !should_process && i < log_spin_limit; i++) {
    // Spin with exponential backoff.
    const int total_spin_count = 1 << i;
    const int inner_spin_count = MIN2(1 << log_min_safepoint_check_interval, total_spin_count);
    const int outer_spin_count = total_spin_count / inner_spin_count;
    for (int outer = 0; outer < outer_spin_count; outer++) {
      should_process = SafepointMechanism::should_process(current);
      if (should_process) {
        // Stop spinning for safepoint.
        break;
      }
      for (int inner = 1; inner < inner_spin_count; inner++) {
        SpinPause();
      }
    }

    mark = obj->mark();
    while (mark.is_unlocked()) {
      ensure_lock_stack_space(current);
      assert(!lock_stack.is_full(), "must have made room on the lock stack");
      assert(!lock_stack.contains(obj), "thread must not already hold the lock");
      // Try to swing into 'fast-locked' state.
      markWord locked_mark = mark.set_fast_locked();
      markWord old_mark = mark;
      mark = obj->cas_set_mark(locked_mark, old_mark);
      if (old_mark == mark) {
        // Successfully fast-locked, push object to lock-stack and return.
        lock_stack.push(obj);
        return true;
      }
    }
  }
  return false;
}

void LightweightSynchronizer::enter_for(Handle obj, BasicLock* lock, JavaThread* locking_thread) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  JavaThread* current = JavaThread::current();
  VerifyThreadState vts(locking_thread, current);

  // TODO[OMWorld]: Is this necessary?
  if (obj->klass()->is_value_based()) {
    ObjectSynchronizer::handle_sync_on_value_based_class(obj, locking_thread);
  }

  locking_thread->inc_held_monitor_count();

  CacheSetter cache_setter(locking_thread, lock);

  LockStack& lock_stack = locking_thread->lock_stack();

  ObjectMonitor* monitor = nullptr;
  if (lock_stack.contains(obj())) {
    monitor = inflate_fast_locked_object(obj(), locking_thread, current, ObjectSynchronizer::inflate_cause_monitor_enter);
    bool entered = monitor->enter_for(locking_thread);
    assert(entered, "recursive ObjectMonitor::enter_for must succeed");
  } else {
    // It is assumed that enter_for must enter on an object without contention.
    // TODO[OMWorld]: We also assume that this re-lock is on either a new never
    //                inflated monitor, or one that is already locked by the
    //                locking_thread. Should we have this stricter restriction?
    monitor = inflate_and_enter(obj(), locking_thread, current, ObjectSynchronizer::inflate_cause_monitor_enter);
  }

  assert(monitor != nullptr, "LightweightSynchronizer::enter_for must succeed");
  cache_setter.set_monitor(monitor);
}

void LightweightSynchronizer::enter(Handle obj, BasicLock* lock, JavaThread* current) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  assert(current == JavaThread::current(), "must be");

  if (obj->klass()->is_value_based()) {
    ObjectSynchronizer::handle_sync_on_value_based_class(obj, current);
  }

  current->inc_held_monitor_count();

  CacheSetter cache_setter(current, lock);

  // Used when deflation is observed. Progress here requires progress
  // from the deflator. After observing the that the deflator is not
  // making progress (after two yields), switch to sleeping.
  SpinYield spin_yield(0, 2);
  bool observed_deflation = false;

  LockStack& lock_stack = current->lock_stack();

  if (!lock_stack.is_full() && lock_stack.try_recursive_enter(obj())) {
    // TODO[OMWorld]: Maybe guard this by the value in the markWord (only is fast locked)
    //                Currently this is done when exiting. Doing it early could remove,
    //                LockStack::CAPACITY - 1 slow paths in the best case. But need to fix
    //                some of the inflation counters for this change.

    // Recursively fast locked
    return;
  }

  if (lock_stack.contains(obj())) {
    ObjectMonitor* monitor = inflate_fast_locked_object(obj(), current, current, ObjectSynchronizer::inflate_cause_monitor_enter);
    bool entered = monitor->enter(current);
    assert(entered, "recursive ObjectMonitor::enter must succeed");
    cache_setter.set_monitor(monitor);
    return;
  }

  while (true) {
    // Fast-locking does not use the 'lock' argument.
    if (fast_lock_spin_enter(obj(), current, observed_deflation)) {
      return;
    }

    if (observed_deflation) {
      spin_yield.wait();
    }

    ObjectMonitor* monitor = inflate_and_enter(obj(), current, current, ObjectSynchronizer::inflate_cause_monitor_enter);
    if (monitor != nullptr) {
      cache_setter.set_monitor(monitor);
      return;
    }

    // If inflate_and_enter returns nullptr it is because a deflated monitor
    // was encountered. Fallback to fast locking. The deflater is responsible
    // for clearing out the monitor and transitioning the markWord back to
    // fast locking.
    observed_deflation = true;
  }
}

void LightweightSynchronizer::exit(oop object, JavaThread* current) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  assert(current == Thread::current(), "must be");

  markWord mark = object->mark();
  assert(!mark.is_unlocked(), "must be unlocked");

  LockStack& lock_stack = current->lock_stack();
  if (mark.is_fast_locked()) {
    if (lock_stack.try_recursive_exit(object)) {
      // This is a recursive exit which succeeded
      return;
    }
    if (lock_stack.is_recursive(object)) {
      // Must inflate recursive locks if try_recursive_exit fails
      // This happens for un-structured unlocks, could potentially
      // fix try_recursive_exit to handle these.
      inflate_fast_locked_object(object, current, current, ObjectSynchronizer::inflate_cause_vm_internal);
    }
  }

  // Fast-locking does not use the 'lock' argument.
  while (mark.is_fast_locked()) {
    markWord unlocked_mark = mark.set_unlocked();
    markWord old_mark = mark;
    mark = object->cas_set_mark(unlocked_mark, old_mark);
    if (old_mark == mark) {
      // CAS successful, remove from lock_stack
      size_t recursion = lock_stack.remove(object) - 1;
      assert(recursion == 0, "Should not have unlocked here");
      return;
    }
  }

  assert(mark.has_monitor(), "must be");
  // The monitor is
  ObjectMonitor* monitor = read_monitor(current, object);
  if (monitor->is_owner_anonymous()) {
    assert(current->lock_stack().contains(object), "current must have object on its lock stack");
    monitor->set_owner_from_anonymous(current);
    monitor->set_recursions(current->lock_stack().remove(object) - 1);
    current->_contended_inflation++;
  }

  monitor->exit(current);
}

// TODO[OMWorld]: Rename this. No idea what to call it, used by notify/notifyall/wait and jni exit
ObjectMonitor* LightweightSynchronizer::inflate_locked_or_imse(oop obj, const ObjectSynchronizer::InflateCause cause, TRAPS) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  JavaThread* current = THREAD;

  for(;;) {
    markWord mark = obj->mark_acquire();
    if (mark.is_unlocked()) {
      // No lock, IMSE.
      THROW_MSG_(vmSymbols::java_lang_IllegalMonitorStateException(),
                "current thread is not owner", nullptr);
    }

    if (mark.is_fast_locked()) {
      if (!current->lock_stack().contains(obj)) {
        // Fast locked by other thread, IMSE.
        THROW_MSG_(vmSymbols::java_lang_IllegalMonitorStateException(),
                  "current thread is not owner", nullptr);
      } else {
        // Current thread owns the lock, must inflate
        return inflate_fast_locked_object(obj, current, current, cause);
      }
    }

    assert(mark.has_monitor(), "must be");
    ObjectMonitor* monitor = read_monitor(current, obj);
    if (monitor != nullptr) {
      if (monitor->is_owner_anonymous()) {
        LockStack& lock_stack = current->lock_stack();
        if (lock_stack.contains(obj)) {
          // Current thread owns the lock but someone else inflated
          // fix owner and pop lock stack
          monitor->set_owner_from_anonymous(current);
          monitor->set_recursions(lock_stack.remove(obj) - 1);
          current->_contended_inflation++;
        } else {
          // Fast locked (and inflated) by other thread, or deflation in progress, IMSE.
          THROW_MSG_(vmSymbols::java_lang_IllegalMonitorStateException(),
                    "current thread is not owner", nullptr);
        }
      }
      return monitor;
    }
  }
}

ObjectMonitor* LightweightSynchronizer::inflate_fast_locked_object(oop object, JavaThread* locking_thread, JavaThread* current, const ObjectSynchronizer::InflateCause cause) {
  assert(LockingMode == LM_LIGHTWEIGHT, "only used for lightweight");
  VerifyThreadState vts(locking_thread, current);
  assert(locking_thread->lock_stack().contains(object), "locking_thread must have object on its lock stack");

  // Inflating requires a hash code
  FastHashCode(current, object);

  markWord mark = object->mark_acquire();
  assert(!mark.is_unlocked(), "Cannot be unlocked");

  ObjectMonitor* monitor;

  for (;;) {
  // Fetch the monitor from the table
    monitor = get_or_insert_monitor(object, current, cause, true /* try_read */);

    if (monitor->is_owner_anonymous()) {
      assert(monitor == read_monitor(current, object), "The monitor must be in the table");
      // New fresh monitor
      break;
    }

    os::naked_yield();
    assert(monitor->is_being_async_deflated(), "Should be the reason");
  }

  // Set the mark word; loop to handle concurrent updates to other parts of the mark word
  while (mark.is_fast_locked()) {
    mark = object->cas_set_mark(mark.set_has_monitor(), mark);
  }

  // Indicate that the monitor now has a known owner
  monitor->set_owner_from_anonymous(locking_thread);

  // Remove the entry from the thread's lock stack
  monitor->set_recursions(locking_thread->lock_stack().remove(object) - 1);

  if (locking_thread == current) {
    locking_thread->om_set_monitor_cache(monitor);
  }

  if (cause == ObjectSynchronizer::inflate_cause_wait) {
    locking_thread->_wait_inflation++;
  } else if (cause == ObjectSynchronizer::inflate_cause_monitor_enter) {
    locking_thread->_recursive_inflation++;
  } else if (cause == ObjectSynchronizer::inflate_cause_vm_internal) {
    locking_thread->_lock_stack_inflation++;
  }

  return monitor;
}

ObjectMonitor* LightweightSynchronizer::inflate_and_enter(oop object, JavaThread* locking_thread, JavaThread* current, const ObjectSynchronizer::InflateCause cause) {
  assert(LockingMode == LM_LIGHTWEIGHT, "only used for lightweight");
  VerifyThreadState vts(locking_thread, current);
  NoSafepointVerifier nsv;

  // Note: In some paths (deoptimization) the 'current' thread inflates and
  // enters the lock on behalf of the 'locking_thread' thread.

  // Lightweight monitors require that hash codes are installed first
  FastHashCode(locking_thread, object);

  ObjectMonitor* monitor = nullptr;

  // Try to get the monitor from the thread-local cache.
  // There's no need to use the cache if we are locking
  // on behalf of another thread.
  if (current == locking_thread) {
    monitor = current->om_get_from_monitor_cache(object);
  }

  // Get or create the monitor
  if (monitor == nullptr) {
    monitor = get_or_insert_monitor(object, current, cause, true /* try_read */);
  }

  if (monitor->try_enter(locking_thread)) {
    return monitor;
  }

  // Holds is_being_async_deflated() stable throughout this function.
  ObjectMonitorContentionMark contention_mark(monitor);

  /// First handle the case where the monitor from the table is deflated
  if (monitor->is_being_async_deflated()) {
    // The MonitorDeflation thread is deflating the monitor. The locking thread
    // can either help transition the mark word or yield / spin until further
    // progress have been made.

    const markWord mark = object->mark_acquire();

    if (mark.has_monitor()) {
      // Waiting on the deflation thread to remove the deflated monitor from the table.
      os::naked_yield();

    } else if (mark.is_fast_locked()) {
      // Some other thread managed to fast-lock the lock, or this is a
      // recursive lock from the same thread; yield for the deflation
      // thread to remove the deflated monitor from the table.
      os::naked_yield();

    } else {
      assert(mark.is_unlocked(), "Implied");
      // Retry immediately
    }

    // Retry
    return nullptr;
  }

  for (;;) {
    const markWord mark = object->mark_acquire();
    // The mark can be in one of the following states:
    // *  inflated     - If the ObjectMonitor owner is anonymous
    //                   and the locking_thread thread owns the object
    //                   lock, then we make the locking_thread thread
    //                   the ObjectMonitor owner and remove the
    //                   lock from the locking_thread thread's lock stack.
    // *  fast-locked  - Coerce it to inflated from fast-locked.
    // *  neutral      - Inflate the object. Successful CAS is locked

    // CASE: inflated
    if (mark.has_monitor()) {
      LockStack& lock_stack = locking_thread->lock_stack();
      if (monitor->is_owner_anonymous() && lock_stack.contains(object)) {
        // The lock is fast-locked by the locking thread,
        // convert it to a held monitor with a known owner.
        monitor->set_owner_from_anonymous(locking_thread);
        monitor->set_recursions(lock_stack.remove(object) - 1);
        locking_thread->_contended_recursive_inflation++;
      }

      break; // Success
    }

    // CASE: fast-locked
    // Could be fast-locked either by locking_thread or by some other thread.
    //
    if (mark.is_fast_locked()) {
      markWord old_mark = object->cas_set_mark(mark.set_has_monitor(), mark);
      if (old_mark != mark) {
        // CAS failed
        continue;
      }

      // Success! Return inflated monitor.
      LockStack& lock_stack = locking_thread->lock_stack();
      if (lock_stack.contains(object)) {
        // The lock is fast-locked by the locking thread,
        // convert it to a held monitor with a known owner.
        monitor->set_owner_from_anonymous(locking_thread);
        monitor->set_recursions(lock_stack.remove(object) - 1);
        locking_thread->_recursive_inflation++;
      }

      break; // Success
    }

    // CASE: neutral (unlocked)

    // Catch if the object's header is not neutral (not locked and
    // not marked is what we care about here).
    assert(mark.is_neutral(), "invariant: header=" INTPTR_FORMAT, mark.value());
    markWord old_mark = object->cas_set_mark(mark.set_has_monitor(), mark);
    if (old_mark != mark) {
      // CAS failed
      continue;
    }

    // Transitioned from unlocked to monitor means locking_thread owns the lock.
    monitor->set_owner_from_anonymous(locking_thread);

    locking_thread->_unlocked_inflation++;

    return monitor;
  }

  if (current == locking_thread) {
    // One round of spinning
    if (monitor->spin_enter(locking_thread)) {
      return monitor;
    }

    // Monitor is contended, take the time befor entering to fix the lock stack.
    LockStackInflateContendedLocks().inflate(locking_thread, current);
  }

  // enter can block for safepoints; clear the unhandled object oop
  PauseNoSafepointVerifier pnsv(&nsv);
  object = nullptr;

  if (current == locking_thread) {
    monitor->enter_with_contention_mark(locking_thread, contention_mark);
  } else {
    monitor->enter_for_with_contention_mark(locking_thread, contention_mark);
  }

  return monitor;
}

void LightweightSynchronizer::deflate_monitor(Thread* current, oop obj, ObjectMonitor* monitor) {
  if (obj != nullptr) {
    deflate_mark_word(obj);
  }
  bool removed = remove_monitor(current, obj, monitor);
  if (obj != nullptr) {
    assert(removed, "Should have removed the entry if obj was alive");
  }
}

void LightweightSynchronizer::deflate_anon_monitor(Thread* current, oop obj, ObjectMonitor* monitor) {
  markWord mark = obj->mark_acquire();
  assert(!mark.has_no_hash(), "obj with inflated monitor must have had a hash");

  while (mark.has_monitor()) {
    const markWord new_mark = mark.set_fast_locked();
    mark = obj->cas_set_mark(new_mark, mark);
  }

  bool removed = remove_monitor(current, obj, monitor);
  assert(removed, "Should have removed the entry");
}

ObjectMonitor* LightweightSynchronizer::read_monitor(Thread* current, oop obj) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  return _omworld->monitor_get(current, obj);
}

bool LightweightSynchronizer::contains_monitor(Thread* current, ObjectMonitor* monitor) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  return _omworld->contains_monitor(current, monitor);
}

intptr_t LightweightSynchronizer::FastHashCode(Thread* current, oop obj) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");

  markWord mark = obj->mark_acquire();
  for(;;) {
    intptr_t hash = mark.hash();
    if (hash != 0) {
      return hash;
    }

    hash = ObjectSynchronizer::get_next_hash(current, obj);
    const markWord old_mark = mark;
    const markWord new_mark = old_mark.copy_set_hash(hash);

    mark = obj->cas_set_mark(new_mark, old_mark);
    if (old_mark == mark) {
      return hash;
    }
  }
}

bool LightweightSynchronizer::quick_enter(oop obj, JavaThread* current, BasicLock* lock) {
  assert(LockingMode == LM_LIGHTWEIGHT, "must be");
  assert(current->thread_state() == _thread_in_Java, "must be");
  assert(obj != nullptr, "must be");
  NoSafepointVerifier nsv;

  CacheSetter cache_setter(current, lock);

  LockStack& lock_stack = current->lock_stack();
  if (lock_stack.is_full()) {
    // Always go into runtime if the lock stack is full.
    return false;
  }

  if (lock_stack.try_recursive_enter(obj)) {
    // Recursive lock successful.
    current->inc_held_monitor_count();
    return true;
  }

  const markWord mark = obj->mark();

  if (mark.has_monitor()) {
    ObjectMonitor* const monitor = current->om_get_from_monitor_cache(obj);

    if (monitor == nullptr) {
      // Take the slow-path on a cache miss.
      return false;
    }

    if (monitor->try_enter(current)) {
      // ObjectMonitor enter successful.
      cache_setter.set_monitor(monitor);
      current->inc_held_monitor_count();
      return true;
    }
  }

  // Slow-path.
  return false;
}
