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

#include "asm/macroAssembler.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/interp_masm.hpp"
#include "interpreter/templateTable.hpp"
#include "memory/universe.hpp"
#include "oops/methodCounters.hpp"
#include "oops/methodData.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "oops/resolvedFieldEntry.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "oops/resolvedMethodEntry.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"
#include "utilities/macros.hpp"

#define __ Disassembler::hook<InterpreterMacroAssembler>(__FILE__, __LINE__, _masm)->

// Global Register Names
static const Register rbcp     = r13;
static const Register rlocals  = r14;

// Address Computation: local variables
static inline Address iaddress(int n) {
  return Address(rlocals, Interpreter::local_offset_in_bytes(n));
}

static inline Address laddress(int n) {
  return iaddress(n + 1);
}

static inline Address faddress(int n) {
  return iaddress(n);
}

static inline Address daddress(int n) {
  return laddress(n);
}

static inline Address aaddress(int n) {
  return iaddress(n);
}

static inline Address iaddress(Register r) {
  return Address(rlocals, r, Address::times_ptr);
}

static inline Address laddress(Register r) {
  return Address(rlocals, r, Address::times_ptr, Interpreter::local_offset_in_bytes(1));
}

static inline Address faddress(Register r) {
  return iaddress(r);
}

static inline Address daddress(Register r) {
  return laddress(r);
}

static inline Address aaddress(Register r) {
  return iaddress(r);
}


// expression stack
// (Note: Must not use symmetric equivalents at_rsp_m1/2 since they store
// data beyond the rsp which is potentially unsafe in an MT environment;
// an interrupt may overwrite that data.)
static inline Address at_rsp   () {
  return Address(rsp, 0);
}

// At top of Java expression stack which may be different than esp().  It
// isn't for category 1 objects.
static inline Address at_tos   () {
  return Address(rsp,  Interpreter::expr_offset_in_bytes(0));
}

static inline Address at_tos_p1() {
  return Address(rsp,  Interpreter::expr_offset_in_bytes(1));
}

static inline Address at_tos_p2() {
  return Address(rsp,  Interpreter::expr_offset_in_bytes(2));
}

// Condition conversion
static Assembler::Condition j_not(TemplateTable::Condition cc) {
  switch (cc) {
  case TemplateTable::equal        : return Assembler::notEqual;
  case TemplateTable::not_equal    : return Assembler::equal;
  case TemplateTable::less         : return Assembler::greaterEqual;
  case TemplateTable::less_equal   : return Assembler::greater;
  case TemplateTable::greater      : return Assembler::lessEqual;
  case TemplateTable::greater_equal: return Assembler::less;
  }
  ShouldNotReachHere();
  return Assembler::zero;
}



// Miscellaneous helper routines
// Store an oop (or null) at the address described by obj.
// If val == noreg this means store a null


static void do_oop_store(InterpreterMacroAssembler* _masm,
                         Address dst,
                         Register val,
                         DecoratorSet decorators = 0) {
  assert(val == noreg || val == rax, "parameter is just for looks");
  __ store_heap_oop(dst, val, rscratch2, r9, r8, decorators);
}

static void do_oop_load(InterpreterMacroAssembler* _masm,
                        Address src,
                        Register dst,
                        DecoratorSet decorators = 0) {
  __ load_heap_oop(dst, src, rdx, decorators);
}

Address TemplateTable::at_bcp(int offset) {
  assert(_desc->uses_bcp(), "inconsistent uses_bcp information");
  return Address(rbcp, offset);
}


void TemplateTable::patch_bytecode(Bytecodes::Code bc, Register bc_reg,
                                   Register temp_reg, bool load_bc_into_bc_reg/*=true*/,
                                   int byte_no) {
  if (!RewriteBytecodes)  return;
  Label L_patch_done;

  switch (bc) {
  case Bytecodes::_fast_aputfield:
  case Bytecodes::_fast_bputfield:
  case Bytecodes::_fast_zputfield:
  case Bytecodes::_fast_cputfield:
  case Bytecodes::_fast_dputfield:
  case Bytecodes::_fast_fputfield:
  case Bytecodes::_fast_iputfield:
  case Bytecodes::_fast_lputfield:
  case Bytecodes::_fast_sputfield:
    {
      // We skip bytecode quickening for putfield instructions when
      // the put_code written to the constant pool cache is zero.
      // This is required so that every execution of this instruction
      // calls out to InterpreterRuntime::resolve_get_put to do
      // additional, required work.
      assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
      assert(load_bc_into_bc_reg, "we use bc_reg as temp");
      __ load_field_entry(temp_reg, bc_reg);
      if (byte_no == f1_byte) {
        __ load_unsigned_byte(temp_reg, Address(temp_reg, in_bytes(ResolvedFieldEntry::get_code_offset())));
      } else {
        __ load_unsigned_byte(temp_reg, Address(temp_reg, in_bytes(ResolvedFieldEntry::put_code_offset())));
      }

      __ movl(bc_reg, bc);
      __ cmpl(temp_reg, (int) 0);
      __ jcc(Assembler::zero, L_patch_done);  // don't patch
    }
    break;
  default:
    assert(byte_no == -1, "sanity");
    // the pair bytecodes have already done the load.
    if (load_bc_into_bc_reg) {
      __ movl(bc_reg, bc);
    }
  }

  if (JvmtiExport::can_post_breakpoint()) {
    Label L_fast_patch;
    // if a breakpoint is present we can't rewrite the stream directly
    __ movzbl(temp_reg, at_bcp(0));
    __ cmpl(temp_reg, Bytecodes::_breakpoint);
    __ jcc(Assembler::notEqual, L_fast_patch);
    __ get_method(temp_reg);
    // Let breakpoint table handling rewrite to quicker bytecode
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::set_original_bytecode_at), temp_reg, rbcp, bc_reg);
#ifndef ASSERT
    __ jmpb(L_patch_done);
#else
    __ jmp(L_patch_done);
#endif
    __ bind(L_fast_patch);
  }

#ifdef ASSERT
  Label L_okay;
  __ load_unsigned_byte(temp_reg, at_bcp(0));
  __ cmpl(temp_reg, (int) Bytecodes::java_code(bc));
  __ jcc(Assembler::equal, L_okay);
  __ cmpl(temp_reg, bc_reg);
  __ jcc(Assembler::equal, L_okay);
  __ stop("patching the wrong bytecode");
  __ bind(L_okay);
#endif

  // patch bytecode
  __ movb(at_bcp(0), bc_reg);
  __ bind(L_patch_done);
}
// Individual instructions


void TemplateTable::nop() {
  transition(vtos, vtos);
  // nothing to do
}

void TemplateTable::shouldnotreachhere() {
  transition(vtos, vtos);
  __ stop("shouldnotreachhere bytecode");
}

void TemplateTable::aconst_null() {
  transition(vtos, atos);
  __ xorl(rax, rax);
}

void TemplateTable::iconst(int value) {
  transition(vtos, itos);
  if (value == 0) {
    __ xorl(rax, rax);
  } else {
    __ movl(rax, value);
  }
}

void TemplateTable::lconst(int value) {
  transition(vtos, ltos);
  if (value == 0) {
    __ xorl(rax, rax);
  } else {
    __ movl(rax, value);
  }
}



