/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_OOP_HPP
#define SHARE_OOPS_OOP_HPP

#include "memory/iterator.hpp"
#include "memory/memRegion.hpp"
#include "oops/compressedKlass.hpp"
#include "oops/accessDecorators.hpp"
#include "oops/markWord.hpp"
#include "oops/metadata.hpp"
#include "oops/objLayout.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include <type_traits>

// oopDesc is the top baseclass for objects classes. The {name}Desc classes describe
// the format of Java objects so the fields can be accessed from C++.
// oopDesc is abstract.
// (see oopHierarchy for complete oop class hierarchy)
//
// no virtual functions allowed

class oopDesc {
  friend class VMStructs;
  friend class JVMCIVMStructs;
 private:
  volatile markWord _mark;
  union _metadata {
    Klass*      _klass;
    narrowKlass _compressed_klass;
  } _metadata;

  // There may be ordering constraints on the initialization of fields that
  // make use of the C++ copy/assign incorrect.
  NONCOPYABLE(oopDesc);

  inline oop cas_set_forwardee(markWord new_mark, markWord old_mark, atomic_memory_order order);

 public:
  // Must be trivial; see verifying static assert after the class.
  oopDesc() = default;

  inline markWord  mark()          const;
  inline markWord  mark_acquire()  const;
  inline markWord* mark_addr() const;

  inline void set_mark(markWord m);
  static inline void set_mark(HeapWord* mem, markWord m);
  inline void set_mark_full(markWord m);
  static inline void release_set_mark(HeapWord* mem, markWord m);

  inline void release_set_mark(markWord m);
  inline markWord cas_set_mark(markWord new_mark, markWord old_mark);
  inline markWord cas_set_mark(markWord new_mark, markWord old_mark, atomic_memory_order order);

  // Returns the prototype mark that should be used for this object.
  inline markWord prototype_mark() const;

  // Used only to re-initialize the mark word (e.g., of promoted
  // objects during a GC) -- requires a valid klass pointer
  inline void init_mark();

  inline Klass* klass() const;
  inline Klass* klass_or_null() const;
  inline Klass* klass_or_null_acquire() const;
  // Get the klass without running any asserts.
  inline Klass* klass_without_asserts() const;

  void set_narrow_klass(narrowKlass nk) NOT_CDS_JAVA_HEAP_RETURN;
  inline void set_klass(Klass* k);
  static inline void release_set_klass(HeapWord* mem, Klass* k);

  // For klass field compression
  static inline void set_klass_gap(HeapWord* mem, int z);

  // Size of object header, aligned to platform wordSize
  static int header_size() {
    if (UseCompactObjectHeaders) {
      return sizeof(markWord) / HeapWordSize;
    } else {
      return sizeof(oopDesc)  / HeapWordSize;
    }
  }

  // Returns whether this is an instance of k or an instance of a subclass of k
  inline bool is_a(Klass* k) const;

  // Returns the actual oop size of the object in machine words
  inline size_t size();

  // Returns the size that a copy of this object requires, in machine words.
  // It can be 1 word larger than its current size to accomodate
  // an additional 4-byte-field for the identity hash-code.
  //
  // size: the current size of this object, we're passing this here for performance
  //       reasons, because all callers compute this anyway, and we want to avoid
  //       recomputing it.
  // mark: the mark-word of this object. Some callers (e.g. G1ParScanThreadState::do_copy_to_survivor_space())
  //       need to use a known markWord because of racing GC threads that can change
  //       the markWord at any time.
  inline size_t copy_size(size_t size, markWord mark) const;
  // Special version to deal with scratch classes in CDS. There we allocate
  // temporary scratch classes (which are skeleton versions of InstanceMirrorKlass,
  // which represent java.lang.Class objects in the CDS archive). At that point, we
  // don't know whether or not the final archived version will be hashed or expanded,
  // and therefore we allocate them in the special state not-hashed-but-expanded.
  // When creating the final copy of those objects, we either populate the hidden hash
  // field and make the object 'expanded', or we turn it back to 'not-hashed'
  // and reduce the object's size. We do this by providing a separate method for CDS
  // so that we don't affect GC performance.
  inline size_t copy_size_cds(size_t size, markWord mark) const;

