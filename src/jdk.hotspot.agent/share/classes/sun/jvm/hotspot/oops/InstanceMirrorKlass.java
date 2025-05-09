/*
 * Copyright (c) 2011, 2020, Oracle and/or its affiliates. All rights reserved.
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

package sun.jvm.hotspot.oops;

import java.io.*;
import java.util.*;
import sun.jvm.hotspot.debugger.*;
import sun.jvm.hotspot.memory.*;
import sun.jvm.hotspot.runtime.*;
import sun.jvm.hotspot.types.*;
import sun.jvm.hotspot.utilities.*;
import sun.jvm.hotspot.utilities.Observable;
import sun.jvm.hotspot.utilities.Observer;

// An InstanceKlass is the VM level representation of a Java class.

public class InstanceMirrorKlass extends InstanceKlass {
  static {
    VM.registerVMInitializedObserver(new Observer() {
        public void update(Observable o, Object data) {
          initialize(VM.getVM().getTypeDataBase());
        }
      });
  }

  private static synchronized void initialize(TypeDataBase db) throws WrongTypeException {
    // Just make sure it's there for now
    Type type = db.lookupType("InstanceMirrorKlass");
  }

  public InstanceMirrorKlass(Address addr) {
    super(addr);
  }

  public long getObjectSize(Oop o) {
    long s = java_lang_Class.getOopSize(o) * VM.getVM().getAddressSize();
    if (VM.getVM().isCompactObjectHeadersEnabled()) {
      Mark mark = o.getMark();
      if (mark.isExpanded()) {
        // Needs extra 4 bytes for identity hash-code (and align-up to whole word).
        s += VM.getVM().getAddressSize();
      }
    }
    return s;
  }

  public void iterateNonStaticFields(OopVisitor visitor, Oop obj) {
    super.iterateNonStaticFields(visitor, obj);
    // Fetch the real klass from the mirror object
    Klass klass = java_lang_Class.asKlass(obj);
    if (klass instanceof InstanceKlass) {
      ((InstanceKlass)klass).iterateStaticFields(visitor);
    }
  }
}