void TemplateTable::fconst(int value) {
  transition(vtos, ftos);
  static float one = 1.0f, two = 2.0f;
  switch (value) {
  case 0:
    __ xorps(xmm0, xmm0);
    break;
  case 1:
    __ movflt(xmm0, ExternalAddress((address) &one), rscratch1);
    break;
  case 2:
    __ movflt(xmm0, ExternalAddress((address) &two), rscratch1);
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

void TemplateTable::dconst(int value) {
  transition(vtos, dtos);
  static double one = 1.0;
  switch (value) {
  case 0:
    __ xorpd(xmm0, xmm0);
    break;
  case 1:
    __ movdbl(xmm0, ExternalAddress((address) &one), rscratch1);
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

void TemplateTable::bipush() {
  transition(vtos, itos);
  __ load_signed_byte(rax, at_bcp(1));
}

void TemplateTable::sipush() {
  transition(vtos, itos);
  __ load_unsigned_short(rax, at_bcp(1));
  __ bswapl(rax);
  __ sarl(rax, 16);
}

void TemplateTable::ldc(LdcType type) {
  transition(vtos, vtos);
  Register rarg = c_rarg1;
  Label call_ldc, notFloat, notClass, notInt, Done;

  if (is_ldc_wide(type)) {
    __ get_unsigned_2_byte_index_at_bcp(rbx, 1);
  } else {
    __ load_unsigned_byte(rbx, at_bcp(1));
  }

  __ get_cpool_and_tags(rcx, rax);
  const int base_offset = ConstantPool::header_size() * wordSize;
  const int tags_offset = Array<u1>::base_offset_in_bytes();

  // get type
  __ movzbl(rdx, Address(rax, rbx, Address::times_1, tags_offset));

  // unresolved class - get the resolved class
  __ cmpl(rdx, JVM_CONSTANT_UnresolvedClass);
  __ jccb(Assembler::equal, call_ldc);

  // unresolved class in error state - call into runtime to throw the error
  // from the first resolution attempt
  __ cmpl(rdx, JVM_CONSTANT_UnresolvedClassInError);
  __ jccb(Assembler::equal, call_ldc);

  // resolved class - need to call vm to get java mirror of the class
  __ cmpl(rdx, JVM_CONSTANT_Class);
  __ jcc(Assembler::notEqual, notClass);

  __ bind(call_ldc);

  __ movl(rarg, is_ldc_wide(type) ? 1 : 0);
  call_VM(rax, CAST_FROM_FN_PTR(address, InterpreterRuntime::ldc), rarg);

  __ push(atos);
  __ jmp(Done);

  __ bind(notClass);
  __ cmpl(rdx, JVM_CONSTANT_Float);
  __ jccb(Assembler::notEqual, notFloat);

  // ftos
  __ movflt(xmm0, Address(rcx, rbx, Address::times_ptr, base_offset));
  __ push(ftos);
  __ jmp(Done);

  __ bind(notFloat);
  __ cmpl(rdx, JVM_CONSTANT_Integer);
  __ jccb(Assembler::notEqual, notInt);

  // itos
  __ movl(rax, Address(rcx, rbx, Address::times_ptr, base_offset));
  __ push(itos);
  __ jmp(Done);

  // assume the tag is for condy; if not, the VM runtime will tell us
  __ bind(notInt);
  condy_helper(Done);

  __ bind(Done);
}

// Fast path for caching oop constants.
void TemplateTable::fast_aldc(LdcType type) {
  transition(vtos, atos);

  Register result = rax;
  Register tmp = rdx;
  Register rarg = c_rarg1;
  int index_size = is_ldc_wide(type) ? sizeof(u2) : sizeof(u1);

  Label resolved;

  // We are resolved if the resolved reference cache entry contains a
  // non-null object (String, MethodType, etc.)
  assert_different_registers(result, tmp);
  __ get_cache_index_at_bcp(tmp, 1, index_size);
  __ load_resolved_reference_at_index(result, tmp);
  __ testptr(result, result);
  __ jcc(Assembler::notZero, resolved);

  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_ldc);

  // first time invocation - must resolve first
  __ movl(rarg, (int)bytecode());
  __ call_VM(result, entry, rarg);
  __ bind(resolved);

  { // Check for the null sentinel.
    // If we just called the VM, it already did the mapping for us,
    // but it's harmless to retry.
    Label notNull;
    ExternalAddress null_sentinel((address)Universe::the_null_sentinel_addr());
    __ movptr(tmp, null_sentinel);
    __ resolve_oop_handle(tmp, rscratch2);
    __ cmpoop(tmp, result);
    __ jccb(Assembler::notEqual, notNull);
    __ xorptr(result, result);  // null object reference
    __ bind(notNull);
  }

  if (VerifyOops) {
    __ verify_oop(result);
  }
}

void TemplateTable::ldc2_w() {
  transition(vtos, vtos);
  Label notDouble, notLong, Done;
  __ get_unsigned_2_byte_index_at_bcp(rbx, 1);

  __ get_cpool_and_tags(rcx, rax);
  const int base_offset = ConstantPool::header_size() * wordSize;
  const int tags_offset = Array<u1>::base_offset_in_bytes();

  // get type
  __ movzbl(rdx, Address(rax, rbx, Address::times_1, tags_offset));
  __ cmpl(rdx, JVM_CONSTANT_Double);
  __ jccb(Assembler::notEqual, notDouble);

  // dtos
  __ movdbl(xmm0, Address(rcx, rbx, Address::times_ptr, base_offset));
  __ push(dtos);

  __ jmp(Done);
  __ bind(notDouble);
  __ cmpl(rdx, JVM_CONSTANT_Long);
  __ jccb(Assembler::notEqual, notLong);

  // ltos
  __ movptr(rax, Address(rcx, rbx, Address::times_ptr, base_offset + 0 * wordSize));
  __ push(ltos);
  __ jmp(Done);

  __ bind(notLong);
  condy_helper(Done);

  __ bind(Done);
}

void TemplateTable::condy_helper(Label& Done) {
  const Register obj = rax;
  const Register off = rbx;
  const Register flags = rcx;
  const Register rarg = c_rarg1;
  __ movl(rarg, (int)bytecode());
  call_VM(obj, CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_ldc), rarg);
  __ get_vm_result_metadata(flags);
  // VMr = obj = base address to find primitive value to push
  // VMr2 = flags = (tos, off) using format of CPCE::_flags
  __ movl(off, flags);
  __ andl(off, ConstantPoolCache::field_index_mask);
  const Address field(obj, off, Address::times_1, 0*wordSize);

  // What sort of thing are we loading?
  __ shrl(flags, ConstantPoolCache::tos_state_shift);
  __ andl(flags, ConstantPoolCache::tos_state_mask);

  switch (bytecode()) {
  case Bytecodes::_ldc:
  case Bytecodes::_ldc_w:
    {
      // tos in (itos, ftos, stos, btos, ctos, ztos)
      Label notInt, notFloat, notShort, notByte, notChar, notBool;
      __ cmpl(flags, itos);
      __ jccb(Assembler::notEqual, notInt);
      // itos
      __ movl(rax, field);
      __ push(itos);
      __ jmp(Done);

      __ bind(notInt);
      __ cmpl(flags, ftos);
      __ jccb(Assembler::notEqual, notFloat);
      // ftos
      __ movflt(xmm0, field);
      __ push(ftos);
      __ jmp(Done);

      __ bind(notFloat);
      __ cmpl(flags, stos);
      __ jccb(Assembler::notEqual, notShort);
      // stos
      __ load_signed_short(rax, field);
      __ push(stos);
      __ jmp(Done);

      __ bind(notShort);
      __ cmpl(flags, btos);
      __ jccb(Assembler::notEqual, notByte);
      // btos
      __ load_signed_byte(rax, field);
      __ push(btos);
      __ jmp(Done);

      __ bind(notByte);
      __ cmpl(flags, ctos);
      __ jccb(Assembler::notEqual, notChar);
      // ctos
      __ load_unsigned_short(rax, field);
      __ push(ctos);
      __ jmp(Done);

      __ bind(notChar);
      __ cmpl(flags, ztos);
      __ jccb(Assembler::notEqual, notBool);
      // ztos
      __ load_signed_byte(rax, field);
      __ push(ztos);
      __ jmp(Done);

      __ bind(notBool);
      break;
    }

  case Bytecodes::_ldc2_w:
    {
      Label notLong, notDouble;
      __ cmpl(flags, ltos);
      __ jccb(Assembler::notEqual, notLong);
      // ltos
      // Loading high word first because movptr clobbers rax
      __ movptr(rax, field);
      __ push(ltos);
      __ jmp(Done);

      __ bind(notLong);
      __ cmpl(flags, dtos);
      __ jccb(Assembler::notEqual, notDouble);
      // dtos
      __ movdbl(xmm0, field);
      __ push(dtos);
      __ jmp(Done);

      __ bind(notDouble);
      break;
    }

  default:
    ShouldNotReachHere();
  }

  __ stop("bad ldc/condy");
}

void TemplateTable::locals_index(Register reg, int offset) {
  __ load_unsigned_byte(reg, at_bcp(offset));
  __ negptr(reg);
}

void TemplateTable::iload() {
  iload_internal();
}

void TemplateTable::nofast_iload() {
  iload_internal(may_not_rewrite);
}

void TemplateTable::iload_internal(RewriteControl rc) {
  transition(vtos, itos);
  if (RewriteFrequentPairs && rc == may_rewrite) {
    Label rewrite, done;
    const Register bc = c_rarg3;
    assert(rbx != bc, "register damaged");

    // get next byte
    __ load_unsigned_byte(rbx,
                          at_bcp(Bytecodes::length_for(Bytecodes::_iload)));
    // if _iload, wait to rewrite to iload2.  We only want to rewrite the
    // last two iloads in a pair.  Comparing against fast_iload means that
    // the next bytecode is neither an iload or a caload, and therefore
    // an iload pair.
    __ cmpl(rbx, Bytecodes::_iload);
    __ jcc(Assembler::equal, done);

    __ cmpl(rbx, Bytecodes::_fast_iload);
    __ movl(bc, Bytecodes::_fast_iload2);

    __ jccb(Assembler::equal, rewrite);

    // if _caload, rewrite to fast_icaload
    __ cmpl(rbx, Bytecodes::_caload);
    __ movl(bc, Bytecodes::_fast_icaload);
    __ jccb(Assembler::equal, rewrite);

    // rewrite so iload doesn't check again.
    __ movl(bc, Bytecodes::_fast_iload);

    // rewrite
    // bc: fast bytecode
    __ bind(rewrite);
    patch_bytecode(Bytecodes::_iload, bc, rbx, false);
    __ bind(done);
  }

  // Get the local value into tos
  locals_index(rbx);
  __ movl(rax, iaddress(rbx));
}

void TemplateTable::fast_iload2() {
  transition(vtos, itos);
  locals_index(rbx);
  __ movl(rax, iaddress(rbx));
  __ push(itos);
  locals_index(rbx, 3);
  __ movl(rax, iaddress(rbx));
}

void TemplateTable::fast_iload() {
  transition(vtos, itos);
  locals_index(rbx);
  __ movl(rax, iaddress(rbx));
}

void TemplateTable::lload() {
  transition(vtos, ltos);
  locals_index(rbx);
  __ movptr(rax, laddress(rbx));
}

void TemplateTable::fload() {
  transition(vtos, ftos);
  locals_index(rbx);
  __ movflt(xmm0, faddress(rbx));
}

void TemplateTable::dload() {
  transition(vtos, dtos);
  locals_index(rbx);
  __ movdbl(xmm0, daddress(rbx));
}

void TemplateTable::aload() {
  transition(vtos, atos);
  locals_index(rbx);
  __ movptr(rax, aaddress(rbx));
}

void TemplateTable::locals_index_wide(Register reg) {
  __ load_unsigned_short(reg, at_bcp(2));
  __ bswapl(reg);
  __ shrl(reg, 16);
  __ negptr(reg);
}

void TemplateTable::wide_iload() {
  transition(vtos, itos);
  locals_index_wide(rbx);
  __ movl(rax, iaddress(rbx));
}

void TemplateTable::wide_lload() {
  transition(vtos, ltos);
  locals_index_wide(rbx);
  __ movptr(rax, laddress(rbx));
}

void TemplateTable::wide_fload() {
  transition(vtos, ftos);
  locals_index_wide(rbx);
  __ movflt(xmm0, faddress(rbx));
}

void TemplateTable::wide_dload() {
  transition(vtos, dtos);
  locals_index_wide(rbx);
  __ movdbl(xmm0, daddress(rbx));
}

void TemplateTable::wide_aload() {
  transition(vtos, atos);
  locals_index_wide(rbx);
  __ movptr(rax, aaddress(rbx));
}

void TemplateTable::index_check(Register array, Register index) {
  // Pop ptr into array
  __ pop_ptr(array);
  index_check_without_pop(array, index);
}

void TemplateTable::index_check_without_pop(Register array, Register index) {
  // destroys rbx
  // sign extend index for use by indexed load
  __ movl2ptr(index, index);
  // check index
  __ cmpl(index, Address(array, arrayOopDesc::length_offset_in_bytes()));
  if (index != rbx) {
    // ??? convention: move aberrant index into rbx for exception message
    assert(rbx != array, "different registers");
    __ movl(rbx, index);
  }
  Label skip;
  __ jccb(Assembler::below, skip);
  // Pass array to create more detailed exceptions.
  __ mov(c_rarg1, array);
  __ jump(RuntimeAddress(Interpreter::_throw_ArrayIndexOutOfBoundsException_entry));
  __ bind(skip);
}

void TemplateTable::iaload() {
  transition(itos, itos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_INT, IN_HEAP | IS_ARRAY, rax,
                    Address(rdx, rax, Address::times_4,
                            arrayOopDesc::base_offset_in_bytes(T_INT)),
                    noreg);
}

void TemplateTable::laload() {
  transition(itos, ltos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  // rbx,: index
  __ access_load_at(T_LONG, IN_HEAP | IS_ARRAY, noreg /* ltos */,
                    Address(rdx, rbx, Address::times_8,
                            arrayOopDesc::base_offset_in_bytes(T_LONG)),
                    noreg);
}



void TemplateTable::faload() {
  transition(itos, ftos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_FLOAT, IN_HEAP | IS_ARRAY, noreg /* ftos */,
                    Address(rdx, rax,
                            Address::times_4,
                            arrayOopDesc::base_offset_in_bytes(T_FLOAT)),
                    noreg);
}

void TemplateTable::daload() {
  transition(itos, dtos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_DOUBLE, IN_HEAP | IS_ARRAY, noreg /* dtos */,
                    Address(rdx, rax,
                            Address::times_8,
                            arrayOopDesc::base_offset_in_bytes(T_DOUBLE)),
                    noreg);
}

void TemplateTable::aaload() {
  transition(itos, atos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  do_oop_load(_masm,
              Address(rdx, rax,
                      UseCompressedOops ? Address::times_4 : Address::times_ptr,
                      arrayOopDesc::base_offset_in_bytes(T_OBJECT)),
              rax,
              IS_ARRAY);
}

void TemplateTable::baload() {
  transition(itos, itos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_BYTE, IN_HEAP | IS_ARRAY, rax,
                    Address(rdx, rax, Address::times_1, arrayOopDesc::base_offset_in_bytes(T_BYTE)),
                    noreg);
}

void TemplateTable::caload() {
  transition(itos, itos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_CHAR, IN_HEAP | IS_ARRAY, rax,
                    Address(rdx, rax, Address::times_2, arrayOopDesc::base_offset_in_bytes(T_CHAR)),
                    noreg);
}

// iload followed by caload frequent pair
void TemplateTable::fast_icaload() {
  transition(vtos, itos);
  // load index out of locals
  locals_index(rbx);
  __ movl(rax, iaddress(rbx));

  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_CHAR, IN_HEAP | IS_ARRAY, rax,
                    Address(rdx, rax, Address::times_2, arrayOopDesc::base_offset_in_bytes(T_CHAR)),
                    noreg);
}


void TemplateTable::saload() {
  transition(itos, itos);
  // rax: index
  // rdx: array
  index_check(rdx, rax); // kills rbx
  __ access_load_at(T_SHORT, IN_HEAP | IS_ARRAY, rax,
                    Address(rdx, rax, Address::times_2, arrayOopDesc::base_offset_in_bytes(T_SHORT)),
                    noreg);
}

void TemplateTable::iload(int n) {
  transition(vtos, itos);
  __ movl(rax, iaddress(n));
}

void TemplateTable::lload(int n) {
  transition(vtos, ltos);
  __ movptr(rax, laddress(n));
}

void TemplateTable::fload(int n) {
  transition(vtos, ftos);
  __ movflt(xmm0, faddress(n));
}

void TemplateTable::dload(int n) {
  transition(vtos, dtos);
  __ movdbl(xmm0, daddress(n));
}

void TemplateTable::aload(int n) {
  transition(vtos, atos);
  __ movptr(rax, aaddress(n));
}

void TemplateTable::aload_0() {
  aload_0_internal();
}

void TemplateTable::nofast_aload_0() {
  aload_0_internal(may_not_rewrite);
}

void TemplateTable::aload_0_internal(RewriteControl rc) {
  transition(vtos, atos);
  // According to bytecode histograms, the pairs:
  //
  // _aload_0, _fast_igetfield
  // _aload_0, _fast_agetfield
  // _aload_0, _fast_fgetfield
  //
  // occur frequently. If RewriteFrequentPairs is set, the (slow)
  // _aload_0 bytecode checks if the next bytecode is either
  // _fast_igetfield, _fast_agetfield or _fast_fgetfield and then
  // rewrites the current bytecode into a pair bytecode; otherwise it
  // rewrites the current bytecode into _fast_aload_0 that doesn't do
  // the pair check anymore.
  //
  // Note: If the next bytecode is _getfield, the rewrite must be
  //       delayed, otherwise we may miss an opportunity for a pair.
  //
  // Also rewrite frequent pairs
  //   aload_0, aload_1
  //   aload_0, iload_1
  // These bytecodes with a small amount of code are most profitable
  // to rewrite
  if (RewriteFrequentPairs && rc == may_rewrite) {
    Label rewrite, done;

    const Register bc = c_rarg3;
    assert(rbx != bc, "register damaged");

    // get next byte
    __ load_unsigned_byte(rbx, at_bcp(Bytecodes::length_for(Bytecodes::_aload_0)));

    // if _getfield then wait with rewrite
    __ cmpl(rbx, Bytecodes::_getfield);
    __ jcc(Assembler::equal, done);

    // if _igetfield then rewrite to _fast_iaccess_0
    assert(Bytecodes::java_code(Bytecodes::_fast_iaccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ cmpl(rbx, Bytecodes::_fast_igetfield);
    __ movl(bc, Bytecodes::_fast_iaccess_0);
    __ jccb(Assembler::equal, rewrite);

    // if _agetfield then rewrite to _fast_aaccess_0
    assert(Bytecodes::java_code(Bytecodes::_fast_aaccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ cmpl(rbx, Bytecodes::_fast_agetfield);
    __ movl(bc, Bytecodes::_fast_aaccess_0);
    __ jccb(Assembler::equal, rewrite);

    // if _fgetfield then rewrite to _fast_faccess_0
    assert(Bytecodes::java_code(Bytecodes::_fast_faccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ cmpl(rbx, Bytecodes::_fast_fgetfield);
    __ movl(bc, Bytecodes::_fast_faccess_0);
    __ jccb(Assembler::equal, rewrite);

    // else rewrite to _fast_aload0
    assert(Bytecodes::java_code(Bytecodes::_fast_aload_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ movl(bc, Bytecodes::_fast_aload_0);

    // rewrite
    // bc: fast bytecode
    __ bind(rewrite);
    patch_bytecode(Bytecodes::_aload_0, bc, rbx, false);

    __ bind(done);
  }

  // Do actual aload_0 (must do this after patch_bytecode which might call VM and GC might change oop).
  aload(0);
}

void TemplateTable::istore() {
  transition(itos, vtos);
  locals_index(rbx);
  __ movl(iaddress(rbx), rax);
}


void TemplateTable::lstore() {
  transition(ltos, vtos);
  locals_index(rbx);
  __ movptr(laddress(rbx), rax);
}

void TemplateTable::fstore() {
  transition(ftos, vtos);
  locals_index(rbx);
  __ movflt(faddress(rbx), xmm0);
}

void TemplateTable::dstore() {
  transition(dtos, vtos);
  locals_index(rbx);
  __ movdbl(daddress(rbx), xmm0);
}

void TemplateTable::astore() {
  transition(vtos, vtos);
  __ pop_ptr(rax);
  locals_index(rbx);
  __ movptr(aaddress(rbx), rax);
}

void TemplateTable::wide_istore() {
  transition(vtos, vtos);
  __ pop_i();
  locals_index_wide(rbx);
  __ movl(iaddress(rbx), rax);
}

void TemplateTable::wide_lstore() {
  transition(vtos, vtos);
  __ pop_l();
  locals_index_wide(rbx);
  __ movptr(laddress(rbx), rax);
}

void TemplateTable::wide_fstore() {
  transition(vtos, vtos);
  __ pop_f(xmm0);
  locals_index_wide(rbx);
  __ movflt(faddress(rbx), xmm0);
}

void TemplateTable::wide_dstore() {
  transition(vtos, vtos);
  __ pop_d(xmm0);
  locals_index_wide(rbx);
  __ movdbl(daddress(rbx), xmm0);
}

void TemplateTable::wide_astore() {
  transition(vtos, vtos);
  __ pop_ptr(rax);
  locals_index_wide(rbx);
  __ movptr(aaddress(rbx), rax);
}

void TemplateTable::iastore() {
  transition(itos, vtos);
  __ pop_i(rbx);
  // rax: value
  // rbx: index
  // rdx: array
  index_check(rdx, rbx); // prefer index in rbx
  __ access_store_at(T_INT, IN_HEAP | IS_ARRAY,
                     Address(rdx, rbx, Address::times_4,
                             arrayOopDesc::base_offset_in_bytes(T_INT)),
                     rax, noreg, noreg, noreg);
}

void TemplateTable::lastore() {
  transition(ltos, vtos);
  __ pop_i(rbx);
  // rax,: low(value)
  // rcx: array
  // rdx: high(value)
  index_check(rcx, rbx);  // prefer index in rbx,
  // rbx,: index
  __ access_store_at(T_LONG, IN_HEAP | IS_ARRAY,
                     Address(rcx, rbx, Address::times_8,
                             arrayOopDesc::base_offset_in_bytes(T_LONG)),
                     noreg /* ltos */, noreg, noreg, noreg);
}


void TemplateTable::fastore() {
  transition(ftos, vtos);
  __ pop_i(rbx);
  // value is in xmm0
  // rbx:  index
  // rdx:  array
  index_check(rdx, rbx); // prefer index in rbx
  __ access_store_at(T_FLOAT, IN_HEAP | IS_ARRAY,
                     Address(rdx, rbx, Address::times_4,
                             arrayOopDesc::base_offset_in_bytes(T_FLOAT)),
                     noreg /* ftos */, noreg, noreg, noreg);
}

void TemplateTable::dastore() {
  transition(dtos, vtos);
  __ pop_i(rbx);
  // value is in xmm0
  // rbx:  index
  // rdx:  array
  index_check(rdx, rbx); // prefer index in rbx
  __ access_store_at(T_DOUBLE, IN_HEAP | IS_ARRAY,
                     Address(rdx, rbx, Address::times_8,
                             arrayOopDesc::base_offset_in_bytes(T_DOUBLE)),
                     noreg /* dtos */, noreg, noreg, noreg);
}

void TemplateTable::aastore() {
  Label is_null, ok_is_subtype, done;
  transition(vtos, vtos);
  // stack: ..., array, index, value
  __ movptr(rax, at_tos());    // value
  __ movl(rcx, at_tos_p1()); // index
  __ movptr(rdx, at_tos_p2()); // array

  Address element_address(rdx, rcx,
                          UseCompressedOops? Address::times_4 : Address::times_ptr,
                          arrayOopDesc::base_offset_in_bytes(T_OBJECT));

  index_check_without_pop(rdx, rcx);     // kills rbx
  __ testptr(rax, rax);
  __ jcc(Assembler::zero, is_null);

  // Move subklass into rbx
  __ load_klass(rbx, rax, rscratch1);
  // Move superklass into rax
  __ load_klass(rax, rdx, rscratch1);
  __ movptr(rax, Address(rax,
                         ObjArrayKlass::element_klass_offset()));

  // Generate subtype check.  Blows rcx, rdi
  // Superklass in rax.  Subklass in rbx.
  __ gen_subtype_check(rbx, ok_is_subtype);

  // Come here on failure
  // object is at TOS
  __ jump(RuntimeAddress(Interpreter::_throw_ArrayStoreException_entry));

  // Come here on success
  __ bind(ok_is_subtype);

  // Get the value we will store
  __ movptr(rax, at_tos());
  __ movl(rcx, at_tos_p1()); // index
  // Now store using the appropriate barrier
  do_oop_store(_masm, element_address, rax, IS_ARRAY);
  __ jmp(done);

  // Have a null in rax, rdx=array, ecx=index.  Store null at ary[idx]
  __ bind(is_null);
  __ profile_null_seen(rbx);

  // Store a null
  do_oop_store(_masm, element_address, noreg, IS_ARRAY);

  // Pop stack arguments
  __ bind(done);
  __ addptr(rsp, 3 * Interpreter::stackElementSize);
}

void TemplateTable::bastore() {
  transition(itos, vtos);
  __ pop_i(rbx);
  // rax: value
  // rbx: index
  // rdx: array
  index_check(rdx, rbx); // prefer index in rbx
  // Need to check whether array is boolean or byte
  // since both types share the bastore bytecode.
  __ load_klass(rcx, rdx, rscratch1);
  __ movl(rcx, Address(rcx, Klass::layout_helper_offset()));
  int diffbit = Klass::layout_helper_boolean_diffbit();
  __ testl(rcx, diffbit);
  Label L_skip;
  __ jccb(Assembler::zero, L_skip);
  __ andl(rax, 1);  // if it is a T_BOOLEAN array, mask the stored value to 0/1
  __ bind(L_skip);
  __ access_store_at(T_BYTE, IN_HEAP | IS_ARRAY,
                     Address(rdx, rbx,Address::times_1,
                             arrayOopDesc::base_offset_in_bytes(T_BYTE)),
                     rax, noreg, noreg, noreg);
}

void TemplateTable::castore() {
  transition(itos, vtos);
  __ pop_i(rbx);
  // rax: value
  // rbx: index
  // rdx: array
  index_check(rdx, rbx);  // prefer index in rbx
  __ access_store_at(T_CHAR, IN_HEAP | IS_ARRAY,
                     Address(rdx, rbx, Address::times_2,
                             arrayOopDesc::base_offset_in_bytes(T_CHAR)),
                     rax, noreg, noreg, noreg);
}


void TemplateTable::sastore() {
  castore();
}

void TemplateTable::istore(int n) {
  transition(itos, vtos);
  __ movl(iaddress(n), rax);
}

void TemplateTable::lstore(int n) {
  transition(ltos, vtos);
  __ movptr(laddress(n), rax);
}

void TemplateTable::fstore(int n) {
  transition(ftos, vtos);
  __ movflt(faddress(n), xmm0);
}

void TemplateTable::dstore(int n) {
  transition(dtos, vtos);
  __ movdbl(daddress(n), xmm0);
}


void TemplateTable::astore(int n) {
  transition(vtos, vtos);
  __ pop_ptr(rax);
  __ movptr(aaddress(n), rax);
}

void TemplateTable::pop() {
  transition(vtos, vtos);
  __ addptr(rsp, Interpreter::stackElementSize);
}

void TemplateTable::pop2() {
  transition(vtos, vtos);
  __ addptr(rsp, 2 * Interpreter::stackElementSize);
}


void TemplateTable::dup() {
  transition(vtos, vtos);
  __ load_ptr(0, rax);
  __ push_ptr(rax);
  // stack: ..., a, a
}

void TemplateTable::dup_x1() {
  transition(vtos, vtos);
  // stack: ..., a, b
  __ load_ptr( 0, rax);  // load b
  __ load_ptr( 1, rcx);  // load a
  __ store_ptr(1, rax);  // store b
  __ store_ptr(0, rcx);  // store a
  __ push_ptr(rax);      // push b
  // stack: ..., b, a, b
}

void TemplateTable::dup_x2() {
  transition(vtos, vtos);
  // stack: ..., a, b, c
  __ load_ptr( 0, rax);  // load c
  __ load_ptr( 2, rcx);  // load a
  __ store_ptr(2, rax);  // store c in a
  __ push_ptr(rax);      // push c
  // stack: ..., c, b, c, c
  __ load_ptr( 2, rax);  // load b
  __ store_ptr(2, rcx);  // store a in b
  // stack: ..., c, a, c, c
  __ store_ptr(1, rax);  // store b in c
  // stack: ..., c, a, b, c
}

void TemplateTable::dup2() {
  transition(vtos, vtos);
  // stack: ..., a, b
  __ load_ptr(1, rax);  // load a
  __ push_ptr(rax);     // push a
  __ load_ptr(1, rax);  // load b
  __ push_ptr(rax);     // push b
  // stack: ..., a, b, a, b
}


void TemplateTable::dup2_x1() {
  transition(vtos, vtos);
  // stack: ..., a, b, c
  __ load_ptr( 0, rcx);  // load c
  __ load_ptr( 1, rax);  // load b
  __ push_ptr(rax);      // push b
  __ push_ptr(rcx);      // push c
  // stack: ..., a, b, c, b, c
  __ store_ptr(3, rcx);  // store c in b
  // stack: ..., a, c, c, b, c
  __ load_ptr( 4, rcx);  // load a
  __ store_ptr(2, rcx);  // store a in 2nd c
  // stack: ..., a, c, a, b, c
  __ store_ptr(4, rax);  // store b in a
  // stack: ..., b, c, a, b, c
}

void TemplateTable::dup2_x2() {
  transition(vtos, vtos);
  // stack: ..., a, b, c, d
  __ load_ptr( 0, rcx);  // load d
  __ load_ptr( 1, rax);  // load c
  __ push_ptr(rax);      // push c
  __ push_ptr(rcx);      // push d
  // stack: ..., a, b, c, d, c, d
  __ load_ptr( 4, rax);  // load b
  __ store_ptr(2, rax);  // store b in d
  __ store_ptr(4, rcx);  // store d in b
  // stack: ..., a, d, c, b, c, d
  __ load_ptr( 5, rcx);  // load a
  __ load_ptr( 3, rax);  // load c
  __ store_ptr(3, rcx);  // store a in c
  __ store_ptr(5, rax);  // store c in a
  // stack: ..., c, d, a, b, c, d
}

void TemplateTable::swap() {
  transition(vtos, vtos);
  // stack: ..., a, b
  __ load_ptr( 1, rcx);  // load a
  __ load_ptr( 0, rax);  // load b
  __ store_ptr(0, rcx);  // store a in b
  __ store_ptr(1, rax);  // store b in a
  // stack: ..., b, a
}

void TemplateTable::iop2(Operation op) {
  transition(itos, itos);
  switch (op) {
  case add  :                    __ pop_i(rdx); __ addl (rax, rdx); break;
  case sub  : __ movl(rdx, rax); __ pop_i(rax); __ subl (rax, rdx); break;
  case mul  :                    __ pop_i(rdx); __ imull(rax, rdx); break;
  case _and :                    __ pop_i(rdx); __ andl (rax, rdx); break;
  case _or  :                    __ pop_i(rdx); __ orl  (rax, rdx); break;
  case _xor :                    __ pop_i(rdx); __ xorl (rax, rdx); break;
  case shl  : __ movl(rcx, rax); __ pop_i(rax); __ shll (rax);      break;
  case shr  : __ movl(rcx, rax); __ pop_i(rax); __ sarl (rax);      break;
  case ushr : __ movl(rcx, rax); __ pop_i(rax); __ shrl (rax);      break;
  default   : ShouldNotReachHere();
  }
}

void TemplateTable::lop2(Operation op) {
  transition(ltos, ltos);
  switch (op) {
  case add  :                    __ pop_l(rdx); __ addptr(rax, rdx); break;
  case sub  : __ mov(rdx, rax);  __ pop_l(rax); __ subptr(rax, rdx); break;
  case _and :                    __ pop_l(rdx); __ andptr(rax, rdx); break;
  case _or  :                    __ pop_l(rdx); __ orptr (rax, rdx); break;
  case _xor :                    __ pop_l(rdx); __ xorptr(rax, rdx); break;
  default   : ShouldNotReachHere();
  }
}

void TemplateTable::idiv() {
  transition(itos, itos);
  __ movl(rcx, rax);
  __ pop_i(rax);
  // Note: could xor rax and ecx and compare with (-1 ^ min_int). If
  //       they are not equal, one could do a normal division (no correction
  //       needed), which may speed up this implementation for the common case.
  //       (see also JVM spec., p.243 & p.271)
  __ corrected_idivl(rcx);
}

void TemplateTable::irem() {
  transition(itos, itos);
  __ movl(rcx, rax);
  __ pop_i(rax);
  // Note: could xor rax and ecx and compare with (-1 ^ min_int). If
  //       they are not equal, one could do a normal division (no correction
  //       needed), which may speed up this implementation for the common case.
  //       (see also JVM spec., p.243 & p.271)
  __ corrected_idivl(rcx);
  __ movl(rax, rdx);
}

void TemplateTable::lmul() {
  transition(ltos, ltos);
  __ pop_l(rdx);
  __ imulq(rax, rdx);
}

void TemplateTable::ldiv() {
  transition(ltos, ltos);
  __ mov(rcx, rax);
  __ pop_l(rax);
  // generate explicit div0 check
  __ testq(rcx, rcx);
  __ jump_cc(Assembler::zero,
             RuntimeAddress(Interpreter::_throw_ArithmeticException_entry));
  // Note: could xor rax and rcx and compare with (-1 ^ min_int). If
  //       they are not equal, one could do a normal division (no correction
  //       needed), which may speed up this implementation for the common case.
  //       (see also JVM spec., p.243 & p.271)
  __ corrected_idivq(rcx); // kills rbx
}

void TemplateTable::lrem() {
  transition(ltos, ltos);
  __ mov(rcx, rax);
  __ pop_l(rax);
  __ testq(rcx, rcx);
  __ jump_cc(Assembler::zero,
             RuntimeAddress(Interpreter::_throw_ArithmeticException_entry));
  // Note: could xor rax and rcx and compare with (-1 ^ min_int). If
  //       they are not equal, one could do a normal division (no correction
  //       needed), which may speed up this implementation for the common case.
  //       (see also JVM spec., p.243 & p.271)
  __ corrected_idivq(rcx); // kills rbx
  __ mov(rax, rdx);
}

void TemplateTable::lshl() {
  transition(itos, ltos);
  __ movl(rcx, rax);                             // get shift count
  __ pop_l(rax);                                 // get shift value
  __ shlq(rax);
}

void TemplateTable::lshr() {
  transition(itos, ltos);
  __ movl(rcx, rax);                             // get shift count
  __ pop_l(rax);                                 // get shift value
  __ sarq(rax);
}

void TemplateTable::lushr() {
  transition(itos, ltos);
  __ movl(rcx, rax);                             // get shift count
  __ pop_l(rax);                                 // get shift value
  __ shrq(rax);
}

void TemplateTable::fop2(Operation op) {
  transition(ftos, ftos);

  switch (op) {
  case add:
    __ addss(xmm0, at_rsp());
    __ addptr(rsp, Interpreter::stackElementSize);
    break;
  case sub:
    __ movflt(xmm1, xmm0);
    __ pop_f(xmm0);
    __ subss(xmm0, xmm1);
    break;
  case mul:
    __ mulss(xmm0, at_rsp());
    __ addptr(rsp, Interpreter::stackElementSize);
    break;
  case div:
    __ movflt(xmm1, xmm0);
    __ pop_f(xmm0);
    __ divss(xmm0, xmm1);
    break;
  case rem:
    // On x86_64 platforms the SharedRuntime::frem method is called to perform the
    // modulo operation. The frem method calls the function
    // double fmod(double x, double y) in math.h. The documentation of fmod states:
    // "If x or y is a NaN, a NaN is returned." without specifying what type of NaN
    // (signalling or quiet) is returned.
    __ movflt(xmm1, xmm0);
    __ pop_f(xmm0);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::frem), 2);
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

void TemplateTable::dop2(Operation op) {
  transition(dtos, dtos);
  switch (op) {
  case add:
    __ addsd(xmm0, at_rsp());
    __ addptr(rsp, 2 * Interpreter::stackElementSize);
    break;
  case sub:
    __ movdbl(xmm1, xmm0);
    __ pop_d(xmm0);
    __ subsd(xmm0, xmm1);
    break;
  case mul:
    __ mulsd(xmm0, at_rsp());
    __ addptr(rsp, 2 * Interpreter::stackElementSize);
    break;
  case div:
    __ movdbl(xmm1, xmm0);
    __ pop_d(xmm0);
    __ divsd(xmm0, xmm1);
    break;
  case rem:
    // Similar to fop2(), the modulo operation is performed using the
    // SharedRuntime::drem method on x86_64 platforms for the same reasons
    // as mentioned in fop2().
    __ movdbl(xmm1, xmm0);
    __ pop_d(xmm0);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::drem), 2);
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

void TemplateTable::ineg() {
  transition(itos, itos);
  __ negl(rax);
}

void TemplateTable::lneg() {
  transition(ltos, ltos);
  __ negq(rax);
}

// Note: 'double' and 'long long' have 32-bits alignment on x86.
static jlong* double_quadword(jlong *adr, jlong lo, jlong hi) {
  // Use the expression (adr)&(~0xF) to provide 128-bits aligned address
  // of 128-bits operands for SSE instructions.
  jlong *operand = (jlong*)(((intptr_t)adr)&((intptr_t)(~0xF)));
  // Store the value to a 128-bits operand.
  operand[0] = lo;
  operand[1] = hi;
  return operand;
}

// Buffer for 128-bits masks used by SSE instructions.
static jlong float_signflip_pool[2*2];
static jlong double_signflip_pool[2*2];

void TemplateTable::fneg() {
  transition(ftos, ftos);
  static jlong *float_signflip  = double_quadword(&float_signflip_pool[1],  CONST64(0x8000000080000000),  CONST64(0x8000000080000000));
  __ xorps(xmm0, ExternalAddress((address) float_signflip), rscratch1);
}

void TemplateTable::dneg() {
  transition(dtos, dtos);
  static jlong *double_signflip =
    double_quadword(&double_signflip_pool[1], CONST64(0x8000000000000000), CONST64(0x8000000000000000));
  __ xorpd(xmm0, ExternalAddress((address) double_signflip), rscratch1);
}

void TemplateTable::iinc() {
  transition(vtos, vtos);
  __ load_signed_byte(rdx, at_bcp(2)); // get constant
  locals_index(rbx);
  __ addl(iaddress(rbx), rdx);
}

void TemplateTable::wide_iinc() {
  transition(vtos, vtos);
  __ movl(rdx, at_bcp(4)); // get constant
  locals_index_wide(rbx);
  __ bswapl(rdx); // swap bytes & sign-extend constant
  __ sarl(rdx, 16);
  __ addl(iaddress(rbx), rdx);
  // Note: should probably use only one movl to get both
  //       the index and the constant -> fix this
}

void TemplateTable::convert() {
  // Checking
#ifdef ASSERT
  {
    TosState tos_in  = ilgl;
    TosState tos_out = ilgl;
    switch (bytecode()) {
    case Bytecodes::_i2l: // fall through
    case Bytecodes::_i2f: // fall through
    case Bytecodes::_i2d: // fall through
    case Bytecodes::_i2b: // fall through
    case Bytecodes::_i2c: // fall through
    case Bytecodes::_i2s: tos_in = itos; break;
    case Bytecodes::_l2i: // fall through
    case Bytecodes::_l2f: // fall through
    case Bytecodes::_l2d: tos_in = ltos; break;
    case Bytecodes::_f2i: // fall through
    case Bytecodes::_f2l: // fall through
    case Bytecodes::_f2d: tos_in = ftos; break;
    case Bytecodes::_d2i: // fall through
    case Bytecodes::_d2l: // fall through
    case Bytecodes::_d2f: tos_in = dtos; break;
    default             : ShouldNotReachHere();
    }
    switch (bytecode()) {
    case Bytecodes::_l2i: // fall through
    case Bytecodes::_f2i: // fall through
    case Bytecodes::_d2i: // fall through
    case Bytecodes::_i2b: // fall through
    case Bytecodes::_i2c: // fall through
    case Bytecodes::_i2s: tos_out = itos; break;
    case Bytecodes::_i2l: // fall through
    case Bytecodes::_f2l: // fall through
    case Bytecodes::_d2l: tos_out = ltos; break;
    case Bytecodes::_i2f: // fall through
    case Bytecodes::_l2f: // fall through
    case Bytecodes::_d2f: tos_out = ftos; break;
    case Bytecodes::_i2d: // fall through
    case Bytecodes::_l2d: // fall through
    case Bytecodes::_f2d: tos_out = dtos; break;
    default             : ShouldNotReachHere();
    }
    transition(tos_in, tos_out);
  }
#endif // ASSERT

  static const int64_t is_nan = 0x8000000000000000L;

  // Conversion
  switch (bytecode()) {
  case Bytecodes::_i2l:
    __ movslq(rax, rax);
    break;
  case Bytecodes::_i2f:
    __ cvtsi2ssl(xmm0, rax);
    break;
  case Bytecodes::_i2d:
    __ cvtsi2sdl(xmm0, rax);
    break;
  case Bytecodes::_i2b:
    __ movsbl(rax, rax);
    break;
  case Bytecodes::_i2c:
    __ movzwl(rax, rax);
    break;
  case Bytecodes::_i2s:
    __ movswl(rax, rax);
    break;
  case Bytecodes::_l2i:
    __ movl(rax, rax);
    break;
  case Bytecodes::_l2f:
    __ cvtsi2ssq(xmm0, rax);
    break;
  case Bytecodes::_l2d:
    __ cvtsi2sdq(xmm0, rax);
    break;
  case Bytecodes::_f2i:
  {
    Label L;
    __ cvttss2sil(rax, xmm0);
    __ cmpl(rax, 0x80000000); // NaN or overflow/underflow?
    __ jcc(Assembler::notEqual, L);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::f2i), 1);
    __ bind(L);
  }
    break;
  case Bytecodes::_f2l:
  {
    Label L;
    __ cvttss2siq(rax, xmm0);
    // NaN or overflow/underflow?
    __ cmp64(rax, ExternalAddress((address) &is_nan), rscratch1);
    __ jcc(Assembler::notEqual, L);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::f2l), 1);
    __ bind(L);
  }
    break;
  case Bytecodes::_f2d:
    __ cvtss2sd(xmm0, xmm0);
    break;
  case Bytecodes::_d2i:
  {
    Label L;
    __ cvttsd2sil(rax, xmm0);
    __ cmpl(rax, 0x80000000); // NaN or overflow/underflow?
    __ jcc(Assembler::notEqual, L);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::d2i), 1);
    __ bind(L);
  }
    break;
  case Bytecodes::_d2l:
  {
    Label L;
    __ cvttsd2siq(rax, xmm0);
    // NaN or overflow/underflow?
    __ cmp64(rax, ExternalAddress((address) &is_nan), rscratch1);
    __ jcc(Assembler::notEqual, L);
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::d2l), 1);
    __ bind(L);
  }
    break;
  case Bytecodes::_d2f:
    __ cvtsd2ss(xmm0, xmm0);
    break;
  default:
    ShouldNotReachHere();
  }
}

void TemplateTable::lcmp() {
  transition(ltos, itos);
  Label done;
  __ pop_l(rdx);
  __ cmpq(rdx, rax);
  __ movl(rax, -1);
  __ jccb(Assembler::less, done);
  __ setb(Assembler::notEqual, rax);
  __ movzbl(rax, rax);
  __ bind(done);
}

void TemplateTable::float_cmp(bool is_float, int unordered_result) {
  Label done;
  if (is_float) {
    // XXX get rid of pop here, use ... reg, mem32
    __ pop_f(xmm1);
    __ ucomiss(xmm1, xmm0);
  } else {
    // XXX get rid of pop here, use ... reg, mem64
    __ pop_d(xmm1);
    __ ucomisd(xmm1, xmm0);
  }
  if (unordered_result < 0) {
    __ movl(rax, -1);
    __ jccb(Assembler::parity, done);
    __ jccb(Assembler::below, done);
    __ setb(Assembler::notEqual, rdx);
    __ movzbl(rax, rdx);
  } else {
    __ movl(rax, 1);
    __ jccb(Assembler::parity, done);
    __ jccb(Assembler::above, done);
    __ movl(rax, 0);
    __ jccb(Assembler::equal, done);
    __ decrementl(rax);
  }
  __ bind(done);
}

void TemplateTable::branch(bool is_jsr, bool is_wide) {
  __ get_method(rcx); // rcx holds method
  __ profile_taken_branch(rax); // rax holds updated MDP

  const ByteSize be_offset = MethodCounters::backedge_counter_offset() +
                             InvocationCounter::counter_offset();
  const ByteSize inv_offset = MethodCounters::invocation_counter_offset() +
                              InvocationCounter::counter_offset();

  // Load up edx with the branch displacement
  if (is_wide) {
    __ movl(rdx, at_bcp(1));
  } else {
    __ load_signed_short(rdx, at_bcp(1));
  }
  __ bswapl(rdx);

  if (!is_wide) {
    __ sarl(rdx, 16);
  }
  __ movl2ptr(rdx, rdx);

  // Handle all the JSR stuff here, then exit.
  // It's much shorter and cleaner than intermingling with the non-JSR
  // normal-branch stuff occurring below.
  if (is_jsr) {
    // Pre-load the next target bytecode into rbx
    __ load_unsigned_byte(rbx, Address(rbcp, rdx, Address::times_1, 0));

    // compute return address as bci in rax
    __ lea(rax, at_bcp((is_wide ? 5 : 3) -
                        in_bytes(ConstMethod::codes_offset())));
    __ subptr(rax, Address(rcx, Method::const_offset()));
    // Adjust the bcp in r13 by the displacement in rdx
    __ addptr(rbcp, rdx);
    // jsr returns atos that is not an oop
    __ push_i(rax);
    __ dispatch_only(vtos, true);
    return;
  }

  // Normal (non-jsr) branch handling

  // Adjust the bcp in r13 by the displacement in rdx
  __ addptr(rbcp, rdx);

  assert(UseLoopCounter || !UseOnStackReplacement,
         "on-stack-replacement requires loop counters");
  Label backedge_counter_overflow;
  Label dispatch;
  if (UseLoopCounter) {
    // increment backedge counter for backward branches
    // rax: MDO
    // rcx: method
    // rdx: target offset
    // r13: target bcp
    // r14: locals pointer
    __ testl(rdx, rdx);             // check if forward or backward branch
    __ jcc(Assembler::positive, dispatch); // count only if backward branch

    // check if MethodCounters exists
    Label has_counters;
    __ movptr(rax, Address(rcx, Method::method_counters_offset()));
    __ testptr(rax, rax);
    __ jcc(Assembler::notZero, has_counters);
    __ push(rdx);
    __ push(rcx);
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::build_method_counters),
               rcx);
    __ pop(rcx);
    __ pop(rdx);
    __ movptr(rax, Address(rcx, Method::method_counters_offset()));
    __ testptr(rax, rax);
    __ jcc(Assembler::zero, dispatch);
    __ bind(has_counters);

    Label no_mdo;
    if (ProfileInterpreter) {
      // Are we profiling?
      __ movptr(rbx, Address(rcx, in_bytes(Method::method_data_offset())));
      __ testptr(rbx, rbx);
      __ jccb(Assembler::zero, no_mdo);
      // Increment the MDO backedge counter
      const Address mdo_backedge_counter(rbx, in_bytes(MethodData::backedge_counter_offset()) +
          in_bytes(InvocationCounter::counter_offset()));
      const Address mask(rbx, in_bytes(MethodData::backedge_mask_offset()));
      __ increment_mask_and_jump(mdo_backedge_counter, mask, rax,
          UseOnStackReplacement ? &backedge_counter_overflow : nullptr);
      __ jmp(dispatch);
    }
    __ bind(no_mdo);
    // Increment backedge counter in MethodCounters*
    __ movptr(rcx, Address(rcx, Method::method_counters_offset()));
    const Address mask(rcx, in_bytes(MethodCounters::backedge_mask_offset()));
    __ increment_mask_and_jump(Address(rcx, be_offset), mask, rax,
        UseOnStackReplacement ? &backedge_counter_overflow : nullptr);
    __ bind(dispatch);
  }

  // Pre-load the next target bytecode into rbx
  __ load_unsigned_byte(rbx, Address(rbcp, 0));

  // continue with the bytecode @ target
  // rax: return bci for jsr's, unused otherwise
  // rbx: target bytecode
  // r13: target bcp
  __ dispatch_only(vtos, true);

  if (UseLoopCounter) {
    if (UseOnStackReplacement) {
      Label set_mdp;
      // invocation counter overflow
      __ bind(backedge_counter_overflow);
      __ negptr(rdx);
      __ addptr(rdx, rbcp); // branch bcp
      // IcoResult frequency_counter_overflow([JavaThread*], address branch_bcp)
      __ call_VM(noreg,
                 CAST_FROM_FN_PTR(address,
                                  InterpreterRuntime::frequency_counter_overflow),
                 rdx);

      // rax: osr nmethod (osr ok) or null (osr not possible)
      // rdx: scratch
      // r14: locals pointer
      // r13: bcp
      __ testptr(rax, rax);                        // test result
      __ jcc(Assembler::zero, dispatch);         // no osr if null
      // nmethod may have been invalidated (VM may block upon call_VM return)
      __ cmpb(Address(rax, nmethod::state_offset()), nmethod::in_use);
      __ jcc(Assembler::notEqual, dispatch);

      // We have the address of an on stack replacement routine in rax.
      // In preparation of invoking it, first we must migrate the locals
      // and monitors from off the interpreter frame on the stack.
      // Ensure to save the osr nmethod over the migration call,
      // it will be preserved in rbx.
      __ mov(rbx, rax);

      JFR_ONLY(__ enter_jfr_critical_section();)

      call_VM(noreg, CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_begin));

      // rax is OSR buffer, move it to expected parameter location
      __ mov(j_rarg0, rax);
      // We use j_rarg definitions here so that registers don't conflict as parameter
      // registers change across platforms as we are in the midst of a calling
      // sequence to the OSR nmethod and we don't want collision. These are NOT parameters.

      const Register retaddr   = j_rarg2;
      const Register sender_sp = j_rarg1;

      // pop the interpreter frame
      __ movptr(sender_sp, Address(rbp, frame::interpreter_frame_sender_sp_offset * wordSize)); // get sender sp
      __ leave();                                // remove frame anchor
      JFR_ONLY(__ leave_jfr_critical_section();)
      __ pop(retaddr);                           // get return address
      __ mov(rsp, sender_sp);                    // set sp to sender sp
      // Ensure compiled code always sees stack at proper alignment
      __ andptr(rsp, -(StackAlignmentInBytes));

      // push the return address
      __ push(retaddr);

      // and begin the OSR nmethod
      __ jmp(Address(rbx, nmethod::osr_entry_point_offset()));
    }
  }
}

