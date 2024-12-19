/*
 * Copyright (c) 2011, 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "cds/cdsConfig.hpp"
#include "cds/serializeClosure.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/oopFactory.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "oops/instanceOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/symbol.hpp"
#include "runtime/handles.inline.hpp"
#include "utilities/macros.hpp"

int InstanceMirrorKlass::_offset_of_static_fields = 0;

InstanceMirrorKlass::InstanceMirrorKlass() {
  assert(CDSConfig::is_dumping_static_archive() || CDSConfig::is_using_archive(), "only for CDS");
}

size_t InstanceMirrorKlass::instance_size(Klass* k) const {
  if (k != nullptr && k->is_instance_klass()) {
    return align_object_size(size_helper() + InstanceKlass::cast(k)->static_field_size());
  }
  return size_helper();
}

instanceOop InstanceMirrorKlass::allocate_instance(Klass* k, bool extend, TRAPS) {
  // Query before forming handle.
  size_t base_size = instance_size(k);
  size_t size = base_size;
  if (extend && UseCompactObjectHeaders) {
    size_t base_size_bytes = base_size * BytesPerWord;
    assert(checked_cast<int>(base_size_bytes) >= hash_offset(k), "hash_offset must be <= base size");
    if (base_size_bytes - hash_offset(k) < BytesPerInt) {
      size = align_object_size(size + 1);
    }
  }
  assert(base_size > 0, "base object size must be non-zero: " SIZE_FORMAT, size);

  // Since mirrors can be variable sized because of the static fields, store
  // the size in the mirror itself.
  instanceOop obj = (instanceOop)Universe::heap()->class_allocate(this, size, base_size, THREAD);
  if (extend && UseCompactObjectHeaders) {
    obj->set_mark(obj->mark().set_not_hashed_expanded());
  }
  return obj;
}

size_t InstanceMirrorKlass::oop_size(oop obj) const {
  return java_lang_Class::oop_size(obj);
}

int InstanceMirrorKlass::compute_static_oop_field_count(oop obj) {
  Klass* k = java_lang_Class::as_Klass(obj);
  if (k != nullptr && k->is_instance_klass()) {
    return InstanceKlass::cast(k)->static_oop_field_count();
  }
  return 0;
}

int InstanceMirrorKlass::hash_offset(Klass* klass) const {
  if (_offset_of_static_fields - _hash_offset > BytesPerInt) {
    // There is a usable gap between the fields of this Class and the static fields block. Use it.
    //tty->print_cr("offset static fields: %d, hash offset: %d", _offset_of_static_fields, _hash_offset);
    return _hash_offset;
  } else if (klass != nullptr && klass->is_instance_klass()) {
    // Use the static field offset of the corresponding Klass*.
    return InstanceKlass::cast(klass)->static_hash_offset_in_bytes();
  } else {
    // TODO: we could be more clever here and try to use gaps that are
    // left after the static fields. Unfortunately, the static_field_size
    // is only in words, this would require careful rewrite to
    // be in bytes.
    //tty->print_cr("Use base size");
    return instance_size(klass);
  }
}

int InstanceMirrorKlass::hash_offset_in_bytes(oop obj) const {
  assert(UseCompactObjectHeaders, "only with compact i-hash");
  assert(obj != nullptr, "expect object");
  assert(instance_size(java_lang_Class::as_Klass(obj)) == obj->base_size_given_klass(this), "must match");
  return hash_offset(java_lang_Class::as_Klass(obj));
}

#if INCLUDE_CDS
void InstanceMirrorKlass::serialize_offsets(SerializeClosure* f) {
  f->do_int(&_offset_of_static_fields);
}
#endif
