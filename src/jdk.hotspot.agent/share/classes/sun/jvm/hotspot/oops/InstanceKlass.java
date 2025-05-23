/*
 * Copyright (c) 2000, 2025, Oracle and/or its affiliates. All rights reserved.
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
import sun.jvm.hotspot.classfile.ClassLoaderData;
import sun.jvm.hotspot.code.CompressedReadStream;
import sun.jvm.hotspot.debugger.*;
import sun.jvm.hotspot.memory.*;
import sun.jvm.hotspot.runtime.*;
import sun.jvm.hotspot.types.*;
import sun.jvm.hotspot.utilities.*;
import sun.jvm.hotspot.utilities.Observable;
import sun.jvm.hotspot.utilities.Observer;

// An InstanceKlass is the VM level representation of a Java class.

public class InstanceKlass extends Klass {
  static {
    VM.registerVMInitializedObserver(new Observer() {
        public void update(Observable o, Object data) {
          initialize(VM.getVM().getTypeDataBase());
        }
      });
  }

  // internal field flags constants
  static int FIELD_FLAG_IS_INITIALIZED;
  static int FIELD_FLAG_IS_INJECTED;
  static int FIELD_FLAG_IS_GENERIC;
  static int FIELD_FLAG_IS_STABLE;
  static int FIELD_FLAG_IS_CONTENDED;

  // ClassState constants
  private static int CLASS_STATE_ALLOCATED;
  private static int CLASS_STATE_LOADED;
  private static int CLASS_STATE_LINKED;
  private static int CLASS_STATE_BEING_INITIALIZED;
  private static int CLASS_STATE_FULLY_INITIALIZED;
  private static int CLASS_STATE_INITIALIZATION_ERROR;


  private static synchronized void initialize(TypeDataBase db) throws WrongTypeException {
    Type type            = db.lookupType("InstanceKlass");
    annotations          = type.getAddressField("_annotations");
    arrayKlasses         = new MetadataField(type.getAddressField("_array_klasses"), 0);
    methods              = type.getAddressField("_methods");
    defaultMethods       = type.getAddressField("_default_methods");
    methodOrdering       = type.getAddressField("_method_ordering");
    localInterfaces      = type.getAddressField("_local_interfaces");
    transitiveInterfaces = type.getAddressField("_transitive_interfaces");
    fieldinfoStream      = type.getAddressField("_fieldinfo_stream");
    constants            = new MetadataField(type.getAddressField("_constants"), 0);
    sourceDebugExtension = type.getAddressField("_source_debug_extension");
    innerClasses         = type.getAddressField("_inner_classes");
    nestMembers          = type.getAddressField("_nest_members");
    nonstaticFieldSize   = new CIntField(type.getCIntegerField("_nonstatic_field_size"), 0);
    staticFieldSize      = new CIntField(type.getCIntegerField("_static_field_size"), 0);
    staticOopFieldCount  = new CIntField(type.getCIntegerField("_static_oop_field_count"), 0);
    nonstaticOopMapSize  = new CIntField(type.getCIntegerField("_nonstatic_oop_map_size"), 0);
    initState            = new CIntField(type.getCIntegerField("_init_state"), 0);
    itableLen            = new CIntField(type.getCIntegerField("_itable_len"), 0);
    nestHostIndex        = new CIntField(type.getCIntegerField("_nest_host_index"), 0);
    hashOffset           = new CIntField(type.getCIntegerField("_hash_offset"), 0);
    if (VM.getVM().isJvmtiSupported()) {
      breakpoints        = type.getAddressField("_breakpoints");
    }
    headerSize           = type.getSize();

    // read internal field flags constants
    FIELD_FLAG_IS_INITIALIZED      = db.lookupIntConstant("FieldInfo::FieldFlags::_ff_initialized");
    FIELD_FLAG_IS_INJECTED         = db.lookupIntConstant("FieldInfo::FieldFlags::_ff_injected");
    FIELD_FLAG_IS_GENERIC          = db.lookupIntConstant("FieldInfo::FieldFlags::_ff_generic");
    FIELD_FLAG_IS_STABLE           = db.lookupIntConstant("FieldInfo::FieldFlags::_ff_stable");
    FIELD_FLAG_IS_CONTENDED        = db.lookupIntConstant("FieldInfo::FieldFlags::_ff_contended");


    // read ClassState constants
    CLASS_STATE_ALLOCATED = db.lookupIntConstant("InstanceKlass::allocated").intValue();
    CLASS_STATE_LOADED = db.lookupIntConstant("InstanceKlass::loaded").intValue();
    CLASS_STATE_LINKED = db.lookupIntConstant("InstanceKlass::linked").intValue();
    CLASS_STATE_BEING_INITIALIZED = db.lookupIntConstant("InstanceKlass::being_initialized").intValue();
    CLASS_STATE_FULLY_INITIALIZED = db.lookupIntConstant("InstanceKlass::fully_initialized").intValue();
    CLASS_STATE_INITIALIZATION_ERROR = db.lookupIntConstant("InstanceKlass::initialization_error").intValue();
    // We need a new fieldsCache each time we attach.
    fieldsCache = new WeakHashMap<Address, Field[]>();
  }

  public InstanceKlass(Address addr) {
    super(addr);

    // If the class hasn't yet reached the "loaded" init state, then don't go any further
    // or we'll run into problems trying to look at fields that are not yet setup.
    // Attempted lookups of this InstanceKlass via ClassLoaderDataGraph, ClassLoaderData,
    // and Dictionary will all refuse to return it. The main purpose of allowing this
    // InstanceKlass to initialize is so ClassLoaderData.getKlasses() will succeed, allowing
    // ClassLoaderData.classesDo() to iterate over all Klasses (skipping those that are
    // not yet fully loaded).
    if (!isLoaded()) {
        return;
    }

    if (getJavaFieldsCount() != getAllFieldsCount()) {
      // Exercise the injected field logic
      for (int i = getJavaFieldsCount(); i < getAllFieldsCount(); i++) {
        getFieldName(i);
        getFieldSignature(i);
      }
    }
  }

  private static AddressField  annotations;
  private static MetadataField arrayKlasses;
  private static AddressField  methods;
  private static AddressField  defaultMethods;
  private static AddressField  methodOrdering;
  private static AddressField  localInterfaces;
  private static AddressField  transitiveInterfaces;
  private static AddressField  fieldinfoStream;
  private static MetadataField constants;
  private static AddressField  sourceDebugExtension;
  private static AddressField  innerClasses;
  private static AddressField  nestMembers;
  private static CIntField nonstaticFieldSize;
  private static CIntField staticFieldSize;
  private static CIntField staticOopFieldCount;
  private static CIntField nonstaticOopMapSize;
  private static CIntField initState;
  private static CIntField itableLen;
  private static CIntField nestHostIndex;
  private static CIntField hashOffset;
  private static AddressField breakpoints;

  // type safe enum for ClassState from instanceKlass.hpp
  public static class ClassState {
     public static final ClassState ALLOCATED    = new ClassState("allocated");
     public static final ClassState LOADED       = new ClassState("loaded");
     public static final ClassState LINKED       = new ClassState("linked");
     public static final ClassState BEING_INITIALIZED      = new ClassState("beingInitialized");
     public static final ClassState FULLY_INITIALIZED    = new ClassState("fullyInitialized");
     public static final ClassState INITIALIZATION_ERROR = new ClassState("initializationError");

     private ClassState(String value) {
        this.value = value;
     }

     public String toString() {
        return value;
     }

     private String value;
  }

  public int  getInitStateAsInt() { return (int) initState.getValue(this); }
  public ClassState getInitState() {
     int state = getInitStateAsInt();
     if (state == CLASS_STATE_ALLOCATED) {
        return ClassState.ALLOCATED;
     } else if (state == CLASS_STATE_LOADED) {
        return ClassState.LOADED;
     } else if (state == CLASS_STATE_LINKED) {
        return ClassState.LINKED;
     } else if (state == CLASS_STATE_BEING_INITIALIZED) {
        return ClassState.BEING_INITIALIZED;
     } else if (state == CLASS_STATE_FULLY_INITIALIZED) {
        return ClassState.FULLY_INITIALIZED;
     } else if (state == CLASS_STATE_INITIALIZATION_ERROR) {
        return ClassState.INITIALIZATION_ERROR;
     } else {
        throw new RuntimeException("should not reach here");
     }
  }

  // initialization state quaries
  public boolean isLoaded() {
     return getInitStateAsInt() >= CLASS_STATE_LOADED;
  }

  public boolean isLinked() {
     return getInitStateAsInt() >= CLASS_STATE_LINKED;
  }

  public boolean isInitialized() {
     return getInitStateAsInt() == CLASS_STATE_FULLY_INITIALIZED;
  }

  public boolean isNotInitialized() {
     return getInitStateAsInt() < CLASS_STATE_BEING_INITIALIZED;
  }

  public boolean isBeingInitialized() {
     return getInitStateAsInt() == CLASS_STATE_BEING_INITIALIZED;
  }

  public boolean isInErrorState() {
     return getInitStateAsInt() == CLASS_STATE_INITIALIZATION_ERROR;
  }

  public int getClassStatus() {
     int result = 0;
     if (isLinked()) {
        result |= JVMDIClassStatus.VERIFIED | JVMDIClassStatus.PREPARED;
     }

     if (isInitialized()) {
        if (Assert.ASSERTS_ENABLED) {
           Assert.that(isLinked(), "Class status is not consistent");
        }
        result |= JVMDIClassStatus.INITIALIZED;
     }

     if (isInErrorState()) {
        result |= JVMDIClassStatus.ERROR;
     }
     return result;
  }

  // Byteside of the header
  private static long headerSize;

  public long getObjectSize(Oop object) {
    long baseSize = getSizeHelper() * VM.getVM().getAddressSize();
    if (VM.getVM().isCompactObjectHeadersEnabled()) {
      Mark mark = object.getMark();
      if (mark.isExpanded() && (getHashOffset() + 4 /* size of hash field */) > baseSize) {
        // Needs extra word for identity hash-code.
        return baseSize + VM.getVM().getBytesPerWord();
      }
    }
    return baseSize;
  }

  public long getSize() { // in number of bytes
    long wordLength = VM.getVM().getBytesPerWord();
    long size = getHeaderSize() +
                (getVtableLen() +
                 getItableLen() +
                 getNonstaticOopMapSize()) * wordLength;
    if (isInterface()) {
      size += wordLength;
    }
    return alignSize(size);
  }

  public static long getHeaderSize() { return headerSize; }

  // Each InstanceKlass mirror instance will cache the Field[] array after it is decoded,
  // but since there can be multiple InstanceKlass mirror instances per hotspot InstanceKlass,
  // we also have a global cache that uses the Address of the hotspot InstanceKlass as the key.
  private Field[] fields;
  private static Map<Address, Field[]> fieldsCache;

  Field getField(int index) {
    synchronized (this) {
      fields = fieldsCache.get(this.getAddress());
      if (fields == null) {
        fields = Field.getFields(this);
        fieldsCache.put(this.getAddress(), fields);
      } else {
      }
    }
    return fields[index];
  }

  public short getFieldAccessFlags(int index) {
    return (short)getField(index).getAccessFlags();
  }

  public int getFieldNameIndex(int index) {
    if (index >= getJavaFieldsCount()) throw new IndexOutOfBoundsException("not a Java field;");
    return getField(index).getNameIndex();
  }

  public Symbol getFieldName(int index) {
    // Cannot use getFieldNameIndex() because this method is also used for injected fields
    return getField(index).getName();
  }

  public Symbol getSymbolFromIndex(int cpIndex, boolean injected) {
    if (injected) {
      return vmSymbols.symbolAt(cpIndex);
    } else {
      return getConstants().getSymbolAt(cpIndex);
    }
  }

  public int getFieldSignatureIndex(int index) {
    if (index >= getJavaFieldsCount()) throw new IndexOutOfBoundsException("not a Java field;");
    return getField(index).getSignatureIndex();
  }

  public Symbol getFieldSignature(int index) {
    // Cannot use getFieldSignatureIndex() because this method is also use for injected fields
    return getField(index).getSignature();
  }

  public int getFieldGenericSignatureIndex(int index) {
    return getField(index).getGenericSignatureIndex();
  }

  public Symbol getFieldGenericSignature(int index) {
    return getField(index).getGenericSignature();
  }

  public int getFieldInitialValueIndex(int index) {
    if (index >= getJavaFieldsCount()) throw new IndexOutOfBoundsException("not a Java field;");
    return getField(index).getInitialValueIndex();
  }

  public int getFieldOffset(int index) {
    return (int)getField(index).getOffset();
  }

  // Accessors for declared fields
  public Klass     getArrayKlasses()        { return (Klass)        arrayKlasses.getValue(this); }
  public MethodArray  getMethods()              { return new MethodArray(methods.getValue(getAddress())); }

  public MethodArray  getDefaultMethods() {
    if (defaultMethods != null) {
      Address addr = defaultMethods.getValue(getAddress());
      if ((addr != null) && (addr.getAddressAt(0) != null)) {
        return new MethodArray(addr);
      } else {
        return null;
      }
    } else {
      return null;
    }
  }

  private int javaFieldsCount = -1;
  private int allFieldsCount = -1;

  private void initFieldCounts() {
    CompressedReadStream crs = new CompressedReadStream(getFieldInfoStream().getDataStart());
    javaFieldsCount = crs.readInt(); // read num_java_fields
    allFieldsCount = javaFieldsCount + crs.readInt(); // read num_injected_fields;
  }

  public int getJavaFieldsCount() {
    if (javaFieldsCount == -1) {
      initFieldCounts();
    }
    return javaFieldsCount;
  }

  public int getAllFieldsCount() {
    if (allFieldsCount == -1) {
      initFieldCounts();
    }
    return allFieldsCount;
  }

  public KlassArray   getLocalInterfaces()      { return new KlassArray(localInterfaces.getValue(getAddress())); }
  public KlassArray   getTransitiveInterfaces() { return new KlassArray(transitiveInterfaces.getValue(getAddress())); }
  public ConstantPool getConstants()        { return (ConstantPool) constants.getValue(this); }
  public Symbol    getSourceFileName()      { return                getConstants().getSourceFileName(); }
  public String    getSourceDebugExtension(){ return                CStringUtilities.getString(sourceDebugExtension.getValue(getAddress())); }
  public long      getNonstaticFieldSize()  { return                nonstaticFieldSize.getValue(this); }
  public long      getStaticOopFieldCount() { return                staticOopFieldCount.getValue(this); }
  public long      getNonstaticOopMapSize() { return                nonstaticOopMapSize.getValue(this); }
  public long      getItableLen()           { return                itableLen.getValue(this); }
  public short     getNestHostIndex()       { return                (short) nestHostIndex.getValue(this); }
  public long      getHashOffset()          { return                hashOffset.getValue(this); }
  public long      majorVersion()           { return                getConstants().majorVersion(); }
  public long      minorVersion()           { return                getConstants().minorVersion(); }
  public Symbol    getGenericSignature()    { return                getConstants().getGenericSignature(); }
  // "size helper" == instance size in words
  public long getSizeHelper() {
    int lh = getLayoutHelper();
    if (Assert.ASSERTS_ENABLED) {
      Assert.that(lh > 0, "layout helper initialized for instance class");
    }
    return lh / VM.getVM().getAddressSize();
  }
  public Annotations  getAnnotations() {
    Address addr = annotations.getValue(getAddress());
    return VMObjectFactory.newObject(Annotations.class, addr);
  }

  // same as enum InnerClassAttributeOffset in VM code.
  private static class InnerClassAttributeOffset {
    // from JVM spec. "InnerClasses" attribute
    public static int innerClassInnerClassInfoOffset;
    public static int innerClassOuterClassInfoOffset;
    public static int innerClassInnerNameOffset;
    public static int innerClassAccessFlagsOffset;
    public static int innerClassNextOffset;
    static {
      VM.registerVMInitializedObserver(new Observer() {
          public void update(Observable o, Object data) {
              initialize(VM.getVM().getTypeDataBase());
          }
      });
    }

    private static synchronized void initialize(TypeDataBase db) {
      innerClassInnerClassInfoOffset = db.lookupIntConstant(
          "InstanceKlass::inner_class_inner_class_info_offset").intValue();
      innerClassOuterClassInfoOffset = db.lookupIntConstant(
          "InstanceKlass::inner_class_outer_class_info_offset").intValue();
      innerClassInnerNameOffset = db.lookupIntConstant(
          "InstanceKlass::inner_class_inner_name_offset").intValue();
      innerClassAccessFlagsOffset = db.lookupIntConstant(
          "InstanceKlass::inner_class_access_flags_offset").intValue();
      innerClassNextOffset = db.lookupIntConstant(
          "InstanceKlass::inner_class_next_offset").intValue();
    }
  }

  private static class EnclosingMethodAttributeOffset {
    public static int enclosingMethodAttributeSize;
    static {
      VM.registerVMInitializedObserver(new Observer() {
          public void update(Observable o, Object data) {
              initialize(VM.getVM().getTypeDataBase());
          }
      });
    }
    private static synchronized void initialize(TypeDataBase db) {
      enclosingMethodAttributeSize = db.lookupIntConstant("InstanceKlass::enclosing_method_attribute_size").intValue();
    }
  }

  // whether given Symbol is name of an inner/nested Klass of this Klass?
  // anonymous and local classes are excluded.
  public boolean isInnerClassName(Symbol sym) {
    return isInInnerClasses(sym, false);
  }

  // whether given Symbol is name of an inner/nested Klass of this Klass?
  // anonymous classes excluded, but local classes are included.
  public boolean isInnerOrLocalClassName(Symbol sym) {
    return isInInnerClasses(sym, true);
  }

  private boolean isInInnerClasses(Symbol sym, boolean includeLocals) {
    U2Array innerClassList = getInnerClasses();
    int length = ( innerClassList == null)? 0 : innerClassList.length();
    if (length > 0) {
       if (Assert.ASSERTS_ENABLED) {
         Assert.that(length % InnerClassAttributeOffset.innerClassNextOffset == 0 ||
                     length % InnerClassAttributeOffset.innerClassNextOffset == EnclosingMethodAttributeOffset.enclosingMethodAttributeSize,
                     "just checking");
       }
       for (int i = 0; i < length; i += InnerClassAttributeOffset.innerClassNextOffset) {
         if (i == length - EnclosingMethodAttributeOffset.enclosingMethodAttributeSize) {
             break;
         }
         int ioff = innerClassList.at(i +
                        InnerClassAttributeOffset.innerClassInnerClassInfoOffset);
         // 'ioff' can be zero.
         // refer to JVM spec. section 4.7.5.
         if (ioff != 0) {
            Symbol innerName = getConstants().getKlassNameAt(ioff);
            Symbol myname = getName();
            int ooff = innerClassList.at(i +
                        InnerClassAttributeOffset.innerClassOuterClassInfoOffset);
            // for anonymous classes inner_name_index of InnerClasses
            // attribute is zero.
            int innerNameIndex = innerClassList.at(i +
                        InnerClassAttributeOffset.innerClassInnerNameOffset);
            // if this is not a member (anonymous, local etc.), 'ooff' will be zero
            // refer to JVM spec. section 4.7.5.
            if (ooff == 0) {
               if (includeLocals) {
                  // does it looks like my local class?
                  if (innerName.equals(sym) &&
                     innerName.asString().startsWith(myname.asString())) {
                     // exclude anonymous classes.
                     return (innerNameIndex != 0);
                  }
               }
            } else {
               Symbol outerName = getConstants().getKlassNameAt(ooff);

               // include only if current class is outer class.
               if (outerName.equals(myname) && innerName.equals(sym)) {
                  return true;
               }
           }
         }
       } // for inner classes
       return false;
    } else {
       return false;
    }
  }

  public boolean implementsInterface(Klass k) {
    if (Assert.ASSERTS_ENABLED) {
      Assert.that(k.isInterface(), "should not reach here");
    }
    KlassArray interfaces =  getTransitiveInterfaces();
    final int len = interfaces.length();
    for (int i = 0; i < len; i++) {
      if (interfaces.getAt(i).equals(k)) return true;
    }
    return false;
  }

  boolean computeSubtypeOf(Klass k) {
    if (k.isInterface()) {
      return implementsInterface(k);
    } else {
      return super.computeSubtypeOf(k);
    }
  }

  public void printValueOn(PrintStream tty) {
    tty.print("InstanceKlass for " + getName().asString());
  }

  public void iterateFields(MetadataVisitor visitor) {
    super.iterateFields(visitor);
    visitor.doMetadata(arrayKlasses, true);
    // visitor.doOop(methods, true);
    // visitor.doOop(localInterfaces, true);
    // visitor.doOop(transitiveInterfaces, true);
      visitor.doCInt(nonstaticFieldSize, true);
      visitor.doCInt(staticFieldSize, true);
      visitor.doCInt(staticOopFieldCount, true);
      visitor.doCInt(nonstaticOopMapSize, true);
      visitor.doCInt(initState, true);
      visitor.doCInt(itableLen, true);
    }

  /*
   *  Visit the static fields of this InstanceKlass with the obj of
   *  the visitor set to the oop holding the fields, which is
   *  currently the java mirror.
   */
  public void iterateStaticFields(OopVisitor visitor) {
    visitor.setObj(getJavaMirror());
    visitor.prologue();
    iterateStaticFieldsInternal(visitor);
    visitor.epilogue();

  }

  void iterateStaticFieldsInternal(OopVisitor visitor) {
    int length = getJavaFieldsCount();
    for (int index = 0; index < length; index++) {
      short accessFlags    = getFieldAccessFlags(index);
      FieldType   type   = new FieldType(getFieldSignature(index));
      AccessFlags access = new AccessFlags(accessFlags);
      if (access.isStatic()) {
        visitField(visitor, type, index);
      }
    }
  }

  public Klass getJavaSuper() {
    return getSuper();
  }

  public static class StaticField {
    public AccessFlags flags;
    public Field field;

    StaticField(Field field, AccessFlags flags) {
      this.field = field;
      this.flags = flags;
    }
  }

  public Field[] getStaticFields() {
    int length = getJavaFieldsCount();
    ArrayList<Field> result = new ArrayList<>();
    for (int index = 0; index < length; index++) {
      Field f = newField(index);
      if (f.isStatic()) {
        result.add(f);
      }
    }
    return result.toArray(new Field[result.size()]);
  }

  public void iterateNonStaticFields(OopVisitor visitor, Oop obj) {
    if (getSuper() != null) {
      ((InstanceKlass) getSuper()).iterateNonStaticFields(visitor, obj);
    }
    int length = getJavaFieldsCount();
    for (int index = 0; index < length; index++) {
      short accessFlags    = getFieldAccessFlags(index);
      FieldType   type   = new FieldType(getFieldSignature(index));
      AccessFlags access = new AccessFlags(accessFlags);
      if (!access.isStatic()) {
        visitField(visitor, type, index);
      }
    }
  }

  /** Field access by name. */
  public Field findLocalField(String name, String sig) {
    int length = getJavaFieldsCount();
    for (int i = 0; i < length; i++) {
      Symbol f_name = getFieldName(i);
      Symbol f_sig  = getFieldSignature(i);
      if (f_name.equals(name) && f_sig.equals(sig)) {
        return newField(i);
      }
    }

    return null;
  }

  /** Find field in direct superinterfaces. */
  public Field findInterfaceField(String name, String sig) {
    KlassArray interfaces = getLocalInterfaces();
    int n = interfaces.length();
    for (int i = 0; i < n; i++) {
      InstanceKlass intf1 = (InstanceKlass) interfaces.getAt(i);
      if (Assert.ASSERTS_ENABLED) {
        Assert.that(intf1.isInterface(), "just checking type");
     }
      // search for field in current interface
      Field f = intf1.findLocalField(name, sig);
      if (f != null) {
        if (Assert.ASSERTS_ENABLED) {
          Assert.that(f.getAccessFlagsObj().isStatic(), "interface field must be static");
        }
        return f;
      }
      // search for field in direct superinterfaces
      f = intf1.findInterfaceField(name, sig);
      if (f != null) return f;
    }
    // otherwise field lookup fails
    return null;
  }

  /** Find field according to JVM spec 5.4.3.2, returns the klass in
      which the field is defined. */
  public Field findField(String name, String sig) {
    // search order according to newest JVM spec (5.4.3.2, p.167).
    // 1) search for field in current klass
    Field f = findLocalField(name, sig);
    if (f != null) return f;

    // 2) search for field recursively in direct superinterfaces
    f = findInterfaceField(name, sig);
    if (f != null) return f;

    // 3) apply field lookup recursively if superclass exists
    InstanceKlass supr = (InstanceKlass) getSuper();
    if (supr != null) return supr.findField(name, sig);

    // 4) otherwise field lookup fails
    return null;
  }

  /** Find field according to JVM spec 5.4.3.2, returns the klass in
      which the field is defined (retained only for backward
      compatibility with jdbx) */
  public Field findFieldDbg(String name, String sig) {
    return findField(name, sig);
  }

  /** Get field by its index in the fields array. Only designed for
      use in a debugging system. */
  public Field getFieldByIndex(int fieldIndex) {
    return newField(fieldIndex);
  }


    /** Return a List of SA Fields for the fields declared in this class.
        Inherited fields are not included.
        Return an empty list if there are no fields declared in this class.
        Only designed for use in a debugging system. */
    public List<Field> getImmediateFields() {
        // A list of Fields for each field declared in this class/interface,
        // not including inherited fields.
        int length = getJavaFieldsCount();
        List<Field> immediateFields = new ArrayList<>(length);
        for (int index = 0; index < length; index++) {
            immediateFields.add(getFieldByIndex(index));
        }

        return immediateFields;
    }

    /** Return a List of SA Fields for all the java fields in this class,
        including all inherited fields.  This includes hidden
        fields.  Thus the returned list can contain fields with
        the same name.
        Return an empty list if there are no fields.
        Only designed for use in a debugging system. */
    public List<Field> getAllFields() {
        // Contains a Field for each field in this class, including immediate
        // fields and inherited fields.
        List<Field> allFields = getImmediateFields();

        // transitiveInterfaces contains all interfaces implemented
        // by this class and its superclass chain with no duplicates.

        KlassArray interfaces = getTransitiveInterfaces();
        int n = interfaces.length();
        for (int i = 0; i < n; i++) {
            InstanceKlass intf1 = (InstanceKlass) interfaces.getAt(i);
            if (Assert.ASSERTS_ENABLED) {
                Assert.that(intf1.isInterface(), "just checking type");
            }
            allFields.addAll(intf1.getImmediateFields());
        }

        // Get all fields in the superclass, recursively.  But, don't
        // include fields in interfaces implemented by superclasses;
        // we already have all those.
        if (!isInterface()) {
            InstanceKlass supr;
            if  ( (supr = (InstanceKlass) getSuper()) != null) {
                allFields.addAll(supr.getImmediateFields());
            }
        }

        return allFields;
    }


    /** Return a List of SA Methods declared directly in this class/interface.
        Return an empty list if there are none, or if this isn't a class/
        interface.
    */
    public List<Method> getImmediateMethods() {
      // Contains a Method for each method declared in this class/interface
      // not including inherited methods.

      MethodArray methods = getMethods();
      int length = methods.length();
      Method[] tmp = new Method[length];

      IntArray methodOrdering = getMethodOrdering();
      if (methodOrdering.length() != length) {
         // no ordering info present
         for (int index = 0; index < length; index++) {
            tmp[index] = methods.at(index);
         }
      } else {
         for (int index = 0; index < length; index++) {
            int originalIndex = methodOrdering.at(index);
            tmp[originalIndex] = methods.at(index);
         }
      }

      return Arrays.asList(tmp);
    }

    /** Return a List containing an SA InstanceKlass for each
        interface named in this class's 'implements' clause.
    */
    public List<Klass> getDirectImplementedInterfaces() {
        // Contains an InstanceKlass for each interface in this classes
        // 'implements' clause.

        KlassArray interfaces = getLocalInterfaces();
        int length = interfaces.length();
        List<Klass> directImplementedInterfaces = new ArrayList<>(length);

        for (int index = 0; index < length; index ++) {
            directImplementedInterfaces.add(interfaces.getAt(index));
        }

        return directImplementedInterfaces;
    }

  public Klass arrayKlassImpl(boolean orNull, int n) {
    // FIXME: in reflective system this would need to change to
    // actually allocate
    if (getArrayKlasses() == null) { return null; }
    ObjArrayKlass oak = (ObjArrayKlass) getArrayKlasses();
    if (orNull) {
      return oak.arrayKlassOrNull(n);
    }
    return oak.arrayKlass(n);
  }

  public Klass arrayKlassImpl(boolean orNull) {
    return arrayKlassImpl(orNull, 1);
  }

  public String signature() {
     return "L" + super.signature() + ";";
  }

  /** Find method in vtable. */
  public Method findMethod(String name, String sig) {
    return findMethod(getMethods(), name, sig);
  }

  /** Breakpoint support (see methods on Method* for details) */
  public BreakpointInfo getBreakpoints() {
    if (!VM.getVM().isJvmtiSupported()) {
      return null;
    }
    Address addr = getAddress().getAddressAt(breakpoints.getOffset());
    return VMObjectFactory.newObject(BreakpointInfo.class, addr);
  }

  public IntArray  getMethodOrdering() {
    Address addr = getAddress().getAddressAt(methodOrdering.getOffset());
    return VMObjectFactory.newObject(IntArray.class, addr);
  }

  public U1Array getFieldInfoStream() {
    Address addr = getAddress().getAddressAt(fieldinfoStream.getOffset());
    return VMObjectFactory.newObject(U1Array.class, addr);
  }

  public U2Array getInnerClasses() {
    Address addr = getAddress().getAddressAt(innerClasses.getOffset());
    return VMObjectFactory.newObject(U2Array.class, addr);
  }

  public U1Array getClassAnnotations() {
    Annotations annotations = getAnnotations();
    if (annotations != null) {
      return annotations.getClassAnnotations();
    } else {
      return null;
    }
  }

  public U1Array getClassTypeAnnotations() {
    Annotations annotations = getAnnotations();
    if (annotations != null) {
      return annotations.getClassTypeAnnotations();
    } else {
      return null;
    }
  }

  public U1Array getFieldAnnotations(int fieldIndex) {
    Annotations annotations = getAnnotations();
    if (annotations != null) {
      return annotations.getFieldAnnotations(fieldIndex);
    } else {
      return null;
    }
  }

  public U1Array getFieldTypeAnnotations(int fieldIndex) {
    Annotations annotations = getAnnotations();
    if (annotations != null) {
      return annotations.getFieldTypeAnnotations(fieldIndex);
    } else {
      return null;
    }
  }

  public U2Array getNestMembers() {
    Address addr = getAddress().getAddressAt(nestMembers.getOffset());
    return VMObjectFactory.newObject(U2Array.class, addr);
  }

  //----------------------------------------------------------------------
  // Internals only below this point
  //

  private void visitField(OopVisitor visitor, FieldType type, int index) {
    Field f = newField(index);
    if (type.isOop()) {
      visitor.doOop((OopField) f, false);
      return;
    }
    if (type.isByte()) {
      visitor.doByte((ByteField) f, false);
      return;
    }
    if (type.isChar()) {
      visitor.doChar((CharField) f, false);
      return;
    }
    if (type.isDouble()) {
      visitor.doDouble((DoubleField) f, false);
      return;
    }
    if (type.isFloat()) {
      visitor.doFloat((FloatField) f, false);
      return;
    }
    if (type.isInt()) {
      visitor.doInt((IntField) f, false);
      return;
    }
    if (type.isLong()) {
      visitor.doLong((LongField) f, false);
      return;
    }
    if (type.isShort()) {
      visitor.doShort((ShortField) f, false);
      return;
    }
    if (type.isBoolean()) {
      visitor.doBoolean((BooleanField) f, false);
      return;
    }
  }

  // Creates new field from index in fields TypeArray
  private Field newField(int index) {
    FieldType type = new FieldType(getFieldSignature(index));
    if (type.isOop()) {
     if (VM.getVM().isCompressedOopsEnabled()) {
        return new NarrowOopField(this, index);
     } else {
        return new OopField(this, index);
     }
    }
    if (type.isByte()) {
      return new ByteField(this, index);
    }
    if (type.isChar()) {
      return new CharField(this, index);
    }
    if (type.isDouble()) {
      return new DoubleField(this, index);
    }
    if (type.isFloat()) {
      return new FloatField(this, index);
    }
    if (type.isInt()) {
      return new IntField(this, index);
    }
    if (type.isLong()) {
      return new LongField(this, index);
    }
    if (type.isShort()) {
      return new ShortField(this, index);
    }
    if (type.isBoolean()) {
      return new BooleanField(this, index);
    }
    throw new RuntimeException("Illegal field type at index " + index);
  }

  private static Method findMethod(MethodArray methods, String name, String signature) {
    int index = linearSearch(methods, name, signature);
    if (index != -1) {
      return methods.at(index);
    } else {
      return null;
    }
  }

  private static int linearSearch(MethodArray methods, String name, String signature) {
    int len = methods.length();
    for (int index = 0; index < len; index++) {
      Method m = methods.at(index);
      if (m.getSignature().equals(signature) && m.getName().equals(name)) {
        return index;
      }
    }
    return -1;
  }
}