void TemplateTable::if_0cmp(Condition cc) {
  transition(itos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  __ testl(rax, rax);
  __ jcc(j_not(cc), not_taken);
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(rax);
}

void TemplateTable::if_icmp(Condition cc) {
  transition(itos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  __ pop_i(rdx);
  __ cmpl(rdx, rax);
  __ jcc(j_not(cc), not_taken);
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(rax);
}

void TemplateTable::if_nullcmp(Condition cc) {
  transition(atos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  __ testptr(rax, rax);
  __ jcc(j_not(cc), not_taken);
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(rax);
}

void TemplateTable::if_acmp(Condition cc) {
  transition(atos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  __ pop_ptr(rdx);
  __ cmpoop(rdx, rax);
  __ jcc(j_not(cc), not_taken);
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(rax);
}

void TemplateTable::ret() {
  transition(vtos, vtos);
  locals_index(rbx);
  __ movslq(rbx, iaddress(rbx)); // get return bci, compute return bcp
  __ profile_ret(rbx, rcx);
  __ get_method(rax);
  __ movptr(rbcp, Address(rax, Method::const_offset()));
  __ lea(rbcp, Address(rbcp, rbx, Address::times_1,
                      ConstMethod::codes_offset()));
  __ dispatch_next(vtos, 0, true);
}

void TemplateTable::wide_ret() {
  transition(vtos, vtos);
  locals_index_wide(rbx);
  __ movptr(rbx, aaddress(rbx)); // get return bci, compute return bcp
  __ profile_ret(rbx, rcx);
  __ get_method(rax);
  __ movptr(rbcp, Address(rax, Method::const_offset()));
  __ lea(rbcp, Address(rbcp, rbx, Address::times_1, ConstMethod::codes_offset()));
  __ dispatch_next(vtos, 0, true);
}

void TemplateTable::tableswitch() {
  Label default_case, continue_execution;
  transition(itos, vtos);

  // align r13/rsi
  __ lea(rbx, at_bcp(BytesPerInt));
  __ andptr(rbx, -BytesPerInt);
  // load lo & hi
  __ movl(rcx, Address(rbx, BytesPerInt));
  __ movl(rdx, Address(rbx, 2 * BytesPerInt));
  __ bswapl(rcx);
  __ bswapl(rdx);
  // check against lo & hi
  __ cmpl(rax, rcx);
  __ jcc(Assembler::less, default_case);
  __ cmpl(rax, rdx);
  __ jcc(Assembler::greater, default_case);
  // lookup dispatch offset
  __ subl(rax, rcx);
  __ movl(rdx, Address(rbx, rax, Address::times_4, 3 * BytesPerInt));
  __ profile_switch_case(rax, rbx, rcx);
  // continue execution
  __ bind(continue_execution);
  __ bswapl(rdx);
  __ movl2ptr(rdx, rdx);
  __ load_unsigned_byte(rbx, Address(rbcp, rdx, Address::times_1));
  __ addptr(rbcp, rdx);
  __ dispatch_only(vtos, true);
  // handle default
  __ bind(default_case);
  __ profile_switch_default(rax);
  __ movl(rdx, Address(rbx, 0));
  __ jmp(continue_execution);
}

void TemplateTable::lookupswitch() {
  transition(itos, itos);
  __ stop("lookupswitch bytecode should have been rewritten");
}

void TemplateTable::fast_linearswitch() {
  transition(itos, vtos);
  Label loop_entry, loop, found, continue_execution;
  // bswap rax so we can avoid bswapping the table entries
  __ bswapl(rax);
  // align r13
  __ lea(rbx, at_bcp(BytesPerInt)); // btw: should be able to get rid of
                                    // this instruction (change offsets
                                    // below)
  __ andptr(rbx, -BytesPerInt);
  // set counter
  __ movl(rcx, Address(rbx, BytesPerInt));
  __ bswapl(rcx);
  __ jmpb(loop_entry);
  // table search
  __ bind(loop);
  __ cmpl(rax, Address(rbx, rcx, Address::times_8, 2 * BytesPerInt));
  __ jcc(Assembler::equal, found);
  __ bind(loop_entry);
  __ decrementl(rcx);
  __ jcc(Assembler::greaterEqual, loop);
  // default case
  __ profile_switch_default(rax);
  __ movl(rdx, Address(rbx, 0));
  __ jmp(continue_execution);
  // entry found -> get offset
  __ bind(found);
  __ movl(rdx, Address(rbx, rcx, Address::times_8, 3 * BytesPerInt));
  __ profile_switch_case(rcx, rax, rbx);
  // continue execution
  __ bind(continue_execution);
  __ bswapl(rdx);
  __ movl2ptr(rdx, rdx);
  __ load_unsigned_byte(rbx, Address(rbcp, rdx, Address::times_1));
  __ addptr(rbcp, rdx);
  __ dispatch_only(vtos, true);
}

void TemplateTable::fast_binaryswitch() {
  transition(itos, vtos);
  // Implementation using the following core algorithm:
  //
  // int binary_search(int key, LookupswitchPair* array, int n) {
  //   // Binary search according to "Methodik des Programmierens" by
  //   // Edsger W. Dijkstra and W.H.J. Feijen, Addison Wesley Germany 1985.
  //   int i = 0;
  //   int j = n;
  //   while (i+1 < j) {
  //     // invariant P: 0 <= i < j <= n and (a[i] <= key < a[j] or Q)
  //     // with      Q: for all i: 0 <= i < n: key < a[i]
  //     // where a stands for the array and assuming that the (inexisting)
  //     // element a[n] is infinitely big.
  //     int h = (i + j) >> 1;
  //     // i < h < j
  //     if (key < array[h].fast_match()) {
  //       j = h;
  //     } else {
  //       i = h;
  //     }
  //   }
  //   // R: a[i] <= key < a[i+1] or Q
  //   // (i.e., if key is within array, i is the correct index)
  //   return i;
  // }

  // Register allocation
  const Register key   = rax; // already set (tosca)
  const Register array = rbx;
  const Register i     = rcx;
  const Register j     = rdx;
  const Register h     = rdi;
  const Register temp  = rsi;

  // Find array start
  __ lea(array, at_bcp(3 * BytesPerInt)); // btw: should be able to
                                          // get rid of this
                                          // instruction (change
                                          // offsets below)
  __ andptr(array, -BytesPerInt);

  // Initialize i & j
  __ xorl(i, i);                            // i = 0;
  __ movl(j, Address(array, -BytesPerInt)); // j = length(array);

  // Convert j into native byteordering
  __ bswapl(j);

  // And start
  Label entry;
  __ jmp(entry);

  // binary search loop
  {
    Label loop;
    __ bind(loop);
    // int h = (i + j) >> 1;
    __ leal(h, Address(i, j, Address::times_1)); // h = i + j;
    __ sarl(h, 1);                               // h = (i + j) >> 1;
    // if (key < array[h].fast_match()) {
    //   j = h;
    // } else {
    //   i = h;
    // }
    // Convert array[h].match to native byte-ordering before compare
    __ movl(temp, Address(array, h, Address::times_8));
    __ bswapl(temp);
    __ cmpl(key, temp);
    // j = h if (key <  array[h].fast_match())
    __ cmov32(Assembler::less, j, h);
    // i = h if (key >= array[h].fast_match())
    __ cmov32(Assembler::greaterEqual, i, h);
    // while (i+1 < j)
    __ bind(entry);
    __ leal(h, Address(i, 1)); // i+1
    __ cmpl(h, j);             // i+1 < j
    __ jcc(Assembler::less, loop);
  }

  // end of binary search, result index is i (must check again!)
  Label default_case;
  // Convert array[i].match to native byte-ordering before compare
  __ movl(temp, Address(array, i, Address::times_8));
  __ bswapl(temp);
  __ cmpl(key, temp);
  __ jcc(Assembler::notEqual, default_case);

  // entry found -> j = offset
  __ movl(j , Address(array, i, Address::times_8, BytesPerInt));
  __ profile_switch_case(i, key, array);
  __ bswapl(j);
  __ movslq(j, j);

  __ load_unsigned_byte(rbx, Address(rbcp, j, Address::times_1));
  __ addptr(rbcp, j);
  __ dispatch_only(vtos, true);

  // default case -> j = default offset
  __ bind(default_case);
  __ profile_switch_default(i);
  __ movl(j, Address(array, -2 * BytesPerInt));
  __ bswapl(j);
  __ movslq(j, j);

  __ load_unsigned_byte(rbx, Address(rbcp, j, Address::times_1));
  __ addptr(rbcp, j);
  __ dispatch_only(vtos, true);
}

void TemplateTable::_return(TosState state) {
  transition(state, state);

  assert(_desc->calls_vm(),
         "inconsistent calls_vm information"); // call in remove_activation

  if (_desc->bytecode() == Bytecodes::_return_register_finalizer) {
    assert(state == vtos, "only valid state");
    Register robj = c_rarg1;
    __ movptr(robj, aaddress(0));
    __ load_klass(rdi, robj, rscratch1);
    __ testb(Address(rdi, Klass::misc_flags_offset()), KlassFlags::_misc_has_finalizer);
    Label skip_register_finalizer;
    __ jcc(Assembler::zero, skip_register_finalizer);

    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::register_finalizer), robj);

    __ bind(skip_register_finalizer);
  }

  if (_desc->bytecode() != Bytecodes::_return_register_finalizer) {
    Label no_safepoint;
    NOT_PRODUCT(__ block_comment("Thread-local Safepoint poll"));
    __ testb(Address(r15_thread, JavaThread::polling_word_offset()), SafepointMechanism::poll_bit());
    __ jcc(Assembler::zero, no_safepoint);
    __ push(state);
    __ push_cont_fastpath();
    __ call_VM(noreg, CAST_FROM_FN_PTR(address,
                                       InterpreterRuntime::at_safepoint));
    __ pop_cont_fastpath();
    __ pop(state);
    __ bind(no_safepoint);
  }

  // Narrow result if state is itos but result type is smaller.
  // Need to narrow in the return bytecode rather than in generate_return_entry
  // since compiled code callers expect the result to already be narrowed.
  if (state == itos) {
    __ narrow(rax);
  }
  __ remove_activation(state, rbcp);

  __ jmp(rbcp);
}

// ----------------------------------------------------------------------------
// Volatile variables demand their effects be made known to all CPU's
// in order.  Store buffers on most chips allow reads & writes to
// reorder; the JMM's ReadAfterWrite.java test fails in -Xint mode
// without some kind of memory barrier (i.e., it's not sufficient that
// the interpreter does not reorder volatile references, the hardware
// also must not reorder them).
//
// According to the new Java Memory Model (JMM):
// (1) All volatiles are serialized wrt to each other.  ALSO reads &
//     writes act as acquire & release, so:
// (2) A read cannot let unrelated NON-volatile memory refs that
//     happen after the read float up to before the read.  It's OK for
//     non-volatile memory refs that happen before the volatile read to
//     float down below it.
// (3) Similar a volatile write cannot let unrelated NON-volatile
//     memory refs that happen BEFORE the write float down to after the
//     write.  It's OK for non-volatile memory refs that happen after the
//     volatile write to float up before it.
//
// We only put in barriers around volatile refs (they are expensive),
// not _between_ memory refs (that would require us to track the
// flavor of the previous memory refs).  Requirements (2) and (3)
// require some barriers before volatile stores and after volatile
// loads.  These nearly cover requirement (1) but miss the
// volatile-store-volatile-load case.  This final case is placed after
// volatile-stores although it could just as well go before
// volatile-loads.

void TemplateTable::volatile_barrier(Assembler::Membar_mask_bits order_constraint ) {
  // Helper function to insert a is-volatile test and memory barrier
  __ membar(order_constraint);
}

void TemplateTable::resolve_cache_and_index_for_method(int byte_no,
                                                       Register cache,
                                                       Register index) {
  const Register temp = rbx;
  assert_different_registers(cache, index, temp);

  Label L_clinit_barrier_slow;
  Label resolved;

  Bytecodes::Code code = bytecode();

  assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");

  __ load_method_entry(cache, index);
  switch(byte_no) {
    case f1_byte:
      __ load_unsigned_byte(temp, Address(cache, in_bytes(ResolvedMethodEntry::bytecode1_offset())));
      break;
    case f2_byte:
      __ load_unsigned_byte(temp, Address(cache, in_bytes(ResolvedMethodEntry::bytecode2_offset())));
      break;
    default:
      ShouldNotReachHere();
  }
  __ cmpl(temp, code);  // have we resolved this bytecode?
  __ jcc(Assembler::equal, resolved);

  // resolve first time through
  // Class initialization barrier slow path lands here as well.
  __ bind(L_clinit_barrier_slow);
  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_from_cache);
  __ movl(temp, code);
  __ call_VM(noreg, entry, temp);
  // Update registers with resolved info
  __ load_method_entry(cache, index);

  __ bind(resolved);

  // Class initialization barrier for static methods
  if (VM_Version::supports_fast_class_init_checks() && bytecode() == Bytecodes::_invokestatic) {
    const Register method = temp;
    const Register klass  = temp;

    __ movptr(method, Address(cache, in_bytes(ResolvedMethodEntry::method_offset())));
    __ load_method_holder(klass, method);
    __ clinit_barrier(klass, nullptr /*L_fast_path*/, &L_clinit_barrier_slow);
  }
}

void TemplateTable::resolve_cache_and_index_for_field(int byte_no,
                                            Register cache,
                                            Register index) {
  const Register temp = rbx;
  assert_different_registers(cache, index, temp);

  Label resolved;

  Bytecodes::Code code = bytecode();
  switch (code) {
    case Bytecodes::_nofast_getfield: code = Bytecodes::_getfield; break;
    case Bytecodes::_nofast_putfield: code = Bytecodes::_putfield; break;
    default: break;
  }

  assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
  __ load_field_entry(cache, index);
  if (byte_no == f1_byte) {
    __ load_unsigned_byte(temp, Address(cache, in_bytes(ResolvedFieldEntry::get_code_offset())));
  } else {
    __ load_unsigned_byte(temp, Address(cache, in_bytes(ResolvedFieldEntry::put_code_offset())));
  }
  __ cmpl(temp, code);  // have we resolved this bytecode?
  __ jcc(Assembler::equal, resolved);

  // resolve first time through
  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_from_cache);
  __ movl(temp, code);
  __ call_VM(noreg, entry, temp);
  // Update registers with resolved info
  __ load_field_entry(cache, index);

  __ bind(resolved);
}