  // Sometimes (for complicated concurrency-related reasons), it is useful
  // to be able to figure out the size of an object knowing its klass.
  inline size_t base_size_given_klass(markWord m, const Klass* klass);
  inline size_t size_given_mark_and_klass(markWord mrk, const Klass* kls);

  // type test operations (inlined in oop.inline.hpp)
  inline bool is_instance()    const;
  inline bool is_instanceRef() const;
  inline bool is_stackChunk()  const;
  inline bool is_array()       const;
  inline bool is_objArray()    const;
  inline bool is_typeArray()   const;

  // type test operations that don't require inclusion of oop.inline.hpp.
  bool is_instance_noinline()    const;
  bool is_instanceRef_noinline() const;
  bool is_stackChunk_noinline()  const;
  bool is_array_noinline()       const;
  bool is_objArray_noinline()    const;
  bool is_typeArray_noinline()   const;

 protected:
  inline oop        as_oop() const { return const_cast<oopDesc*>(this); }

 public:
  template<typename T>
  inline T* field_addr(int offset) const;

  template <typename T> inline size_t field_offset(T* p) const;

  // Standard compare function returns negative value if o1 < o2
  //                                   0              if o1 == o2
  //                                   positive value if o1 > o2
  inline static int  compare(oop o1, oop o2) {
    void* o1_addr = (void*)o1;
    void* o2_addr = (void*)o2;
    if (o1_addr < o2_addr) {
      return -1;
    } else if (o1_addr > o2_addr) {
      return 1;
    } else {
      return 0;
    }
  }

  // Access to fields in a instanceOop through these methods.
  template<DecoratorSet decorators>
  oop obj_field_access(int offset) const;
  oop obj_field(int offset) const;

  void obj_field_put(int offset, oop value);
  void obj_field_put_raw(int offset, oop value);
  void obj_field_put_volatile(int offset, oop value);
  template<DecoratorSet decorators>
  void obj_field_put_access(int offset, oop value);

  Metadata* metadata_field(int offset) const;
  void metadata_field_put(int offset, Metadata* value);

  Metadata* metadata_field_acquire(int offset) const;
  void release_metadata_field_put(int offset, Metadata* value);

  jbyte byte_field(int offset) const;
  void byte_field_put(int offset, jbyte contents);

  jchar char_field(int offset) const;
  void char_field_put(int offset, jchar contents);

  jboolean bool_field(int offset) const;
  void bool_field_put(int offset, jboolean contents);
  jboolean bool_field_volatile(int offset) const;
  void bool_field_put_volatile(int offset, jboolean contents);

  jint int_field(int offset) const;
  void int_field_put(int offset, jint contents);

  jshort short_field(int offset) const;
  void short_field_put(int offset, jshort contents);

  jlong long_field(int offset) const;
  void long_field_put(int offset, jlong contents);

  jfloat float_field(int offset) const;
  void float_field_put(int offset, jfloat contents);

  jdouble double_field(int offset) const;
  void double_field_put(int offset, jdouble contents);

  address address_field(int offset) const;
  void address_field_put(int offset, address contents);

  oop obj_field_acquire(int offset) const;
  void release_obj_field_put(int offset, oop value);

  jbyte byte_field_acquire(int offset) const;
  void release_byte_field_put(int offset, jbyte contents);

  jchar char_field_acquire(int offset) const;
  void release_char_field_put(int offset, jchar contents);

  jboolean bool_field_acquire(int offset) const;
  void release_bool_field_put(int offset, jboolean contents);

  jint int_field_relaxed(int offset) const;
  void int_field_put_relaxed(int offset, jint contents);
  jint int_field_acquire(int offset) const;
  void release_int_field_put(int offset, jint contents);

  jshort short_field_acquire(int offset) const;
  void release_short_field_put(int offset, jshort contents);

  jlong long_field_acquire(int offset) const;
  void release_long_field_put(int offset, jlong contents);

  jfloat float_field_acquire(int offset) const;
  void release_float_field_put(int offset, jfloat contents);

  jdouble double_field_acquire(int offset) const;
  void release_double_field_put(int offset, jdouble contents);

  address address_field_acquire(int offset) const;
  void release_address_field_put(int offset, address contents);

  // printing functions for VM debugging
  void print_on(outputStream* st) const;         // First level print
  void print_value_on(outputStream* st) const;   // Second level print.
  void print_address_on(outputStream* st) const; // Address printing
  void print_name_on(outputStream* st) const;    // External name printing.

