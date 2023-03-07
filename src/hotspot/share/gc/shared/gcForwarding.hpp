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

#ifndef SHARE_GC_SHARED_GCFORWARDING_HPP
#define SHARE_GC_SHARED_GCFORWARDING_HPP

#include "oops/oopsHierarchy.hpp"
#include "utilities/debug.hpp"

class GCForwardingImpl {
public:
  // Begins GC forwarding. This is called before a GC phase that
  // uses GC forwarding. It is typically used to set-up any data-structures
  // that are used by GC forwarding.
  virtual void begin() = 0;

  // Ends GC forwarding. This is called after a GC phase that
  // uses GC forwarding. It typically cleans up data-structures that
  // are used by GC forwarding.
  virtual void end() = 0;

  // Return true if the object is forwarded, false otherwise.
  virtual bool is_forwarded(oop obj) = 0;

  // Returns the forwardee of an object, which may be any object,
  // or null if not forwarded, or the object itself.
  virtual oop forwardee(oop obj) = 0;

  // Forwards an object to its forwardee.
  // The forwardee must not be the object itself. Use forward_to_self() for this.
  virtual void forward_to(oop obj, oop fwd) = 0;
  // NOTE: The forward_to_self(_atomic) methods exist for performance reasons, so
  // that the common forwarding path doesn't have to check for and special-handle self.
  virtual void forward_to_self(oop obj) = 0;

  // Atomically forwards an object to a forwardee (which may be the object itself).
  // Returns the forwardee, if another thread installed a forwardee before this thread.
  // The forwardee must not be the object itself. Use forward_to_self() for this.
  virtual oop forward_to_atomic(oop obj, oop fwd) = 0;
  virtual oop forward_to_self_atomic(oop obj) = 0;
};

class GCForwarding {
private:
  static GCForwardingImpl* _forwarding;

public:
  static void set_forwarding(GCForwardingImpl& forwarding) {
    _forwarding = &forwarding;
  }

  static void begin() {
    assert(_forwarding != nullptr, "need forwarding impl");
    _forwarding->begin();
  }

  static void end() {
    assert(_forwarding != nullptr, "need forwarding impl");
    _forwarding->end();
  }

  static inline void forward_to(oop obj, oop fwd) {
    assert(_forwarding != nullptr, "need forwarding impl");
    _forwarding->forward_to(obj, fwd);
  }

  static inline oop forwardee(oop obj) {
    assert(_forwarding != nullptr, "need forwarding impl");
    return _forwarding->forwardee(obj);
  }
};

#endif // SHARE_LOGGING_LOGASYNCWRITER_HPP