void TemplateTable::load_resolved_field_entry(Register obj,
                                              Register cache,
                                              Register tos_state,
                                              Register offset,
                                              Register flags,
                                              bool is_static = false) {
  assert_different_registers(cache, tos_state, flags, offset);

  // Field offset
  __ load_sized_value(offset, Address(cache, in_bytes(ResolvedFieldEntry::field_offset_offset())), sizeof(int), true /*is_signed*/);

  // Flags
  __ load_unsigned_byte(flags, Address(cache, in_bytes(ResolvedFieldEntry::flags_offset())));

  // TOS state
  __ load_unsigned_byte(tos_state, Address(cache, in_bytes(ResolvedFieldEntry::type_offset())));

  // Klass overwrite register
  if (is_static) {
    __ movptr(obj, Address(cache, ResolvedFieldEntry::field_holder_offset()));
    const int mirror_offset = in_bytes(Klass::java_mirror_offset());
    __ movptr(obj, Address(obj, mirror_offset));
    __ resolve_oop_handle(obj, rscratch2);
  }

}

void TemplateTable::load_invokedynamic_entry(Register method) {
  // setup registers
  const Register appendix = rax;
  const Register cache = rcx;
  const Register index = rdx;
  assert_different_registers(method, appendix, cache, index);

  __ save_bcp();

  Label resolved;

  __ load_resolved_indy_entry(cache, index);
  __ movptr(method, Address(cache, in_bytes(ResolvedIndyEntry::method_offset())));

  // Compare the method to zero
  __ testptr(method, method);
  __ jcc(Assembler::notZero, resolved);

  Bytecodes::Code code = bytecode();

  // Call to the interpreter runtime to resolve invokedynamic
  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_from_cache);
  __ movl(method, code); // this is essentially Bytecodes::_invokedynamic
  __ call_VM(noreg, entry, method);
  // Update registers with resolved info
  __ load_resolved_indy_entry(cache, index);
  __ movptr(method, Address(cache, in_bytes(ResolvedIndyEntry::method_offset())));

