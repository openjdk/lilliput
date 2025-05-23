/*
 * Copyright (c) 1998, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_SYNCHRONIZER_HPP
#define SHARE_RUNTIME_SYNCHRONIZER_HPP

#include "memory/padded.hpp"
#include "oops/markWord.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/handles.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/resourceHash.hpp"

template <typename T> class GrowableArray;
class LogStream;
class ObjectMonitor;
class ObjectMonitorDeflationSafepointer;
class ThreadsList;

class MonitorList {
  friend class VMStructs;

private:
  ObjectMonitor* volatile _head;
  volatile size_t _count;
  volatile size_t _max;

public:
  MonitorList() : _head(nullptr), _count(0), _max(0) {};
  void add(ObjectMonitor* monitor);
  size_t unlink_deflated(size_t deflated_count,
                         GrowableArray<ObjectMonitor*>* unlinked_list,
                         ObjectMonitorDeflationSafepointer* safepointer);
  size_t count() const;
  size_t max() const;

  class Iterator;
  Iterator iterator() const;
};

class MonitorList::Iterator {
  ObjectMonitor* _current;

public:
  Iterator(ObjectMonitor* head) : _current(head) {}
  bool has_next() const { return _current != nullptr; }
  ObjectMonitor* next();
};

class ObjectSynchronizer : AllStatic {
  friend class VMStructs;
  friend class ObjectMonitorDeflationLogging;
  friend class WhiteBox;

 public:
  typedef enum {
    inflate_cause_vm_internal = 0,
    inflate_cause_monitor_enter = 1,
    inflate_cause_wait = 2,
    inflate_cause_notify = 3,
    inflate_cause_hash_code = 4,
    inflate_cause_jni_enter = 5,
    inflate_cause_jni_exit = 6,
    inflate_cause_nof = 7 // Number of causes
  } InflateCause;

  typedef enum {
    NOT_ENABLED    = 0,
    FATAL_EXIT     = 1,
    LOG_WARNING    = 2
  } SyncDiagnosticOption;

  // exit must be implemented non-blocking, since the compiler cannot easily handle
  // deoptimization at monitor exit. Hence, it does not take a Handle argument.

  // This is the "slow path" version of monitor enter and exit.
  static inline void enter(Handle obj, BasicLock* lock, JavaThread* current);
  static inline void exit(oop obj, BasicLock* lock, JavaThread* current);

  // Used to enter a monitor for another thread. This requires that the
  // locking_thread is suspended, and that entering on a potential
  // inflated monitor may only contend with deflation. That is the obj being
  // locked on is either already locked by the locking_thread or cannot
  // escape the locking_thread.
  static void enter_for(Handle obj, BasicLock* lock, JavaThread* locking_thread);
private:
  // Shared implementation for enter and enter_for. Performs all but
  // inflated monitor enter.
  static bool enter_fast_impl(Handle obj, BasicLock* lock, JavaThread* locking_thread);

  static bool quick_enter_legacy(oop obj, BasicLock* Lock, JavaThread* current);
  static void enter_legacy(Handle obj, BasicLock* Lock, JavaThread* current);
  static void exit_legacy(oop obj, BasicLock* lock, JavaThread* current);
public:
  // Used only to handle jni locks or other unmatched monitor enter/exit
  // Internally they will use heavy weight monitor.
  static void jni_enter(Handle obj, JavaThread* current);
  static void jni_exit(oop obj, TRAPS);

  // Handle all interpreter, compiler and jni cases
  static int  wait(Handle obj, jlong millis, TRAPS);
  static void notify(Handle obj, TRAPS);
  static void notifyall(Handle obj, TRAPS);

  static bool quick_notify(oopDesc* obj, JavaThread* current, bool All);
  static inline bool quick_enter(oop obj, BasicLock* Lock, JavaThread* current);

  // Special internal-use-only method for use by JVM infrastructure
  // that needs to wait() on a java-level object but that can't risk
  // throwing unexpected InterruptedExecutionExceptions.
  static void waitUninterruptibly(Handle obj, jlong Millis, TRAPS);

  // Inflate light weight monitor to heavy weight monitor
  static ObjectMonitor* inflate(Thread* current, oop obj, const InflateCause cause);
  // Used to inflate a monitor as if it was done from the thread JavaThread.
  static ObjectMonitor* inflate_for(JavaThread* thread, oop obj, const InflateCause cause);

private:
  // Shared implementation between the different LockingMode.
  static ObjectMonitor* inflate_impl(JavaThread* locking_thread, oop obj, const InflateCause cause);

public:
  // This version is only for internal use
  static void inflate_helper(oop obj);
  static const char* inflate_cause_name(const InflateCause cause);

  inline static ObjectMonitor* read_monitor(markWord mark);
  inline static ObjectMonitor* read_monitor(Thread* current, oop obj, markWord mark);

  // Returns the identity hash value for an oop
  // NOTE: It may cause monitor inflation
  static intptr_t FastHashCode(Thread* current, oop obj);

  // java.lang.Thread support
  static bool current_thread_holds_lock(JavaThread* current, Handle h_obj);

  static JavaThread* get_lock_owner(ThreadsList * t_list, Handle h_obj);

  // JNI detach support
  static void release_monitors_owned_by_thread(JavaThread* current);

  // Iterate over all ObjectMonitors.
  template <typename Function>
  static void monitors_iterate(Function function);

  // Iterate ObjectMonitors owned by any thread and where the owner `filter`
  // returns true.
  template <typename OwnerFilter>
  static void owned_monitors_iterate_filtered(MonitorClosure* closure, OwnerFilter filter);

  // Iterate ObjectMonitors where the owner is thread; this does NOT include
  // ObjectMonitors where the owner is anonymous.
  static void owned_monitors_iterate(MonitorClosure* m, JavaThread* thread);

  // Iterate ObjectMonitors where the owner is vthread.
  static void owned_monitors_iterate(MonitorClosure* m, oop vthread);

  // Iterate ObjectMonitors owned by any thread.
  static void owned_monitors_iterate(MonitorClosure* closure);

  // Initialize the gInflationLocks
  static void initialize();

  // We currently use aggressive monitor deflation policy;
  // basically we try to deflate all monitors that are not busy.
  static size_t deflate_idle_monitors();

  // Deflate idle monitors:
  static size_t deflate_monitor_list(ObjectMonitorDeflationSafepointer* safepointer);
  static size_t in_use_list_count();
  static size_t in_use_list_max();
  static size_t in_use_list_ceiling();
  static void dec_in_use_list_ceiling();
  static void inc_in_use_list_ceiling();
  static void set_in_use_list_ceiling(size_t new_value);
  static bool is_async_deflation_needed();
  static bool is_async_deflation_requested() { return _is_async_deflation_requested; }
  static bool is_final_audit() { return _is_final_audit; }
  static void set_is_final_audit() { _is_final_audit = true; }
  static jlong last_async_deflation_time_ns() { return _last_async_deflation_time_ns; }
  static void request_deflate_idle_monitors();
  static bool request_deflate_idle_monitors_from_wb();  // for whitebox test support
  static void set_is_async_deflation_requested(bool new_value) { _is_async_deflation_requested = new_value; }
  static jlong time_since_last_async_deflation_ms();

  // debugging
  static void audit_and_print_stats(outputStream* out, bool on_exit);
  static void chk_in_use_list(outputStream* out, int* error_cnt_p);
  static void chk_in_use_entry(ObjectMonitor* n, outputStream* out,
                               int* error_cnt_p);
  static void do_final_audit_and_print_stats();
  static void log_in_use_monitor_details(outputStream* out, bool log_all);

  static intptr_t get_next_hash(Thread* current, oop obj);

 private:
  friend class SynchronizerTest;
  friend class LightweightSynchronizer;

  static MonitorList _in_use_list;
  static volatile bool _is_async_deflation_requested;
  static volatile bool _is_final_audit;
  static jlong         _last_async_deflation_time_ns;

  // Support for SynchronizerTest access to GVars fields:
  static u_char* get_gvars_addr();
  static u_char* get_gvars_hc_sequence_addr();
  static size_t get_gvars_size();
  static u_char* get_gvars_stw_random_addr();

  static void handle_sync_on_value_based_class(Handle obj, JavaThread* locking_thread);
};

// ObjectLocker enforces balanced locking and can never throw an
// IllegalMonitorStateException. However, a pending exception may
// have to pass through, and we must also be able to deal with
// asynchronous exceptions. The caller is responsible for checking
// the thread's pending exception if needed.
// When using ObjectLocker the top native frames in the stack will
// not be seen in case we attempt preemption, since we start walking
// from the last Java anchor, so we disable it with NoPreemptMark.
class ObjectLocker : public StackObj {
 private:
  JavaThread* _thread;
  Handle      _obj;
  BasicLock   _lock;
  NoPreemptMark _npm;
 public:
  ObjectLocker(Handle obj, JavaThread* current);
  ~ObjectLocker();

  // Monitor behavior
  void wait(TRAPS)  { ObjectSynchronizer::wait(_obj, 0, CHECK); } // wait forever
  void wait_uninterruptibly(TRAPS)  { ObjectSynchronizer::waitUninterruptibly(_obj, 0, CHECK); } // wait forever
  void notify_all(TRAPS)  { ObjectSynchronizer::notifyall(_obj, CHECK); }
};

// Interface to visit monitors
class ObjectMonitorsView {
public:
  // Visit monitors that belong to the given thread
  virtual void visit(MonitorClosure* closure, JavaThread* thread) = 0;
};

#endif // SHARE_RUNTIME_SYNCHRONIZER_HPP