  // printing on default output stream
  void print();
  void print_value();
  void print_address();

  // return the print strings
  char* print_string();
  char* print_value_string();

  // verification operations
  static void verify_on(outputStream* st, oopDesc* oop_desc);
  static void verify(oopDesc* oopDesc);

  // locking operations
  inline bool is_locked()   const;
  inline bool is_unlocked() const;

  // asserts and guarantees
  static bool is_oop(oop obj, bool ignore_mark_word = false);
  static bool is_oop_or_null(oop obj, bool ignore_mark_word = false);

  // garbage collection
  inline bool is_gc_marked() const;

  // Forward pointer operations for scavenge
  inline bool is_forwarded() const;
  inline bool is_self_forwarded() const;

  inline void forward_to(oop p);
  inline void forward_to_self();

  // Like "forward_to", but inserts the forwarding pointer atomically.
  // Exactly one thread succeeds in inserting the forwarding pointer, and
  // this call returns null for that thread; any other thread has the
  // value of the forwarding pointer returned and does not modify "this".
  inline oop forward_to_atomic(oop p, markWord compare, atomic_memory_order order = memory_order_conservative);
  inline oop forward_to_self_atomic(markWord compare, atomic_memory_order order = memory_order_conservative);

  inline oop forwardee() const;
  inline oop forwardee(markWord header) const;

  inline void unset_self_forwarded();
  inline void reset_forwarded();

  // Age of object during scavenge
  inline uint age() const;
  inline void incr_age();

  template <typename OopClosureType>
  inline void oop_iterate(OopClosureType* cl);

  template <typename OopClosureType>
  inline void oop_iterate(OopClosureType* cl, MemRegion mr);

  template <typename OopClosureType>
  inline size_t oop_iterate_size(OopClosureType* cl);

  template <typename OopClosureType>
  inline size_t oop_iterate_size(OopClosureType* cl, MemRegion mr);

  template <typename OopClosureType>
  inline void oop_iterate_backwards(OopClosureType* cl);

  template <typename OopClosureType>
  inline void oop_iterate_backwards(OopClosureType* cl, Klass* klass);

  inline static bool is_instanceof_or_null(oop obj, Klass* klass);

  // identity hash; returns the identity hash key (computes it if necessary)
  inline intptr_t identity_hash();
  intptr_t slow_identity_hash();
  inline bool fast_no_hash_check();

  // Initialize identity hash code in hash word of object copy from original object.
  // Returns true if the object has been expanded, false otherwise.
  inline void initialize_hash_if_necessary(oop obj);
  // For CDS only.
  void initialize_hash_if_necessary(oop obj, Klass* k, markWord m);

  // marks are forwarded to stack when object is locked
  inline bool     has_displaced_mark() const;
  inline markWord displaced_mark() const;
  inline void     set_displaced_mark(markWord m);

  // Checks if the mark word needs to be preserved
  inline bool mark_must_be_preserved() const;
  inline bool mark_must_be_preserved(markWord m) const;

  inline static bool has_klass_gap() {
    return ObjLayout::oop_has_klass_gap();
  }

  // for code generation
  static int mark_offset_in_bytes()      { return (int)offset_of(oopDesc, _mark); }
  static int klass_offset_in_bytes()     {
    assert(!UseCompactObjectHeaders, "don't use this with compact headers");
    return (int)offset_of(oopDesc, _metadata._klass);
  }
  static int klass_gap_offset_in_bytes() {
    assert(has_klass_gap(), "only applicable to compressed klass pointers");
    if (UseCompactObjectHeaders) {
      return base_offset_in_bytes();
    } else {
      return klass_offset_in_bytes() + sizeof(narrowKlass);
    }
  }

  static int base_offset_in_bytes() {
    return ObjLayout::oop_base_offset_in_bytes();
  }

  // for error reporting
  static void* load_oop_raw(oop obj, int offset);
};

// An oopDesc is not initialized via a constructor.  Space is allocated in
// the Java heap, and static functions provided here on HeapWord* are used
// to fill in certain parts of that memory.  The allocated memory is then
// treated as referring to an oopDesc.  For that to be valid, the oopDesc
// class must have a trivial default constructor (C++14 3.8/1).
static_assert(std::is_trivially_default_constructible<oopDesc>::value, "required");

#endif // SHARE_OOPS_OOP_HPP