#ifdef ASSERT
  __ testptr(method, method);
  __ jcc(Assembler::notZero, resolved);
  __ stop("Should be resolved by now");
#endif // ASSERT
  __ bind(resolved);

  Label L_no_push;
  // Check if there is an appendix
  __ load_unsigned_byte(index, Address(cache, in_bytes(ResolvedIndyEntry::flags_offset())));
  __ testl(index, (1 << ResolvedIndyEntry::has_appendix_shift));
  __ jcc(Assembler::zero, L_no_push);

  // Get appendix
  __ load_unsigned_short(index, Address(cache, in_bytes(ResolvedIndyEntry::resolved_references_index_offset())));
  // Push the appendix as a trailing parameter
  // since the parameter_size includes it.
  __ load_resolved_reference_at_index(appendix, index);
  __ verify_oop(appendix);
  __ push(appendix);  // push appendix (MethodType, CallSite, etc.)
  __ bind(L_no_push);

  // compute return type
  __ load_unsigned_byte(index, Address(cache, in_bytes(ResolvedIndyEntry::result_type_offset())));
  // load return address
  {
    const address table_addr = (address) Interpreter::invoke_return_entry_table_for(code);
    ExternalAddress table(table_addr);
    __ lea(rscratch1, table);
    __ movptr(index, Address(rscratch1, index, Address::times_ptr));
  }

  // push return address
  __ push(index);
}

void TemplateTable::load_resolved_method_entry_special_or_static(Register cache,
                                                                 Register method,
                                                                 Register flags) {
  // setup registers
  const Register index = rdx;
  assert_different_registers(cache, index);
  assert_different_registers(method, cache, flags);

  // determine constant pool cache field offsets
  resolve_cache_and_index_for_method(f1_byte, cache, index);
  __ load_unsigned_byte(flags, Address(cache, in_bytes(ResolvedMethodEntry::flags_offset())));
  __ movptr(method, Address(cache, in_bytes(ResolvedMethodEntry::method_offset())));
}

void TemplateTable::load_resolved_method_entry_handle(Register cache,
                                               Register method,
                                               Register ref_index,
                                               Register flags) {
  // setup registers
  const Register index = rdx;
  assert_different_registers(cache, index);
  assert_different_registers(cache, method, ref_index, flags);

  // determine constant pool cache field offsets
  resolve_cache_and_index_for_method(f1_byte, cache, index);
  __ load_unsigned_byte(flags, Address(cache, in_bytes(ResolvedMethodEntry::flags_offset())));

  // Maybe push appendix
  Label L_no_push;
  __ testl(flags, (1 << ResolvedMethodEntry::has_appendix_shift));
  __ jcc(Assembler::zero, L_no_push);
  // invokehandle uses an index into the resolved references array
  __ load_unsigned_short(ref_index, Address(cache, in_bytes(ResolvedMethodEntry::resolved_references_index_offset())));
  // Push the appendix as a trailing parameter.
  // This must be done before we get the receiver,
  // since the parameter_size includes it.
  Register appendix = method;
  __ load_resolved_reference_at_index(appendix, ref_index);
  __ push(appendix);  // push appendix (MethodType, CallSite, etc.)
  __ bind(L_no_push);

  __ movptr(method, Address(cache, in_bytes(ResolvedMethodEntry::method_offset())));
}

void TemplateTable::load_resolved_method_entry_interface(Register cache,
                                                         Register klass,
                                                         Register method_or_table_index,
                                                         Register flags) {
  // setup registers
  const Register index = rdx;
  assert_different_registers(cache, klass, method_or_table_index, flags);

  // determine constant pool cache field offsets
  resolve_cache_and_index_for_method(f1_byte, cache, index);
  __ load_unsigned_byte(flags, Address(cache, in_bytes(ResolvedMethodEntry::flags_offset())));

  // Invokeinterface can behave in different ways:
  // If calling a method from java.lang.Object, the forced virtual flag is true so the invocation will
  // behave like an invokevirtual call. The state of the virtual final flag will determine whether a method or
  // vtable index is placed in the register.
  // Otherwise, the registers will be populated with the klass and method.

  Label NotVirtual; Label NotVFinal; Label Done;
  __ testl(flags, 1 << ResolvedMethodEntry::is_forced_virtual_shift);
  __ jcc(Assembler::zero, NotVirtual);
  __ testl(flags, (1 << ResolvedMethodEntry::is_vfinal_shift));
  __ jcc(Assembler::zero, NotVFinal);
  __ movptr(method_or_table_index, Address(cache, in_bytes(ResolvedMethodEntry::method_offset())));
  __ jmp(Done);

  __ bind(NotVFinal);
  __ load_unsigned_short(method_or_table_index, Address(cache, in_bytes(ResolvedMethodEntry::table_index_offset())));
  __ jmp(Done);

  __ bind(NotVirtual);
  __ movptr(method_or_table_index, Address(cache, in_bytes(ResolvedMethodEntry::method_offset())));
  __ movptr(klass, Address(cache, in_bytes(ResolvedMethodEntry::klass_offset())));
  __ bind(Done);
}

void TemplateTable::load_resolved_method_entry_virtual(Register cache,
                                                       Register method_or_table_index,
                                                       Register flags) {
  // setup registers
  const Register index = rdx;
  assert_different_registers(index, cache);
  assert_different_registers(method_or_table_index, cache, flags);

  // determine constant pool cache field offsets
  resolve_cache_and_index_for_method(f2_byte, cache, index);
  __ load_unsigned_byte(flags, Address(cache, in_bytes(ResolvedMethodEntry::flags_offset())));

  // method_or_table_index can either be an itable index or a method depending on the virtual final flag
  Label isVFinal; Label Done;
  __ testl(flags, (1 << ResolvedMethodEntry::is_vfinal_shift));
  __ jcc(Assembler::notZero, isVFinal);
  __ load_unsigned_short(method_or_table_index, Address(cache, in_bytes(ResolvedMethodEntry::table_index_offset())));
  __ jmp(Done);
  __ bind(isVFinal);
  __ movptr(method_or_table_index, Address(cache, in_bytes(ResolvedMethodEntry::method_offset())));
  __ bind(Done);
}

// The registers cache and index expected to be set before call.
// Correct values of the cache and index registers are preserved.
void TemplateTable::jvmti_post_field_access(Register cache,
                                            Register index,
                                            bool is_static,
                                            bool has_tos) {
  if (JvmtiExport::can_post_field_access()) {
    // Check to see if a field access watch has been set before we take
    // the time to call into the VM.
    Label L1;
    assert_different_registers(cache, index, rax);
    __ mov32(rax, ExternalAddress((address) JvmtiExport::get_field_access_count_addr()));
    __ testl(rax,rax);
    __ jcc(Assembler::zero, L1);

    // cache entry pointer
    __ load_field_entry(cache, index);
    if (is_static) {
      __ xorptr(rax, rax);      // null object reference
    } else {
      __ pop(atos);         // Get the object
      __ verify_oop(rax);
      __ push(atos);        // Restore stack state
    }
    // rax,:   object pointer or null
    // cache: cache entry pointer
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::post_field_access),
              rax, cache);

    __ load_field_entry(cache, index);
    __ bind(L1);
  }
}

void TemplateTable::pop_and_check_object(Register r) {
  __ pop_ptr(r);
  __ null_check(r);  // for field access must check obj.
  __ verify_oop(r);
}

void TemplateTable::getfield_or_static(int byte_no, bool is_static, RewriteControl rc) {
  transition(vtos, vtos);

  const Register obj   = c_rarg3;
  const Register cache = rcx;
  const Register index = rdx;
  const Register off   = rbx;
  const Register tos_state   = rax;
  const Register flags = rdx;
  const Register bc    = c_rarg3; // uses same reg as obj, so don't mix them

  resolve_cache_and_index_for_field(byte_no, cache, index);
  jvmti_post_field_access(cache, index, is_static, false);
  load_resolved_field_entry(obj, cache, tos_state, off, flags, is_static);

  if (!is_static) pop_and_check_object(obj);

  const Address field(obj, off, Address::times_1, 0*wordSize);

  Label Done, notByte, notBool, notInt, notShort, notChar, notLong, notFloat, notObj;

  // Make sure we don't need to mask edx after the above shift
  assert(btos == 0, "change code, btos != 0");
  __ testl(tos_state, tos_state);
  __ jcc(Assembler::notZero, notByte);

  // btos
  __ access_load_at(T_BYTE, IN_HEAP, rax, field, noreg);
  __ push(btos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_bgetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notByte);
  __ cmpl(tos_state, ztos);
  __ jcc(Assembler::notEqual, notBool);

  // ztos (same code as btos)
  __ access_load_at(T_BOOLEAN, IN_HEAP, rax, field, noreg);
  __ push(ztos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    // use btos rewriting, no truncating to t/f bit is needed for getfield.
    patch_bytecode(Bytecodes::_fast_bgetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notBool);
  __ cmpl(tos_state, atos);
  __ jcc(Assembler::notEqual, notObj);
  // atos
  do_oop_load(_masm, field, rax);
  __ push(atos);
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_agetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notObj);
  __ cmpl(tos_state, itos);
  __ jcc(Assembler::notEqual, notInt);
  // itos
  __ access_load_at(T_INT, IN_HEAP, rax, field, noreg);
  __ push(itos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_igetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notInt);
  __ cmpl(tos_state, ctos);
  __ jcc(Assembler::notEqual, notChar);
  // ctos
  __ access_load_at(T_CHAR, IN_HEAP, rax, field, noreg);
  __ push(ctos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_cgetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notChar);
  __ cmpl(tos_state, stos);
  __ jcc(Assembler::notEqual, notShort);
  // stos
  __ access_load_at(T_SHORT, IN_HEAP, rax, field, noreg);
  __ push(stos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_sgetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notShort);
  __ cmpl(tos_state, ltos);
  __ jcc(Assembler::notEqual, notLong);
  // ltos
    // Generate code as if volatile (x86_32).  There just aren't enough registers to
    // save that information and this code is faster than the test.
  __ access_load_at(T_LONG, IN_HEAP | MO_RELAXED, noreg /* ltos */, field, noreg);
  __ push(ltos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) patch_bytecode(Bytecodes::_fast_lgetfield, bc, rbx);
  __ jmp(Done);

  __ bind(notLong);
  __ cmpl(tos_state, ftos);
  __ jcc(Assembler::notEqual, notFloat);
  // ftos

  __ access_load_at(T_FLOAT, IN_HEAP, noreg /* ftos */, field, noreg);
  __ push(ftos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_fgetfield, bc, rbx);
  }
  __ jmp(Done);

  __ bind(notFloat);
#ifdef ASSERT
  Label notDouble;
  __ cmpl(tos_state, dtos);
  __ jcc(Assembler::notEqual, notDouble);
#endif
  // dtos
  // MO_RELAXED: for the case of volatile field, in fact it adds no extra work for the underlying implementation
  __ access_load_at(T_DOUBLE, IN_HEAP | MO_RELAXED, noreg /* dtos */, field, noreg);
  __ push(dtos);
  // Rewrite bytecode to be faster
  if (!is_static && rc == may_rewrite) {
    patch_bytecode(Bytecodes::_fast_dgetfield, bc, rbx);
  }
#ifdef ASSERT
  __ jmp(Done);

  __ bind(notDouble);
  __ stop("Bad state");
#endif

  __ bind(Done);
  // [jk] not needed currently
  // volatile_barrier(Assembler::Membar_mask_bits(Assembler::LoadLoad |
  //                                              Assembler::LoadStore));
}

void TemplateTable::getfield(int byte_no) {
  getfield_or_static(byte_no, false);
}

void TemplateTable::nofast_getfield(int byte_no) {
  getfield_or_static(byte_no, false, may_not_rewrite);
}

void TemplateTable::getstatic(int byte_no) {
  getfield_or_static(byte_no, true);
}


// The registers cache and index expected to be set before call.
// The function may destroy various registers, just not the cache and index registers.
void TemplateTable::jvmti_post_field_mod(Register cache, Register index, bool is_static) {
  // Cache is rcx and index is rdx
  const Register entry = c_rarg2; // ResolvedFieldEntry
  const Register obj = c_rarg1;   // Object pointer
  const Register value = c_rarg3; // JValue object

  if (JvmtiExport::can_post_field_modification()) {
    // Check to see if a field modification watch has been set before
    // we take the time to call into the VM.
    Label L1;
    assert_different_registers(cache, obj, rax);
    __ mov32(rax, ExternalAddress((address)JvmtiExport::get_field_modification_count_addr()));
    __ testl(rax, rax);
    __ jcc(Assembler::zero, L1);

    __ mov(entry, cache);

    if (is_static) {
      // Life is simple.  Null out the object pointer.
      __ xorl(obj, obj);

    } else {
      // Life is harder. The stack holds the value on top, followed by
      // the object.  We don't know the size of the value, though; it
      // could be one or two words depending on its type. As a result,
      // we must find the type to determine where the object is.
      __ load_unsigned_byte(value, Address(entry, in_bytes(ResolvedFieldEntry::type_offset())));
      __ movptr(obj, at_tos_p1());  // initially assume a one word jvalue
      __ cmpl(value, ltos);
      __ cmovptr(Assembler::equal,
                 obj, at_tos_p2()); // ltos (two word jvalue)
      __ cmpl(value, dtos);
      __ cmovptr(Assembler::equal,
                 obj, at_tos_p2()); // dtos (two word jvalue)
    }

    // object (tos)
    __ mov(value, rsp);
    // obj: object pointer set up above (null if static)
    // cache: field entry pointer
    // value: jvalue object on the stack
    __ call_VM(noreg,
              CAST_FROM_FN_PTR(address,
                              InterpreterRuntime::post_field_modification),
              obj, entry, value);
    // Reload field entry
    __ load_field_entry(cache, index);
    __ bind(L1);
  }
}

void TemplateTable::putfield_or_static(int byte_no, bool is_static, RewriteControl rc) {
  transition(vtos, vtos);

  const Register obj = rcx;
  const Register cache = rcx;
  const Register index = rdx;
  const Register tos_state   = rdx;
  const Register off   = rbx;
  const Register flags = rax;

  resolve_cache_and_index_for_field(byte_no, cache, index);
  jvmti_post_field_mod(cache, index, is_static);
  load_resolved_field_entry(obj, cache, tos_state, off, flags, is_static);

  // [jk] not needed currently
  // volatile_barrier(Assembler::Membar_mask_bits(Assembler::LoadStore |
  //                                              Assembler::StoreStore));

  Label notVolatile, Done;

  // Check for volatile store
  __ andl(flags, (1 << ResolvedFieldEntry::is_volatile_shift));
  __ testl(flags, flags);
  __ jcc(Assembler::zero, notVolatile);

  putfield_or_static_helper(byte_no, is_static, rc, obj, off, tos_state);
  volatile_barrier(Assembler::Membar_mask_bits(Assembler::StoreLoad |
                                               Assembler::StoreStore));
  __ jmp(Done);
  __ bind(notVolatile);

  putfield_or_static_helper(byte_no, is_static, rc, obj, off, tos_state);

  __ bind(Done);
}

void TemplateTable::putfield_or_static_helper(int byte_no, bool is_static, RewriteControl rc,
                                              Register obj, Register off, Register tos_state) {

  // field addresses
  const Address field(obj, off, Address::times_1, 0*wordSize);

  Label notByte, notBool, notInt, notShort, notChar,
        notLong, notFloat, notObj;
  Label Done;

  const Register bc    = c_rarg3;

  // Test TOS state
  __ testl(tos_state, tos_state);
  __ jcc(Assembler::notZero, notByte);

  // btos
  {
    __ pop(btos);
    if (!is_static) pop_and_check_object(obj);
    __ access_store_at(T_BYTE, IN_HEAP, field, rax, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_bputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notByte);
  __ cmpl(tos_state, ztos);
  __ jcc(Assembler::notEqual, notBool);

  // ztos
  {
    __ pop(ztos);
    if (!is_static) pop_and_check_object(obj);
    __ access_store_at(T_BOOLEAN, IN_HEAP, field, rax, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_zputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notBool);
  __ cmpl(tos_state, atos);
  __ jcc(Assembler::notEqual, notObj);

  // atos
  {
    __ pop(atos);
    if (!is_static) pop_and_check_object(obj);
    // Store into the field
    do_oop_store(_masm, field, rax);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_aputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notObj);
  __ cmpl(tos_state, itos);
  __ jcc(Assembler::notEqual, notInt);

  // itos
  {
    __ pop(itos);
    if (!is_static) pop_and_check_object(obj);
    __ access_store_at(T_INT, IN_HEAP, field, rax, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_iputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notInt);
  __ cmpl(tos_state, ctos);
  __ jcc(Assembler::notEqual, notChar);

  // ctos
  {
    __ pop(ctos);
    if (!is_static) pop_and_check_object(obj);
    __ access_store_at(T_CHAR, IN_HEAP, field, rax, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_cputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notChar);
  __ cmpl(tos_state, stos);
  __ jcc(Assembler::notEqual, notShort);

  // stos
  {
    __ pop(stos);
    if (!is_static) pop_and_check_object(obj);
    __ access_store_at(T_SHORT, IN_HEAP, field, rax, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_sputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notShort);
  __ cmpl(tos_state, ltos);
  __ jcc(Assembler::notEqual, notLong);

  // ltos
  {
    __ pop(ltos);
    if (!is_static) pop_and_check_object(obj);
    // MO_RELAXED: generate atomic store for the case of volatile field (important for x86_32)
    __ access_store_at(T_LONG, IN_HEAP | MO_RELAXED, field, noreg /* ltos*/, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_lputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notLong);
  __ cmpl(tos_state, ftos);
  __ jcc(Assembler::notEqual, notFloat);

  // ftos
  {
    __ pop(ftos);
    if (!is_static) pop_and_check_object(obj);
    __ access_store_at(T_FLOAT, IN_HEAP, field, noreg /* ftos */, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_fputfield, bc, rbx, true, byte_no);
    }
    __ jmp(Done);
  }

  __ bind(notFloat);
#ifdef ASSERT
  Label notDouble;
  __ cmpl(tos_state, dtos);
  __ jcc(Assembler::notEqual, notDouble);
#endif

  // dtos
  {
    __ pop(dtos);
    if (!is_static) pop_and_check_object(obj);
    // MO_RELAXED: for the case of volatile field, in fact it adds no extra work for the underlying implementation
    __ access_store_at(T_DOUBLE, IN_HEAP | MO_RELAXED, field, noreg /* dtos */, noreg, noreg, noreg);
    if (!is_static && rc == may_rewrite) {
      patch_bytecode(Bytecodes::_fast_dputfield, bc, rbx, true, byte_no);
    }
  }

#ifdef ASSERT
  __ jmp(Done);

  __ bind(notDouble);
  __ stop("Bad state");
#endif

  __ bind(Done);
}

void TemplateTable::putfield(int byte_no) {
  putfield_or_static(byte_no, false);
}

void TemplateTable::nofast_putfield(int byte_no) {
  putfield_or_static(byte_no, false, may_not_rewrite);
}

void TemplateTable::putstatic(int byte_no) {
  putfield_or_static(byte_no, true);
}

void TemplateTable::jvmti_post_fast_field_mod() {

  const Register scratch = c_rarg3;

  if (JvmtiExport::can_post_field_modification()) {
    // Check to see if a field modification watch has been set before
    // we take the time to call into the VM.
    Label L2;
    __ mov32(scratch, ExternalAddress((address)JvmtiExport::get_field_modification_count_addr()));
    __ testl(scratch, scratch);
    __ jcc(Assembler::zero, L2);
    __ pop_ptr(rbx);                  // copy the object pointer from tos
    __ verify_oop(rbx);
    __ push_ptr(rbx);                 // put the object pointer back on tos
    // Save tos values before call_VM() clobbers them. Since we have
    // to do it for every data type, we use the saved values as the
    // jvalue object.
    switch (bytecode()) {          // load values into the jvalue object
    case Bytecodes::_fast_aputfield: __ push_ptr(rax); break;
    case Bytecodes::_fast_bputfield: // fall through
    case Bytecodes::_fast_zputfield: // fall through
    case Bytecodes::_fast_sputfield: // fall through
    case Bytecodes::_fast_cputfield: // fall through
    case Bytecodes::_fast_iputfield: __ push_i(rax); break;
    case Bytecodes::_fast_dputfield: __ push(dtos); break;
    case Bytecodes::_fast_fputfield: __ push(ftos); break;
    case Bytecodes::_fast_lputfield: __ push_l(rax); break;

    default:
      ShouldNotReachHere();
    }
    __ mov(scratch, rsp);             // points to jvalue on the stack
    // access constant pool cache entry
    __ load_field_entry(c_rarg2, rax);
    __ verify_oop(rbx);
    // rbx: object pointer copied above
    // c_rarg2: cache entry pointer
    // c_rarg3: jvalue object on the stack
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::post_field_modification), rbx, c_rarg2, c_rarg3);

    switch (bytecode()) {             // restore tos values
    case Bytecodes::_fast_aputfield: __ pop_ptr(rax); break;
    case Bytecodes::_fast_bputfield: // fall through
    case Bytecodes::_fast_zputfield: // fall through
    case Bytecodes::_fast_sputfield: // fall through
    case Bytecodes::_fast_cputfield: // fall through
    case Bytecodes::_fast_iputfield: __ pop_i(rax); break;
    case Bytecodes::_fast_dputfield: __ pop(dtos); break;
    case Bytecodes::_fast_fputfield: __ pop(ftos); break;
    case Bytecodes::_fast_lputfield: __ pop_l(rax); break;
    default: break;
    }
    __ bind(L2);
  }
}

void TemplateTable::fast_storefield(TosState state) {
  transition(state, vtos);

  Register cache = rcx;

  Label notVolatile, Done;

  jvmti_post_fast_field_mod();

  __ push(rax);
  __ load_field_entry(rcx, rax);
  load_resolved_field_entry(noreg, cache, rax, rbx, rdx);
  // RBX: field offset, RAX: TOS, RDX: flags
  __ andl(rdx, (1 << ResolvedFieldEntry::is_volatile_shift));
  __ pop(rax);

  // Get object from stack
  pop_and_check_object(rcx);

  // field address
  const Address field(rcx, rbx, Address::times_1);

  // Check for volatile store
  __ testl(rdx, rdx);
  __ jcc(Assembler::zero, notVolatile);

  fast_storefield_helper(field, rax);
  volatile_barrier(Assembler::Membar_mask_bits(Assembler::StoreLoad |
                                               Assembler::StoreStore));
  __ jmp(Done);
  __ bind(notVolatile);

  fast_storefield_helper(field, rax);

  __ bind(Done);
}

void TemplateTable::fast_storefield_helper(Address field, Register rax) {

  // access field
  switch (bytecode()) {
  case Bytecodes::_fast_aputfield:
    do_oop_store(_masm, field, rax);
    break;
  case Bytecodes::_fast_lputfield:
    __ access_store_at(T_LONG, IN_HEAP, field, noreg /* ltos */, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_iputfield:
    __ access_store_at(T_INT, IN_HEAP, field, rax, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_zputfield:
    __ access_store_at(T_BOOLEAN, IN_HEAP, field, rax, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_bputfield:
    __ access_store_at(T_BYTE, IN_HEAP, field, rax, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_sputfield:
    __ access_store_at(T_SHORT, IN_HEAP, field, rax, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_cputfield:
    __ access_store_at(T_CHAR, IN_HEAP, field, rax, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_fputfield:
    __ access_store_at(T_FLOAT, IN_HEAP, field, noreg /* ftos*/, noreg, noreg, noreg);
    break;
  case Bytecodes::_fast_dputfield:
    __ access_store_at(T_DOUBLE, IN_HEAP, field, noreg /* dtos*/, noreg, noreg, noreg);
    break;
  default:
    ShouldNotReachHere();
  }
}

void TemplateTable::fast_accessfield(TosState state) {
  transition(atos, state);

  // Do the JVMTI work here to avoid disturbing the register state below
  if (JvmtiExport::can_post_field_access()) {
    // Check to see if a field access watch has been set before we
    // take the time to call into the VM.
    Label L1;
    __ mov32(rcx, ExternalAddress((address) JvmtiExport::get_field_access_count_addr()));
    __ testl(rcx, rcx);
    __ jcc(Assembler::zero, L1);
    // access constant pool cache entry
    __ load_field_entry(c_rarg2, rcx);
    __ verify_oop(rax);
    __ push_ptr(rax);  // save object pointer before call_VM() clobbers it
    __ mov(c_rarg1, rax);
    // c_rarg1: object pointer copied above
    // c_rarg2: cache entry pointer
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::post_field_access), c_rarg1, c_rarg2);
    __ pop_ptr(rax); // restore object pointer
    __ bind(L1);
  }

  // access constant pool cache
  __ load_field_entry(rcx, rbx);
  __ load_sized_value(rbx, Address(rcx, in_bytes(ResolvedFieldEntry::field_offset_offset())), sizeof(int), true /*is_signed*/);

  // rax: object
  __ verify_oop(rax);
  __ null_check(rax);
  Address field(rax, rbx, Address::times_1);

  // access field
  switch (bytecode()) {
  case Bytecodes::_fast_agetfield:
    do_oop_load(_masm, field, rax);
    __ verify_oop(rax);
    break;
  case Bytecodes::_fast_lgetfield:
    __ access_load_at(T_LONG, IN_HEAP, noreg /* ltos */, field, noreg);
    break;
  case Bytecodes::_fast_igetfield:
    __ access_load_at(T_INT, IN_HEAP, rax, field, noreg);
    break;
  case Bytecodes::_fast_bgetfield:
    __ access_load_at(T_BYTE, IN_HEAP, rax, field, noreg);
    break;
  case Bytecodes::_fast_sgetfield:
    __ access_load_at(T_SHORT, IN_HEAP, rax, field, noreg);
    break;
  case Bytecodes::_fast_cgetfield:
    __ access_load_at(T_CHAR, IN_HEAP, rax, field, noreg);
    break;
  case Bytecodes::_fast_fgetfield:
    __ access_load_at(T_FLOAT, IN_HEAP, noreg /* ftos */, field, noreg);
    break;
  case Bytecodes::_fast_dgetfield:
    __ access_load_at(T_DOUBLE, IN_HEAP, noreg /* dtos */, field, noreg);
    break;
  default:
    ShouldNotReachHere();
  }
  // [jk] not needed currently
  //   Label notVolatile;
  //   __ testl(rdx, rdx);
  //   __ jcc(Assembler::zero, notVolatile);
  //   __ membar(Assembler::LoadLoad);
  //   __ bind(notVolatile);
}

void TemplateTable::fast_xaccess(TosState state) {
  transition(vtos, state);

  // get receiver
  __ movptr(rax, aaddress(0));
  // access constant pool cache
  __ load_field_entry(rcx, rdx, 2);
  __ load_sized_value(rbx, Address(rcx, in_bytes(ResolvedFieldEntry::field_offset_offset())), sizeof(int), true /*is_signed*/);

  // make sure exception is reported in correct bcp range (getfield is
  // next instruction)
  __ increment(rbcp);
  __ null_check(rax);
  const Address field = Address(rax, rbx, Address::times_1, 0*wordSize);
  switch (state) {
  case itos:
    __ access_load_at(T_INT, IN_HEAP, rax, field, noreg);
    break;
  case atos:
    do_oop_load(_masm, field, rax);
    __ verify_oop(rax);
    break;
  case ftos:
    __ access_load_at(T_FLOAT, IN_HEAP, noreg /* ftos */, field, noreg);
    break;
  default:
    ShouldNotReachHere();
  }

  // [jk] not needed currently
  // Label notVolatile;
  // __ movl(rdx, Address(rcx, rdx, Address::times_8,
  //                      in_bytes(ConstantPoolCache::base_offset() +
  //                               ConstantPoolCacheEntry::flags_offset())));
  // __ shrl(rdx, ConstantPoolCacheEntry::is_volatile_shift);
  // __ testl(rdx, 0x1);
  // __ jcc(Assembler::zero, notVolatile);
  // __ membar(Assembler::LoadLoad);
  // __ bind(notVolatile);

  __ decrement(rbcp);
}

//-----------------------------------------------------------------------------
// Calls

void TemplateTable::prepare_invoke(Register cache, Register recv, Register flags) {
  // determine flags
  const Bytecodes::Code code = bytecode();
  const bool load_receiver       = (code != Bytecodes::_invokestatic) && (code != Bytecodes::_invokedynamic);
  assert_different_registers(recv, flags);

  // save 'interpreter return address'
  __ save_bcp();

  // Save flags and load TOS
  __ movl(rbcp, flags);
  __ load_unsigned_byte(flags, Address(cache, in_bytes(ResolvedMethodEntry::type_offset())));

  // load receiver if needed (after appendix is pushed so parameter size is correct)
  // Note: no return address pushed yet
  if (load_receiver) {
    __ load_unsigned_short(recv, Address(cache, in_bytes(ResolvedMethodEntry::num_parameters_offset())));
    const int no_return_pc_pushed_yet = -1;  // argument slot correction before we push return address
    const int receiver_is_at_end      = -1;  // back off one slot to get receiver
    Address recv_addr = __ argument_address(recv, no_return_pc_pushed_yet + receiver_is_at_end);
    __ movptr(recv, recv_addr);
    __ verify_oop(recv);
  }

  // load return address
  {
    const address table_addr = (address) Interpreter::invoke_return_entry_table_for(code);
    ExternalAddress table(table_addr);
    __ lea(rscratch1, table);
    __ movptr(flags, Address(rscratch1, flags, Address::times_ptr));
  }

  // push return address
  __ push(flags);

  // Restore flags value from the constant pool cache entry, and restore rsi
  // for later null checks.  r13 is the bytecode pointer
  __ movl(flags, rbcp);
  __ restore_bcp();
}

void TemplateTable::invokevirtual_helper(Register index,
                                         Register recv,
                                         Register flags) {
  // Uses temporary registers rax, rdx
  assert_different_registers(index, recv, rax, rdx);
  assert(index == rbx, "");
  assert(recv  == rcx, "");

  // Test for an invoke of a final method
  Label notFinal;
  __ movl(rax, flags);
  __ andl(rax, (1 << ResolvedMethodEntry::is_vfinal_shift));
  __ jcc(Assembler::zero, notFinal);

  const Register method = index;  // method must be rbx
  assert(method == rbx,
         "Method* must be rbx for interpreter calling convention");

  // do the call - the index is actually the method to call
  // that is, f2 is a vtable index if !is_vfinal, else f2 is a Method*

  // It's final, need a null check here!
  __ null_check(recv);

  // profile this call
  __ profile_final_call(rax);
  __ profile_arguments_type(rax, method, rbcp, true);

  __ jump_from_interpreted(method, rax);

  __ bind(notFinal);

  // get receiver klass
  __ load_klass(rax, recv, rscratch1);

  // profile this call
  __ profile_virtual_call(rax, rlocals, rdx);
  // get target Method* & entry point
  __ lookup_virtual_method(rax, index, method);

  __ profile_arguments_type(rdx, method, rbcp, true);
  __ jump_from_interpreted(method, rdx);
}

void TemplateTable::invokevirtual(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f2_byte, "use this argument");

  load_resolved_method_entry_virtual(rcx,  // ResolvedMethodEntry*
                                     rbx,  // Method or itable index
                                     rdx); // Flags
  prepare_invoke(rcx,  // ResolvedMethodEntry*
                 rcx,  // Receiver
                 rdx); // flags

  // rbx: index
  // rcx: receiver
  // rdx: flags
  invokevirtual_helper(rbx, rcx, rdx);
}

void TemplateTable::invokespecial(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  load_resolved_method_entry_special_or_static(rcx,  // ResolvedMethodEntry*
                                               rbx,  // Method*
                                               rdx); // flags
  prepare_invoke(rcx,
                 rcx,  // get receiver also for null check
                 rdx); // flags

  __ verify_oop(rcx);
  __ null_check(rcx);
  // do the call
  __ profile_call(rax);
  __ profile_arguments_type(rax, rbx, rbcp, false);
  __ jump_from_interpreted(rbx, rax);
}

void TemplateTable::invokestatic(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  load_resolved_method_entry_special_or_static(rcx, // ResolvedMethodEntry*
                                               rbx, // Method*
                                               rdx  // flags
                                               );
  prepare_invoke(rcx, rcx, rdx);  // cache and flags

  // do the call
  __ profile_call(rax);
  __ profile_arguments_type(rax, rbx, rbcp, false);
  __ jump_from_interpreted(rbx, rax);
}


void TemplateTable::fast_invokevfinal(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f2_byte, "use this argument");
  __ stop("fast_invokevfinal not used on x86");
}


void TemplateTable::invokeinterface(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  load_resolved_method_entry_interface(rcx,  // ResolvedMethodEntry*
                                       rax,  // Klass*
                                       rbx,  // Method* or itable/vtable index
                                       rdx); // flags
  prepare_invoke(rcx, rcx, rdx); // receiver, flags

  // First check for Object case, then private interface method,
  // then regular interface method.

  // Special case of invokeinterface called for virtual method of
  // java.lang.Object.  See cpCache.cpp for details.
  Label notObjectMethod;
  __ movl(rlocals, rdx);
  __ andl(rlocals, (1 << ResolvedMethodEntry::is_forced_virtual_shift));
  __ jcc(Assembler::zero, notObjectMethod);

  invokevirtual_helper(rbx, rcx, rdx);
  // no return from above
  __ bind(notObjectMethod);

  Label no_such_interface; // for receiver subtype check
  Register recvKlass; // used for exception processing

  // Check for private method invocation - indicated by vfinal
  Label notVFinal;
  __ movl(rlocals, rdx);
  __ andl(rlocals, (1 << ResolvedMethodEntry::is_vfinal_shift));
  __ jcc(Assembler::zero, notVFinal);

  // Get receiver klass into rlocals - also a null check
  __ load_klass(rlocals, rcx, rscratch1);

  Label subtype;
  __ check_klass_subtype(rlocals, rax, rbcp, subtype);
  // If we get here the typecheck failed
  recvKlass = rdx;
  __ mov(recvKlass, rlocals); // shuffle receiver class for exception use
  __ jmp(no_such_interface);

  __ bind(subtype);

  // do the call - rbx is actually the method to call

  __ profile_final_call(rdx);
  __ profile_arguments_type(rdx, rbx, rbcp, true);

  __ jump_from_interpreted(rbx, rdx);
  // no return from above
  __ bind(notVFinal);

  // Get receiver klass into rdx - also a null check
  __ restore_locals();  // restore r14
  __ load_klass(rdx, rcx, rscratch1);

  Label no_such_method;

  // Preserve method for throw_AbstractMethodErrorVerbose.
  __ mov(rcx, rbx);
  // Receiver subtype check against REFC.
  // Superklass in rax. Subklass in rdx. Blows rcx, rdi.
  __ lookup_interface_method(// inputs: rec. class, interface, itable index
                             rdx, rax, noreg,
                             // outputs: scan temp. reg, scan temp. reg
                             rbcp, rlocals,
                             no_such_interface,
                             /*return_method=*/false);

  // profile this call
  __ restore_bcp(); // rbcp was destroyed by receiver type check
  __ profile_virtual_call(rdx, rbcp, rlocals);

  // Get declaring interface class from method, and itable index
  __ load_method_holder(rax, rbx);
  __ movl(rbx, Address(rbx, Method::itable_index_offset()));
  __ subl(rbx, Method::itable_index_max);
  __ negl(rbx);

  // Preserve recvKlass for throw_AbstractMethodErrorVerbose.
  __ mov(rlocals, rdx);
  __ lookup_interface_method(// inputs: rec. class, interface, itable index
                             rlocals, rax, rbx,
                             // outputs: method, scan temp. reg
                             rbx, rbcp,
                             no_such_interface);

  // rbx: Method* to call
  // rcx: receiver
  // Check for abstract method error
  // Note: This should be done more efficiently via a throw_abstract_method_error
  //       interpreter entry point and a conditional jump to it in case of a null
  //       method.
  __ testptr(rbx, rbx);
  __ jcc(Assembler::zero, no_such_method);

  __ profile_arguments_type(rdx, rbx, rbcp, true);

  // do the call
  // rcx: receiver
  // rbx,: Method*
  __ jump_from_interpreted(rbx, rdx);
  __ should_not_reach_here();

  // exception handling code follows...
  // note: must restore interpreter registers to canonical
  //       state for exception handling to work correctly!

  __ bind(no_such_method);
  // throw exception
  __ pop(rbx);           // pop return address (pushed by prepare_invoke)
  __ restore_bcp();      // rbcp must be correct for exception handler   (was destroyed)
  __ restore_locals();   // make sure locals pointer is correct as well (was destroyed)
  // Pass arguments for generating a verbose error message.
  recvKlass = c_rarg1;
  Register method    = c_rarg2;
  if (recvKlass != rdx) { __ movq(recvKlass, rdx); }
  if (method != rcx)    { __ movq(method, rcx);    }
  __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_AbstractMethodErrorVerbose),
             recvKlass, method);
  // The call_VM checks for exception, so we should never return here.
  __ should_not_reach_here();

  __ bind(no_such_interface);
  // throw exception
  __ pop(rbx);           // pop return address (pushed by prepare_invoke)
  __ restore_bcp();      // rbcp must be correct for exception handler   (was destroyed)
  __ restore_locals();   // make sure locals pointer is correct as well (was destroyed)
  // Pass arguments for generating a verbose error message.
  if (recvKlass != rdx) {
    __ movq(recvKlass, rdx);
  }
  __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_IncompatibleClassChangeErrorVerbose),
             recvKlass, rax);
  // the call_VM checks for exception, so we should never return here.
  __ should_not_reach_here();
}

void TemplateTable::invokehandle(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");
  const Register rbx_method = rbx;
  const Register rax_mtype  = rax;
  const Register rcx_recv   = rcx;
  const Register rdx_flags  = rdx;

  load_resolved_method_entry_handle(rcx, rbx_method, rax_mtype, rdx_flags);
  prepare_invoke(rcx, rcx_recv, rdx_flags);

  __ verify_method_ptr(rbx_method);
  __ verify_oop(rcx_recv);
  __ null_check(rcx_recv);

  // rax: MethodType object (from cpool->resolved_references[f1], if necessary)
  // rbx: MH.invokeExact_MT method

  // Note:  rax_mtype is already pushed (if necessary)

  // FIXME: profile the LambdaForm also
  __ profile_final_call(rax);
  __ profile_arguments_type(rdx, rbx_method, rbcp, true);

  __ jump_from_interpreted(rbx_method, rdx);
}

void TemplateTable::invokedynamic(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  const Register rbx_method   = rbx;
  const Register rax_callsite = rax;

  load_invokedynamic_entry(rbx_method);
  // rax: CallSite object (from cpool->resolved_references[])
  // rbx: MH.linkToCallSite method

  // Note:  rax_callsite is already pushed

  // %%% should make a type profile for any invokedynamic that takes a ref argument
  // profile this call
  __ profile_call(rbcp);
  __ profile_arguments_type(rdx, rbx_method, rbcp, false);

  __ verify_oop(rax_callsite);

  __ jump_from_interpreted(rbx_method, rdx);
}

//-----------------------------------------------------------------------------
// Allocation

void TemplateTable::_new() {
  transition(vtos, atos);
  __ get_unsigned_2_byte_index_at_bcp(rdx, 1);
  Label slow_case;
  Label slow_case_no_pop;
  Label done;
  Label initialize_header;

  __ get_cpool_and_tags(rcx, rax);

  // Make sure the class we're about to instantiate has been resolved.
  // This is done before loading InstanceKlass to be consistent with the order
  // how Constant Pool is updated (see ConstantPool::klass_at_put)
  const int tags_offset = Array<u1>::base_offset_in_bytes();
  __ cmpb(Address(rax, rdx, Address::times_1, tags_offset), JVM_CONSTANT_Class);
  __ jcc(Assembler::notEqual, slow_case_no_pop);

  // get InstanceKlass
  __ load_resolved_klass_at_index(rcx, rcx, rdx);
  __ push(rcx);  // save the contexts of klass for initializing the header

  // make sure klass is initialized
  // init_state needs acquire, but x86 is TSO, and so we are already good.
  assert(VM_Version::supports_fast_class_init_checks(), "must support fast class initialization checks");
  __ clinit_barrier(rcx, nullptr /*L_fast_path*/, &slow_case);

  // get instance_size in InstanceKlass (scaled to a count of bytes)
  __ movl(rdx, Address(rcx, Klass::layout_helper_offset()));
  // test to see if it is malformed in some way
  __ testl(rdx, Klass::_lh_instance_slow_path_bit);
  __ jcc(Assembler::notZero, slow_case);

  // Allocate the instance:
  //  If TLAB is enabled:
  //    Try to allocate in the TLAB.
  //    If fails, go to the slow path.
  //    Initialize the allocation.
  //    Exit.
  //
  //  Go to slow path.

  if (UseTLAB) {
    __ tlab_allocate(rax, rdx, 0, rcx, rbx, slow_case);
    if (ZeroTLAB) {
      // the fields have been already cleared
      __ jmp(initialize_header);
    }

    // The object is initialized before the header.  If the object size is
    // zero, go directly to the header initialization.
    if (UseCompactObjectHeaders) {
      __ decrement(rdx, align_up(oopDesc::base_offset_in_bytes(), BytesPerLong));
    } else {
      __ decrement(rdx, sizeof(oopDesc));
    }
    __ jcc(Assembler::zero, initialize_header);

    // Initialize topmost object field, divide rdx by 8, check if odd and
    // test if zero.
    __ xorl(rcx, rcx);    // use zero reg to clear memory (shorter code)
    __ shrl(rdx, LogBytesPerLong); // divide by 2*oopSize and set carry flag if odd

    // rdx must have been multiple of 8
#ifdef ASSERT
    // make sure rdx was multiple of 8
    Label L;
    // Ignore partial flag stall after shrl() since it is debug VM
    __ jcc(Assembler::carryClear, L);
    __ stop("object size is not multiple of 2 - adjust this code");
    __ bind(L);
    // rdx must be > 0, no extra check needed here
#endif

    // initialize remaining object fields: rdx was a multiple of 8
    { Label loop;
    __ bind(loop);
    int header_size_bytes = oopDesc::header_size() * HeapWordSize;
    assert(is_aligned(header_size_bytes, BytesPerLong), "oop header size must be 8-byte-aligned");
    __ movptr(Address(rax, rdx, Address::times_8, header_size_bytes - 1*oopSize), rcx);
    __ decrement(rdx);
    __ jcc(Assembler::notZero, loop);
    }

    // initialize object header only.
    __ bind(initialize_header);
    if (UseCompactObjectHeaders) {
      __ pop(rcx);   // get saved klass back in the register.
      __ movptr(rbx, Address(rcx, Klass::prototype_header_offset()));
      __ movptr(Address(rax, oopDesc::mark_offset_in_bytes()), rbx);
    } else {
      __ movptr(Address(rax, oopDesc::mark_offset_in_bytes()),
                (intptr_t)markWord::prototype().value()); // header
      __ pop(rcx);   // get saved klass back in the register.
      __ xorl(rsi, rsi); // use zero reg to clear memory (shorter code)
      __ store_klass_gap(rax, rsi);  // zero klass gap for compressed oops
      __ store_klass(rax, rcx, rscratch1);  // klass
    }

    if (DTraceAllocProbes) {
      // Trigger dtrace event for fastpath
      __ push(atos);
      __ call_VM_leaf(
           CAST_FROM_FN_PTR(address, static_cast<int (*)(oopDesc*)>(SharedRuntime::dtrace_object_alloc)), rax);
      __ pop(atos);
    }

    __ jmp(done);
  }

  // slow case
  __ bind(slow_case);
  __ pop(rcx);   // restore stack pointer to what it was when we came in.
  __ bind(slow_case_no_pop);

  __ get_constant_pool(c_rarg1);
  __ get_unsigned_2_byte_index_at_bcp(c_rarg2, 1);
  call_VM(rax, CAST_FROM_FN_PTR(address, InterpreterRuntime::_new), c_rarg1, c_rarg2);
   __ verify_oop(rax);

  // continue
  __ bind(done);
}

void TemplateTable::newarray() {
  transition(itos, atos);
  __ load_unsigned_byte(c_rarg1, at_bcp(1));
  call_VM(rax, CAST_FROM_FN_PTR(address, InterpreterRuntime::newarray),
          c_rarg1, rax);
}

void TemplateTable::anewarray() {
  transition(itos, atos);

  __ get_unsigned_2_byte_index_at_bcp(c_rarg2, 1);
  __ get_constant_pool(c_rarg1);
  call_VM(rax, CAST_FROM_FN_PTR(address, InterpreterRuntime::anewarray),
          c_rarg1, c_rarg2, rax);
}

void TemplateTable::arraylength() {
  transition(atos, itos);
  __ movl(rax, Address(rax, arrayOopDesc::length_offset_in_bytes()));
}

void TemplateTable::checkcast() {
  transition(atos, atos);
  Label done, is_null, ok_is_subtype, quicked, resolved;
  __ testptr(rax, rax); // object is in rax
  __ jcc(Assembler::zero, is_null);

  // Get cpool & tags index
  __ get_cpool_and_tags(rcx, rdx); // rcx=cpool, rdx=tags array
  __ get_unsigned_2_byte_index_at_bcp(rbx, 1); // rbx=index
  // See if bytecode has already been quicked
  __ cmpb(Address(rdx, rbx,
                  Address::times_1,
                  Array<u1>::base_offset_in_bytes()),
          JVM_CONSTANT_Class);
  __ jcc(Assembler::equal, quicked);
  __ push(atos); // save receiver for result, and for GC
  call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));

  __ get_vm_result_metadata(rax);

  __ pop_ptr(rdx); // restore receiver
  __ jmpb(resolved);

  // Get superklass in rax and subklass in rbx
  __ bind(quicked);
  __ mov(rdx, rax); // Save object in rdx; rax needed for subtype check
  __ load_resolved_klass_at_index(rax, rcx, rbx);

  __ bind(resolved);
  __ load_klass(rbx, rdx, rscratch1);

  // Generate subtype check.  Blows rcx, rdi.  Object in rdx.
  // Superklass in rax.  Subklass in rbx.
  __ gen_subtype_check(rbx, ok_is_subtype);

  // Come here on failure
  __ push_ptr(rdx);
  // object is at TOS
  __ jump(RuntimeAddress(Interpreter::_throw_ClassCastException_entry));

  // Come here on success
  __ bind(ok_is_subtype);
  __ mov(rax, rdx); // Restore object in rdx

  // Collect counts on whether this check-cast sees nulls a lot or not.
  if (ProfileInterpreter) {
    __ jmp(done);
    __ bind(is_null);
    __ profile_null_seen(rcx);
  } else {
    __ bind(is_null);   // same as 'done'
  }
  __ bind(done);
}

void TemplateTable::instanceof() {
  transition(atos, itos);
  Label done, is_null, ok_is_subtype, quicked, resolved;
  __ testptr(rax, rax);
  __ jcc(Assembler::zero, is_null);

  // Get cpool & tags index
  __ get_cpool_and_tags(rcx, rdx); // rcx=cpool, rdx=tags array
  __ get_unsigned_2_byte_index_at_bcp(rbx, 1); // rbx=index
  // See if bytecode has already been quicked
  __ cmpb(Address(rdx, rbx,
                  Address::times_1,
                  Array<u1>::base_offset_in_bytes()),
          JVM_CONSTANT_Class);
  __ jcc(Assembler::equal, quicked);

  __ push(atos); // save receiver for result, and for GC
  call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));

  __ get_vm_result_metadata(rax);

  __ pop_ptr(rdx); // restore receiver
  __ verify_oop(rdx);
  __ load_klass(rdx, rdx, rscratch1);
  __ jmpb(resolved);

  // Get superklass in rax and subklass in rdx
  __ bind(quicked);
  __ load_klass(rdx, rax, rscratch1);
  __ load_resolved_klass_at_index(rax, rcx, rbx);

  __ bind(resolved);

  // Generate subtype check.  Blows rcx, rdi
  // Superklass in rax.  Subklass in rdx.
  __ gen_subtype_check(rdx, ok_is_subtype);

  // Come here on failure
  __ xorl(rax, rax);
  __ jmpb(done);
  // Come here on success
  __ bind(ok_is_subtype);
  __ movl(rax, 1);

  // Collect counts on whether this test sees nulls a lot or not.
  if (ProfileInterpreter) {
    __ jmp(done);
    __ bind(is_null);
    __ profile_null_seen(rcx);
  } else {
    __ bind(is_null);   // same as 'done'
  }
  __ bind(done);
  // rax = 0: obj == nullptr or  obj is not an instanceof the specified klass
  // rax = 1: obj != nullptr and obj is     an instanceof the specified klass
}


//----------------------------------------------------------------------------------------------------
// Breakpoints
void TemplateTable::_breakpoint() {
  // Note: We get here even if we are single stepping..
  // jbug insists on setting breakpoints at every bytecode
  // even if we are in single step mode.

  transition(vtos, vtos);

  // get the unpatched byte code
  __ get_method(c_rarg1);
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address,
                              InterpreterRuntime::get_original_bytecode_at),
             c_rarg1, rbcp);
  __ mov(rbx, rax);  // why?

  // post the breakpoint event
  __ get_method(c_rarg1);
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address, InterpreterRuntime::_breakpoint),
             c_rarg1, rbcp);

  // complete the execution of original bytecode
  __ dispatch_only_normal(vtos);
}

//-----------------------------------------------------------------------------
// Exceptions

void TemplateTable::athrow() {
  transition(atos, vtos);
  __ null_check(rax);
  __ jump(RuntimeAddress(Interpreter::throw_exception_entry()));
}

//-----------------------------------------------------------------------------
// Synchronization
//
// Note: monitorenter & exit are symmetric routines; which is reflected
//       in the assembly code structure as well
//
// Stack layout:
//
// [expressions  ] <--- rsp               = expression stack top
// ..
// [expressions  ]
// [monitor entry] <--- monitor block top = expression stack bot
// ..
// [monitor entry]
// [frame data   ] <--- monitor block bot
// ...
// [saved rbp    ] <--- rbp
void TemplateTable::monitorenter() {
  transition(atos, vtos);

  // check for null object
  __ null_check(rax);

  const Address monitor_block_top(
        rbp, frame::interpreter_frame_monitor_block_top_offset * wordSize);
  const Address monitor_block_bot(
        rbp, frame::interpreter_frame_initial_sp_offset * wordSize);
  const int entry_size = frame::interpreter_frame_monitor_size_in_bytes();

  Label allocated;

  Register rtop = c_rarg3;
  Register rbot = c_rarg2;
  Register rmon = c_rarg1;

  // initialize entry pointer
  __ xorl(rmon, rmon); // points to free slot or null

  // find a free slot in the monitor block (result in rmon)
  {
    Label entry, loop, exit;
    __ movptr(rtop, monitor_block_top); // derelativize pointer
    __ lea(rtop, Address(rbp, rtop, Address::times_ptr));
    // rtop points to current entry, starting with top-most entry

    __ lea(rbot, monitor_block_bot);    // points to word before bottom
                                        // of monitor block
    __ jmpb(entry);

    __ bind(loop);
    // check if current entry is used
    __ cmpptr(Address(rtop, BasicObjectLock::obj_offset()), NULL_WORD);
    // if not used then remember entry in rmon
    __ cmovptr(Assembler::equal, rmon, rtop);   // cmov => cmovptr
    // check if current entry is for same object
    __ cmpptr(rax, Address(rtop, BasicObjectLock::obj_offset()));
    // if same object then stop searching
    __ jccb(Assembler::equal, exit);
    // otherwise advance to next entry
    __ addptr(rtop, entry_size);
    __ bind(entry);
    // check if bottom reached
    __ cmpptr(rtop, rbot);
    // if not at bottom then check this entry
    __ jcc(Assembler::notEqual, loop);
    __ bind(exit);
  }

  __ testptr(rmon, rmon); // check if a slot has been found
  __ jcc(Assembler::notZero, allocated); // if found, continue with that one

  // allocate one if there's no free slot
  {
    Label entry, loop;
    // 1. compute new pointers          // rsp: old expression stack top
    __ movptr(rmon, monitor_block_bot); // rmon: old expression stack bottom
    __ lea(rmon, Address(rbp, rmon, Address::times_ptr));
    __ subptr(rsp, entry_size);         // move expression stack top
    __ subptr(rmon, entry_size);        // move expression stack bottom
    __ mov(rtop, rsp);                  // set start value for copy loop
    __ subptr(monitor_block_bot, entry_size / wordSize); // set new monitor block bottom
    __ jmp(entry);
    // 2. move expression stack contents
    __ bind(loop);
    __ movptr(rbot, Address(rtop, entry_size)); // load expression stack
                                                // word from old location
    __ movptr(Address(rtop, 0), rbot);          // and store it at new location
    __ addptr(rtop, wordSize);                  // advance to next word
    __ bind(entry);
    __ cmpptr(rtop, rmon);                      // check if bottom reached
    __ jcc(Assembler::notEqual, loop);          // if not at bottom then
                                                // copy next word
  }

  // call run-time routine
  // rmon: points to monitor entry
  __ bind(allocated);

  // Increment bcp to point to the next bytecode, so exception
  // handling for async. exceptions work correctly.
  // The object has already been popped from the stack, so the
  // expression stack looks correct.
  __ increment(rbcp);

  // store object
  __ movptr(Address(rmon, BasicObjectLock::obj_offset()), rax);
  __ lock_object(rmon);

  // check to make sure this monitor doesn't cause stack overflow after locking
  __ save_bcp();  // in case of exception
  __ generate_stack_overflow_check(0);

  // The bcp has already been incremented. Just need to dispatch to
  // next instruction.
  __ dispatch_next(vtos);
}

void TemplateTable::monitorexit() {
  transition(atos, vtos);

  // check for null object
  __ null_check(rax);

  const Address monitor_block_top(
        rbp, frame::interpreter_frame_monitor_block_top_offset * wordSize);
  const Address monitor_block_bot(
        rbp, frame::interpreter_frame_initial_sp_offset * wordSize);
  const int entry_size = frame::interpreter_frame_monitor_size_in_bytes();

  Register rtop = c_rarg1;
  Register rbot = c_rarg2;

  Label found;

  // find matching slot
  {
    Label entry, loop;
    __ movptr(rtop, monitor_block_top); // derelativize pointer
    __ lea(rtop, Address(rbp, rtop, Address::times_ptr));
    // rtop points to current entry, starting with top-most entry

    __ lea(rbot, monitor_block_bot);    // points to word before bottom
                                        // of monitor block
    __ jmpb(entry);

    __ bind(loop);
    // check if current entry is for same object
    __ cmpptr(rax, Address(rtop, BasicObjectLock::obj_offset()));
    // if same object then stop searching
    __ jcc(Assembler::equal, found);
    // otherwise advance to next entry
    __ addptr(rtop, entry_size);
    __ bind(entry);
    // check if bottom reached
    __ cmpptr(rtop, rbot);
    // if not at bottom then check this entry
    __ jcc(Assembler::notEqual, loop);
  }

  // error handling. Unlocking was not block-structured
  __ call_VM(noreg, CAST_FROM_FN_PTR(address,
                   InterpreterRuntime::throw_illegal_monitor_state_exception));
  __ should_not_reach_here();

  // call run-time routine
  __ bind(found);
  __ push_ptr(rax); // make sure object is on stack (contract with oopMaps)
  __ unlock_object(rtop);
  __ pop_ptr(rax); // discard object
}

// Wide instructions
void TemplateTable::wide() {
  transition(vtos, vtos);
  __ load_unsigned_byte(rbx, at_bcp(1));
  ExternalAddress wtable((address)Interpreter::_wentry_point);
  __ jump(ArrayAddress(wtable, Address(noreg, rbx, Address::times_ptr)), rscratch1);
  // Note: the rbcp increment step is part of the individual wide bytecode implementations
}

// Multi arrays
void TemplateTable::multianewarray() {
  transition(vtos, atos);

  __ load_unsigned_byte(rax, at_bcp(3)); // get number of dimensions
  // last dim is on top of stack; we want address of first one:
  // first_addr = last_addr + (ndims - 1) * stackElementSize - 1*wordsize
  // the latter wordSize to point to the beginning of the array.
  __ lea(c_rarg1, Address(rsp, rax, Interpreter::stackElementScale(), -wordSize));
  call_VM(rax, CAST_FROM_FN_PTR(address, InterpreterRuntime::multianewarray), c_rarg1);
  __ load_unsigned_byte(rbx, at_bcp(3));
  __ lea(rsp, Address(rsp, rbx, Interpreter::stackElementScale()));  // get rid of counts
}
