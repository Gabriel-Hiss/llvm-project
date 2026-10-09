//===- X86Lift.td - hand-written lift semantics for x86 -------------------===//
//
// Scalar/GPR semantics the ISel patterns cannot express (EFLAGS as separate
// i1 flags, stack ops, control flow, mul/div pairs, instruction-variant
// families). The -gen-revenant-lift backend reads RVLift/RVMatch records
// here and in the sibling family files (Sse.td, X87.td, StringAtomic.td,
// System.td). This file declares every shared op; family files declare only
// their own namespaced ops (rv_x87*, rv_x*/rv_p*/rv_v*, ...).
//
// A body is a dag evaluated against an X86LiftContext:
//
//   Leaves (expressions):
//     $name          declared operand by name, read typed by context
//     (rv_op N)      Nth declared operand counting ins after outs
//     (rv_out N)     Nth declared out operand
//     (rv_reg "N")   physical register by X86 enum name, own width
//     (rv_flags)     EFLAGS operand placeholder, yields undef
//     (rv_imm N)     literal, width from context
//     (rv_immw W N)  literal of exactly W bits
//     plain ints     same as rv_imm
//
//   Statements:
//     (rv_set T, E)      write expr E to target T ($name, rv_op, rv_reg,
//                        or a memory operand)
//     (rv_setz T, E)     rv_set but zero-extend to the root reg width (VEX)
//     (rv_store A, E)    store E at address expr A (i64)
//     (rv_seq S...)      run statements in order
//     (rv_nop)           no-op
//     (rv_opaque)        force opaque lifting (@revenant.asm.<name>)
//
//   Widths: expressions infer an integer bit width from context; rv_set
//   passes the destination width down, rv_zext/sext/trunc and width-taking
//   ops set it explicitly. Memory operands read/write at the context width,
//   which defaults to the instruction's scalar element width.
//
// Attachment: RVLift binds an exact Instruction record; RVMatch binds every
// instruction whose def name matches a glob (* = any infix).
// Resolution order: RVLift > RVMatch > inverted ISel pattern > opaque.
//
//===----------------------------------------------------------------------===//

// ---- shared op classes ----------------------------------------------------

class RVOp;

// statements
def rv_seq   : RVOp;
def rv_set   : RVOp;
def rv_setz  : RVOp;
def rv_store : RVOp;
def rv_nop   : RVOp;
def rv_opaque: RVOp;
// operand/leaf expressions
def rv_op    : RVOp;
def rv_out   : RVOp;
def rv_flags : RVOp;
def rv_imm   : RVOp;
def rv_immw  : RVOp;
def rv_reg   : RVOp;
// integer expressions (type-agnostic where IRBuilder allows)
def rv_add   : RVOp;
def rv_sub   : RVOp;
def rv_mul   : RVOp;
def rv_and   : RVOp;
def rv_or    : RVOp;
def rv_xor   : RVOp;
def rv_not   : RVOp;
def rv_shl   : RVOp;
def rv_lshr  : RVOp;
def rv_ashr  : RVOp;
def rv_udiv  : RVOp;
def rv_sdiv  : RVOp;
def rv_urem  : RVOp;
def rv_srem  : RVOp;
def rv_zext  : RVOp;  // (rv_zext [W,] x)
def rv_sext  : RVOp;
def rv_trunc : RVOp;
def rv_icmp  : RVOp;  // (rv_icmp "eq|ne|ult|ule|ugt|uge|slt|sle|sgt|sge", a, b)
def rv_select: RVOp;  // (rv_select cond, a, b)
def rv_fshl  : RVOp;  // (rv_fshl W, a, b, c)
def rv_fshr  : RVOp;
def rv_load  : RVOp;  // (rv_load W, addr_i64); W=0 -> context width
def rv_fload : RVOp;  // (rv_fload "f32|f64|f80", addr)
def rv_addr  : RVOp;  // (rv_addr $m): effective address of a memory operand
def rv_extract : RVOp;// (rv_extract N, agg)
// fp expressions (type-polymorphic)
def rv_ffadd   : RVOp;
def rv_ffsub   : RVOp;
def rv_ffmul   : RVOp;
def rv_ffdiv   : RVOp;
def rv_fneg    : RVOp;
def rv_fminnum : RVOp;
def rv_fmaxnum : RVOp;
def rv_fsqrt   : RVOp;
def rv_sitofpf32 : RVOp;
def rv_sitofpf64 : RVOp;
def rv_fptosi  : RVOp;  // (rv_fptosi W, x) -> iW, truncating
def rv_fpext   : RVOp;  // dest type from context width
def rv_fptrunc : RVOp;
// flag ops: evaluate to a value AND set EFLAGS as a side effect
def rv_fadd  : RVOp;  // add + flags (CF/OF/AF/ZF/SF/PF)
def rv_fsub  : RVOp;
def rv_fand  : RVOp;  // logical result: CF=OF=0, AF undef
def rv_for   : RVOp;
def rv_fxor  : RVOp;
def rv_fadc  : RVOp;
def rv_fsbb  : RVOp;
def rv_finc  : RVOp;  // add semantics, CF preserved
def rv_fdec  : RVOp;
def rv_fnegx : RVOp;  // 0 - x, CF = (x != 0)
def rv_fmul  : RVOp;  // signed low product; CF=OF = (high != sext)
def rv_fumul : RVOp;  // unsigned low product; CF=OF = (high != 0)
def rv_fcmp  : RVOp;  // sub flags, result discarded
def rv_fcmpf : RVOp;  // unordered fp compare flags (ZF=uno|eq,PF=uno,CF=uno|lt)
def rv_fshift  : RVOp; // (rv_fshift "shl|shr|sar|rol|ror", val, cnt)
def rv_fshift2 : RVOp; // (rv_fshift2 "shld|shrd", val, extra, cnt)
// flag image / plumbing
def rv_flagread : RVOp;  // (rv_flagread "cf|zf|sf|of|pf|af|df") -> i1
def rv_flagset  : RVOp;  // (rv_flagset "cf|zf|sf|of|pf|af|df", i1)
def rv_sahf : RVOp;
def rv_lahf : RVOp;   // yields the AH flags image (i8)
// control flow / stack
def rv_cpred : RVOp;  // (rv_cpred $op | "cc"): i1 from cond operand or literal
def rv_setcc : RVOp;  // (rv_setcc $op | "cc"): zext(cpred) to i8
def rv_br    : RVOp;  // (rv_br $op): jump
def rv_brcc  : RVOp;  // (rv_brcc $dst, $cond | "cc"): conditional jump
def rv_brcond: RVOp;  // (rv_brcond $dst, i1-expr): jump if expr
def rv_callr : RVOp;  // (rv_callr $op): indirect call
def rv_calli : RVOp;  // (rv_calli $op): direct call (pcrel target)
def rv_ret   : RVOp;  // ret [imm16]
def rv_push  : RVOp;  // (rv_push W, val)
def rv_pop   : RVOp;  // (rv_pop W) -> popped value of W bits
def rv_pushf : RVOp;
def rv_popf  : RVOp;
def rv_leave : RVOp;
// read-modify-write, locks, fences
def rv_frmw  : RVOp;  // (rv_frmw "add|sub|and|or|xor|inc|dec|neg|not", $mem, $src)
                      // statement: load+op+flags+store; atomicrmw when lock
def rv_atomicrmw : RVOp; // (rv_atomicrmw "op", addr, val) -> old
def rv_cmpxchg   : RVOp; // (rv_cmpxchg addr, expected, new) -> old; sets flags
def rv_cmpxchgr  : RVOp; // (rv_cmpxchgr $dst, $src): reg form; acc = A-register
def rv_cmpxchg8b : RVOp; // (rv_cmpxchg8b addr): edx:eax vs ebx:ecx
def rv_cmpxchg16b: RVOp; // (rv_cmpxchg16b addr): rdx:rax vs rbx:rcx
def rv_fence     : RVOp;
def rv_memmove   : RVOp; // (rv_memmove dst, src, len) i64 args
def rv_memset    : RVOp; // (rv_memset dst, val_i8, len)
// misc families
def rv_xchg  : RVOp;  // (rv_xchg $a, $b): exchange; args may be rv_reg too
def rv_xadd  : RVOp;  // (rv_xadd $dst, $src): full statement, both writes+flags
def rv_mulpair : RVOp;  // (rv_mulpair "u|s", W, $src): rdx:rax = acc*src
def rv_divpair : RVOp;  // (rv_divpair "u|s", W, $src)
def rv_sextacc : RVOp;  // (rv_sextacc W): cbw=16,cwde=32,cdqe=64; cwd/cdq/cqo=+100
def rv_bt    : RVOp;  // (rv_bt "bt|bts|btr|btc", $base, $off)
def rv_bsf   : RVOp;
def rv_bsr   : RVOp;
def rv_tzcnt : RVOp;
def rv_lzcnt : RVOp;
def rv_popcnt: RVOp;
def rv_bswap : RVOp;
def rv_rorx  : RVOp;  // (rv_rorx val, cnt): rotate right, no flags
// prefix state (separate MCInsts, tracked across instructions)
def rv_prefix  : RVOp; // (rv_prefix "lock|rep|repe|repne|seg"[, rv_reg "FS"])
def rv_hasrep  : RVOp;
def rv_hasrepne: RVOp;
def rv_hasrepe : RVOp;
def rv_haslock : RVOp;
def rv_df      : RVOp; // direction flag as i1
// x87 entry points used by defs here; the family file declares the rest.
def rv_x87init : RVOp;

// ---- attachment classes ------------------------------------------------------

/// Hand-written semantics for one instruction record.
class RVLift {
  Instruction Insn;
  dag Body;
}

/// Hand-written semantics for every instruction whose name matches a glob
/// (e.g. "SHL32*" covers _ND/_EVEX/_NF variants).
class RVMatch {
  string Glob;
  dag Body;
}

// ---- flag-setting binary ops ------------------------------------------------
// rr/ri/rm forms: dst tied to src1. mr/mi forms go through rv_frmw so LOCK
// becomes an atomicrmw. Accumulator-immediate forms write the A-register
// directly.

// ADD
foreach I = [ADD8rr, ADD16rr, ADD32rr, ADD64rr,
             ADD8rr_REV, ADD16rr_REV, ADD32rr_REV, ADD64rr_REV,
             ADD8ri, ADD16ri, ADD16ri8, ADD32ri, ADD32ri8,
             ADD64ri8, ADD64ri32,
             ADD8rm, ADD16rm, ADD32rm, ADD64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fadd $src1, $src2));
  }
foreach I = [ADD8mr, ADD16mr, ADD32mr, ADD64mr,
             ADD8mi, ADD16mi, ADD16mi8, ADD32mi, ADD32mi8,
             ADD64mi8, ADD64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "add", $dst, $src);
  }
def : RVLift { let Insn = ADD8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_fadd (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = ADD16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_fadd (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = ADD32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_fadd (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = ADD64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_fadd (rv_reg "RAX"), $src)); }

// SUB
foreach I = [SUB8rr, SUB16rr, SUB32rr, SUB64rr,
             SUB8rr_REV, SUB16rr_REV, SUB32rr_REV, SUB64rr_REV,
             SUB8ri, SUB16ri, SUB16ri8, SUB32ri, SUB32ri8,
             SUB64ri8, SUB64ri32,
             SUB8rm, SUB16rm, SUB32rm, SUB64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fsub $src1, $src2));
  }
foreach I = [SUB8mr, SUB16mr, SUB32mr, SUB64mr,
             SUB8mi, SUB16mi, SUB16mi8, SUB32mi, SUB32mi8,
             SUB64mi8, SUB64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "sub", $dst, $src);
  }
def : RVLift { let Insn = SUB8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_fsub (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = SUB16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_fsub (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = SUB32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_fsub (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = SUB64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_fsub (rv_reg "RAX"), $src)); }

// ADC
foreach I = [ADC8rr, ADC16rr, ADC32rr, ADC64rr,
             ADC8rr_REV, ADC16rr_REV, ADC32rr_REV, ADC64rr_REV,
             ADC8ri, ADC16ri, ADC16ri8, ADC32ri, ADC32ri8,
             ADC64ri8, ADC64ri32,
             ADC8rm, ADC16rm, ADC32rm, ADC64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fadc $src1, $src2));
  }
foreach I = [ADC8mr, ADC16mr, ADC32mr, ADC64mr,
             ADC8mi, ADC16mi, ADC16mi8, ADC32mi, ADC32mi8,
             ADC64mi8, ADC64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "adc", $dst, $src);
  }
def : RVLift { let Insn = ADC8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_fadc (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = ADC16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_fadc (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = ADC32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_fadc (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = ADC64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_fadc (rv_reg "RAX"), $src)); }

// SBB
foreach I = [SBB8rr, SBB16rr, SBB32rr, SBB64rr,
             SBB8rr_REV, SBB16rr_REV, SBB32rr_REV, SBB64rr_REV,
             SBB8ri, SBB16ri, SBB16ri8, SBB32ri, SBB32ri8,
             SBB64ri8, SBB64ri32,
             SBB8rm, SBB16rm, SBB32rm, SBB64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fsbb $src1, $src2));
  }
foreach I = [SBB8mr, SBB16mr, SBB32mr, SBB64mr,
             SBB8mi, SBB16mi, SBB16mi8, SBB32mi, SBB32mi8,
             SBB64mi8, SBB64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "sbb", $dst, $src);
  }
def : RVLift { let Insn = SBB8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_fsbb (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = SBB16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_fsbb (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = SBB32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_fsbb (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = SBB64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_fsbb (rv_reg "RAX"), $src)); }

// AND
foreach I = [AND8rr, AND16rr, AND32rr, AND64rr,
             AND8rr_REV, AND16rr_REV, AND32rr_REV, AND64rr_REV,
             AND8ri, AND16ri, AND16ri8, AND32ri, AND32ri8,
             AND64ri8, AND64ri32,
             AND8rm, AND16rm, AND32rm, AND64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fand $src1, $src2));
  }
foreach I = [AND8mr, AND16mr, AND32mr, AND64mr,
             AND8mi, AND16mi, AND16mi8, AND32mi, AND32mi8,
             AND64mi8, AND64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "and", $dst, $src);
  }
def : RVLift { let Insn = AND8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_fand (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = AND16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_fand (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = AND32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_fand (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = AND64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_fand (rv_reg "RAX"), $src)); }

// OR
foreach I = [OR8rr, OR16rr, OR32rr, OR64rr,
             OR8rr_REV, OR16rr_REV, OR32rr_REV, OR64rr_REV,
             OR8ri, OR16ri, OR16ri8, OR32ri, OR32ri8,
             OR64ri8, OR64ri32,
             OR8rm, OR16rm, OR32rm, OR64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_for $src1, $src2));
  }
foreach I = [OR8mr, OR16mr, OR32mr, OR64mr,
             OR8mi, OR16mi, OR16mi8, OR32mi, OR32mi8,
             OR64mi8, OR64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "or", $dst, $src);
  }
def : RVLift { let Insn = OR8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_for (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = OR16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_for (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = OR32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_for (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = OR64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_for (rv_reg "RAX"), $src)); }

// XOR
foreach I = [XOR8rr, XOR16rr, XOR32rr, XOR64rr,
             XOR8rr_REV, XOR16rr_REV, XOR32rr_REV, XOR64rr_REV,
             XOR8rr_NOREX,
             XOR8ri, XOR16ri, XOR16ri8, XOR32ri, XOR32ri8,
             XOR64ri8, XOR64ri32,
             XOR8rm, XOR16rm, XOR32rm, XOR64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fxor $src1, $src2));
  }
foreach I = [XOR8mr, XOR16mr, XOR32mr, XOR64mr,
             XOR8mi, XOR16mi, XOR16mi8, XOR32mi, XOR32mi8,
             XOR64mi8, XOR64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_frmw "xor", $dst, $src);
  }
def : RVLift { let Insn = XOR8i8;   dag Body = (rv_set (rv_reg "AL"),  (rv_fxor (rv_reg "AL"),  $src)); }
def : RVLift { let Insn = XOR16i16; dag Body = (rv_set (rv_reg "AX"),  (rv_fxor (rv_reg "AX"),  $src)); }
def : RVLift { let Insn = XOR32i32; dag Body = (rv_set (rv_reg "EAX"), (rv_fxor (rv_reg "EAX"), $src)); }
def : RVLift { let Insn = XOR64i32; dag Body = (rv_set (rv_reg "RAX"), (rv_fxor (rv_reg "RAX"), $src)); }

// ---- cmp/test ---------------------------------------------------------------

foreach I = [CMP8rr, CMP16rr, CMP32rr, CMP64rr,
             CMP8rr_REV, CMP16rr_REV, CMP32rr_REV, CMP64rr_REV,
             CMP8ri, CMP16ri, CMP16ri8, CMP32ri, CMP32ri8,
             CMP64ri8, CMP64ri32,
             CMP8rm, CMP16rm, CMP32rm, CMP64rm,
             CMP8mr, CMP16mr, CMP32mr, CMP64mr,
             CMP8mi, CMP16mi, CMP16mi8, CMP32mi, CMP32mi8,
             CMP64mi8, CMP64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_fcmp $src1, $src2);
  }
def : RVLift { let Insn = CMP8i8;   dag Body = (rv_fcmp (rv_reg "AL"),  $src); }
def : RVLift { let Insn = CMP16i16; dag Body = (rv_fcmp (rv_reg "AX"),  $src); }
def : RVLift { let Insn = CMP32i32; dag Body = (rv_fcmp (rv_reg "EAX"), $src); }
def : RVLift { let Insn = CMP64i32; dag Body = (rv_fcmp (rv_reg "RAX"), $src); }

// TEST: and-flags without a destination write.
foreach I = [TEST8rr, TEST16rr, TEST32rr, TEST64rr,
             TEST8ri, TEST16ri, TEST32ri, TEST64ri32,
             TEST8mr, TEST16mr, TEST32mr, TEST64mr,
             TEST8mi, TEST16mi, TEST32mi, TEST64mi32] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_fand $src1, $src2);
  }
def : RVLift { let Insn = TEST8i8;   dag Body = (rv_fand (rv_reg "AL"),  $src); }
def : RVLift { let Insn = TEST16i16; dag Body = (rv_fand (rv_reg "AX"),  $src); }
def : RVLift { let Insn = TEST32i32; dag Body = (rv_fand (rv_reg "EAX"), $src); }
def : RVLift { let Insn = TEST64i32; dag Body = (rv_fand (rv_reg "RAX"), $src); }

// ---- inc/dec/neg/not -----------------------------------------------------------

foreach I = [INC8r, INC16r, INC32r, INC64r] in
  def : RVLift { let Insn = I; dag Body = (rv_set $dst, (rv_finc $src1)); }
foreach I = [INC8m, INC16m, INC32m, INC64m] in
  def : RVLift { let Insn = I; dag Body = (rv_frmw "inc", $src1, (rv_imm 1)); }
foreach I = [DEC8r, DEC16r, DEC32r, DEC64r] in
  def : RVLift { let Insn = I; dag Body = (rv_set $dst, (rv_fdec $src1)); }
foreach I = [DEC8m, DEC16m, DEC32m, DEC64m] in
  def : RVLift { let Insn = I; dag Body = (rv_frmw "dec", $src1, (rv_imm 1)); }
foreach I = [NEG8r, NEG16r, NEG32r, NEG64r] in
  def : RVLift { let Insn = I; dag Body = (rv_set $dst, (rv_fnegx $src1)); }
foreach I = [NEG8m, NEG16m, NEG32m, NEG64m] in
  def : RVLift { let Insn = I; dag Body = (rv_frmw "neg", $src1, (rv_imm 0)); }
foreach I = [NOT8r, NOT16r, NOT32r, NOT64r] in
  def : RVLift { let Insn = I; dag Body = (rv_set $dst, (rv_not $src1)); }
foreach I = [NOT8m, NOT16m, NOT32m, NOT64m] in
  def : RVLift { let Insn = I; dag Body = (rv_frmw "not", $src1, (rv_imm 0)); }

// ---- shifts and rotates ---------------------------------------------------------
// Count is $src2 for ri/rCL forms, a literal 1 for r1 forms. Memory forms
// write $src1 back.

foreach I = [SHL8ri, SHL16ri, SHL32ri, SHL64ri,
             SHL8rCL, SHL16rCL, SHL32rCL, SHL64rCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "shl", $src1, $src2));
  }
foreach I = [SHL8r1, SHL16r1, SHL32r1, SHL64r1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "shl", $src1, 1));
  }
foreach I = [SHL8mi, SHL16mi, SHL32mi, SHL64mi,
             SHL8mCL, SHL16mCL, SHL32mCL, SHL64mCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift "shl", $src1, $src2));
  }
foreach I = [SHL8m1, SHL16m1, SHL32m1, SHL64m1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift "shl", $src1, 1));
  }
foreach I = [SHR8ri, SHR16ri, SHR32ri, SHR64ri,
             SHR8rCL, SHR16rCL, SHR32rCL, SHR64rCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "shr", $src1, $src2));
  }
foreach I = [SHR8r1, SHR16r1, SHR32r1, SHR64r1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "shr", $src1, 1));
  }
foreach I = [SHR8mi, SHR16mi, SHR32mi, SHR64mi,
             SHR8mCL, SHR16mCL, SHR32mCL, SHR64mCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift "shr", $src1, $src2));
  }
foreach I = [SHR8m1, SHR16m1, SHR32m1, SHR64m1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift "shr", $src1, 1));
  }
foreach I = [SAR8ri, SAR16ri, SAR32ri, SAR64ri,
             SAR8rCL, SAR16rCL, SAR32rCL, SAR64rCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "sar", $src1, $src2));
  }
foreach I = [SAR8r1, SAR16r1, SAR32r1, SAR64r1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "sar", $src1, 1));
  }
foreach I = [SAR8mi, SAR16mi, SAR32mi, SAR64mi,
             SAR8mCL, SAR16mCL, SAR32mCL, SAR64mCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift "sar", $src1, $src2));
  }
foreach I = [SAR8m1, SAR16m1, SAR32m1, SAR64m1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift "sar", $src1, 1));
  }
foreach I = [ROL8ri, ROL16ri, ROL32ri, ROL64ri,
             ROL8rCL, ROL16rCL, ROL32rCL, ROL64rCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "rol", $src1, $src2));
  }
foreach I = [ROL8r1, ROL16r1, ROL32r1, ROL64r1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "rol", $src1, 1));
  }
foreach I = [ROR8ri, ROR16ri, ROR32ri, ROR64ri,
             ROR8rCL, ROR16rCL, ROR32rCL, ROR64rCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "ror", $src1, $src2));
  }
foreach I = [ROR8r1, ROR16r1, ROR32r1, ROR64r1] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift "ror", $src1, 1));
  }
// RCL/RCR through carry: opaque until needed (rare in corpus).
def : RVMatch { let Glob = "RCL*"; dag Body = (rv_opaque); }
def : RVMatch { let Glob = "RCR*"; dag Body = (rv_opaque); }
// Double shifts.
foreach I = [SHLD16rri8, SHLD32rri8, SHLD64rri8,
             SHLD16rrCL, SHLD32rrCL, SHLD64rrCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift2 "shld", $src1, $src2, $src3));
  }
foreach I = [SHRD16rri8, SHRD32rri8, SHRD64rri8,
             SHRD16rrCL, SHRD32rrCL, SHRD64rrCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fshift2 "shrd", $src1, $src2, $src3));
  }
foreach I = [SHLD16mri8, SHLD32mri8, SHLD64mri8,
             SHLD16mrCL, SHLD32mrCL, SHLD64mrCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift2 "shld", $src1, $src2, $src3));
  }
foreach I = [SHRD16mri8, SHRD32mri8, SHRD64mri8,
             SHRD16mrCL, SHRD32mrCL, SHRD64mrCL] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_fshift2 "shrd", $src1, $src2, $src3));
  }
// BMI2 shifts (no flags). Count operand is masked by the emitter-side
// semantics; here a plain shift suffices since over-width counts are
// meaningless on BMI forms anyway.
foreach I = [SHLX32rr, SHLX64rr] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_shl $src1, (rv_and $src2, 63)));
  }
foreach I = [SHRX32rr, SHRX64rr] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_lshr $src1, (rv_and $src2, 63)));
  }
foreach I = [SARX32rr, SARX64rr] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_ashr $src1, (rv_and $src2, 63)));
  }
// RORX: rotate right, no flags.
foreach I = [RORX32ri, RORX64ri, RORX32mi, RORX64mi] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_rorx $src, $src2));
  }

// ---- mov / lea / extensions -----------------------------------------------------

// Plain moves. $dst/$src names cover rr/rm/mr/mi/ri forms; the GPR<->XMM
// element moves (MOV*toPQIrr, MOVDI2PDIrr, ...) are also plain copies through
// the root-register slots.
def : RVMatch { let Glob = "MOV*rr";     dag Body = (rv_set $dst, $src); }
def : RVMatch { let Glob = "MOV*rr_REV"; dag Body = (rv_set $dst, $src); }
def : RVMatch { let Glob = "MOV*rm";     dag Body = (rv_set $dst, $src); }
def : RVMatch { let Glob = "MOV*mr";     dag Body = (rv_set $dst, $src); }
def : RVMatch { let Glob = "MOV*mi";     dag Body = (rv_set $dst, $src); }
def : RVMatch { let Glob = "MOV*ri";     dag Body = (rv_set $dst, $src); }
def : RVMatch { let Glob = "MOV*ri32";   dag Body = (rv_set $dst, $src); }
// Absolute-offset (moffs) forms: load into the accumulator / store from it.
def : RVMatch { let Glob = "MOV*ao*"; dag Body = (rv_set $dst, (rv_load 0, $src)); }
def : RVMatch { let Glob = "MOV*o*a"; dag Body = (rv_store $dst, $src); }
// Byte-swap moves.
foreach I = [MOVBE16rm, MOVBE32rm, MOVBE64rm,
             MOVBE16mr, MOVBE32mr, MOVBE64mr] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_bswap $src));
  }
// Zero/sign-extending moves.
def : RVMatch { let Glob = "MOVZX*";  dag Body = (rv_set $dst, (rv_zext $src)); }
def : RVMatch { let Glob = "MOVSX*";  dag Body = (rv_set $dst, (rv_sext $src)); }
def : RVMatch { let Glob = "MOVSXD*"; dag Body = (rv_set $dst, (rv_sext $src)); }

// LEA: the effective address itself.
foreach I = [LEA16r, LEA32r, LEA64r, LEA64_32r] in
  def : RVLift { let Insn = I; dag Body = (rv_set $dst, (rv_addr $src)); }

// ---- stack ------------------------------------------------------------------------

// Stack operand width: 16-bit pushes move 2 bytes, everything else in
// 64-bit mode moves 8.
foreach I = [PUSH64r, PUSH64rmr] in
  def : RVLift { let Insn = I; dag Body = (rv_push 64, $reg); }
foreach I = [PUSH64rmm] in
  def : RVLift { let Insn = I; dag Body = (rv_push 64, $src); }
foreach I = [PUSH64i8, PUSH64i32] in
  def : RVLift { let Insn = I; dag Body = (rv_push 64, $imm); }
foreach I = [PUSH16r, PUSH16rmr] in
  def : RVLift { let Insn = I; dag Body = (rv_push 16, $reg); }
def : RVLift { let Insn = PUSH16rmm; dag Body = (rv_push 16, $src); }
foreach I = [PUSH16i8, PUSH16i] in
  def : RVLift { let Insn = I; dag Body = (rv_push 16, $imm); }
foreach I = [PUSH32r, PUSH32rmr] in
  def : RVLift { let Insn = I; dag Body = (rv_push 64, $reg); }
def : RVLift { let Insn = PUSH32rmm; dag Body = (rv_push 64, $src); }
foreach I = [PUSH32i8, PUSH32i] in
  def : RVLift { let Insn = I; dag Body = (rv_push 64, $imm); }
foreach I = [POP64r, POP64rmr] in
  def : RVLift { let Insn = I; dag Body = (rv_set $reg, (rv_pop 64)); }
def : RVLift { let Insn = POP64rmm; dag Body = (rv_set $dst, (rv_pop 64)); }
foreach I = [POP16r, POP16rmr] in
  def : RVLift { let Insn = I; dag Body = (rv_set $reg, (rv_pop 16)); }
def : RVLift { let Insn = POP16rmm; dag Body = (rv_set $dst, (rv_pop 16)); }
def : RVLift { let Insn = PUSHF64; dag Body = (rv_pushf); }
def : RVLift { let Insn = POPF64;  dag Body = (rv_popf); }
def : RVLift { let Insn = PUSHF32; dag Body = (rv_pushf); }
def : RVLift { let Insn = POPF32;  dag Body = (rv_popf); }
def : RVLift { let Insn = PUSHF16; dag Body = (rv_pushf); }
def : RVLift { let Insn = POPF16;  dag Body = (rv_popf); }
def : RVLift { let Insn = LEAVE;   dag Body = (rv_leave); }
def : RVLift { let Insn = LEAVE64; dag Body = (rv_leave); }

// ---- control flow -----------------------------------------------------------------

foreach I = [JMP_1, JMP_2, JMP_4] in
  def : RVLift { let Insn = I; dag Body = (rv_br $dst); }
foreach I = [JCC_1, JCC_2, JCC_4] in
  def : RVLift { let Insn = I; dag Body = (rv_brcc $dst, $src); }
foreach I = [JMP64r, JMP32r, JMP64m, JMP32m] in
  def : RVLift { let Insn = I; dag Body = (rv_br $src); }
foreach I = [CALL64pcrel32, CALLpcrel32, CALLpcrel16] in
  def : RVLift { let Insn = I; dag Body = (rv_calli $dst); }
foreach I = [CALL64r, CALL32r, CALL64m, CALL32m] in
  def : RVLift { let Insn = I; dag Body = (rv_callr $src); }
foreach I = [RET, RET64] in
  def : RVLift { let Insn = I; dag Body = (rv_ret); }
// Far/interrupt returns pop more state; opaque for now.
def : RVMatch { let Glob = "RETI*";  dag Body = (rv_opaque); }
def : RVMatch { let Glob = "LRET*";  dag Body = (rv_opaque); }
def : RVMatch { let Glob = "IRET*";  dag Body = (rv_opaque); }
// Tail-call pseudos never reach MCInst streams; opaque to be safe.
def : RVMatch { let Glob = "TAILJMP*";   dag Body = (rv_opaque); }
def : RVMatch { let Glob = "TCRETURN*";  dag Body = (rv_opaque); }
// LOOP family: decrement RCX/ECX/CX, branch on count (and ZF for E/NE).
def : RVLift {
  let Insn = LOOP;
  dag Body = (rv_seq
    (rv_set (rv_reg "RCX"), (rv_sub (rv_reg "RCX"), 1)),
    (rv_brcond $dst, (rv_icmp "ne", (rv_reg "RCX"), 0)));
}
def : RVLift {
  let Insn = LOOPE;
  dag Body = (rv_seq
    (rv_set (rv_reg "RCX"), (rv_sub (rv_reg "RCX"), 1)),
    (rv_brcond $dst,
      (rv_and (rv_flagread "zf"),
               (rv_icmp "ne", (rv_reg "RCX"), 0))));
}
def : RVLift {
  let Insn = LOOPNE;
  dag Body = (rv_seq
    (rv_set (rv_reg "RCX"), (rv_sub (rv_reg "RCX"), 1)),
    (rv_brcond $dst,
      (rv_and (rv_not (rv_flagread "zf")),
               (rv_icmp "ne", (rv_reg "RCX"), 0))));
}
def : RVLift { let Insn = JRCXZ; dag Body = (rv_brcond $dst, (rv_icmp "eq", (rv_reg "RCX"), 0)); }
def : RVLift { let Insn = JECXZ; dag Body = (rv_brcond $dst, (rv_icmp "eq", (rv_reg "ECX"), 0)); }
def : RVLift { let Insn = JCXZ;  dag Body = (rv_brcond $dst, (rv_icmp "eq", (rv_reg "CX"), 0)); }

// ---- setcc / cmovcc ------------------------------------------------------------------

// SETCCr / SETCCm: (outs i8:$dst), (ins i8imm:$cond).
def : RVMatch { let Glob = "SET*"; dag Body = (rv_set $dst, (rv_setcc $cond)); }
// CMOVcc: (outs t:$dst), (ins t:$src1, t:$src2, i8imm:$cond).
def : RVMatch {
  let Glob = "CMOV*";
  dag Body = (rv_set $dst, (rv_select (rv_cpred $cond), $src2, $src1));
}

// ---- flag carry / image ----------------------------------------------------------

def : RVLift { let Insn = LAHF; dag Body = (rv_set (rv_reg "AH"), (rv_lahf)); }
def : RVLift { let Insn = SAHF; dag Body = (rv_sahf); }
def : RVLift { let Insn = CLC; dag Body = (rv_flagset "cf", 0); }
def : RVLift { let Insn = STC; dag Body = (rv_flagset "cf", 1); }
def : RVLift { let Insn = CMC; dag Body = (rv_flagset "cf", (rv_xor (rv_flagread "cf"), 1)); }
def : RVLift { let Insn = CLD; dag Body = (rv_flagset "df", 0); }
def : RVLift { let Insn = STD; dag Body = (rv_flagset "df", 1); }

// ---- mul/div families --------------------------------------------------------------

// 2/3-operand IMUL: signed low product + flags.
foreach I = [IMUL16rr, IMUL32rr, IMUL64rr,
             IMUL16rri, IMUL32rri, IMUL64rri32,
             IMUL16rri8, IMUL32rri8, IMUL64rri8,
             IMUL16rm, IMUL32rm, IMUL64rm,
             IMUL16rmi, IMUL32rmi, IMUL64rmi32,
             IMUL16rmi8, IMUL32rmi8, IMUL64rmi8] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_fmul $src1, $src2));
  }
// 1-operand forms write the accumulator pair.
foreach I = [IMUL8r, IMUL8m]   in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "s", 8, $src1); }
foreach I = [IMUL16r, IMUL16m] in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "s", 16, $src1); }
foreach I = [IMUL32r, IMUL32m] in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "s", 32, $src1); }
foreach I = [IMUL64r, IMUL64m] in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "s", 64, $src1); }
foreach I = [MUL8r, MUL8m]   in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "u", 8, $src1); }
foreach I = [MUL16r, MUL16m] in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "u", 16, $src1); }
foreach I = [MUL32r, MUL32m] in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "u", 32, $src1); }
foreach I = [MUL64r, MUL64m] in
  def : RVLift { let Insn = I; dag Body = (rv_mulpair "u", 64, $src1); }
foreach I = [DIV8r, DIV8m]   in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "u", 8, $src1); }
foreach I = [DIV16r, DIV16m] in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "u", 16, $src1); }
foreach I = [DIV32r, DIV32m] in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "u", 32, $src1); }
foreach I = [DIV64r, DIV64m] in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "u", 64, $src1); }
foreach I = [IDIV8r, IDIV8m]   in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "s", 8, $src1); }
foreach I = [IDIV16r, IDIV16m] in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "s", 16, $src1); }
foreach I = [IDIV32r, IDIV32m] in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "s", 32, $src1); }
foreach I = [IDIV64r, IDIV64m] in
  def : RVLift { let Insn = I; dag Body = (rv_divpair "s", 64, $src1); }
// Accumulator extends.
def : RVLift { let Insn = CBW;  dag Body = (rv_sextacc 16); }
def : RVLift { let Insn = CWDE; dag Body = (rv_sextacc 32); }
def : RVLift { let Insn = CDQE; dag Body = (rv_sextacc 64); }
def : RVLift { let Insn = CWD;  dag Body = (rv_sextacc 116); }
def : RVLift { let Insn = CDQ;  dag Body = (rv_sextacc 132); }
def : RVLift { let Insn = CQO;  dag Body = (rv_sextacc 164); }

// ---- bit ops ---------------------------------------------------------------------------

foreach I = [BT16rr, BT32rr, BT64rr,
             BT16ri8, BT32ri8, BT64ri8,
             BT16mr, BT32mr, BT64mr,
             BT16mi8, BT32mi8, BT64mi8] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_bt "bt", $src1, $src2);
  }
foreach I = [BTS16rr, BTS32rr, BTS64rr,
             BTS16ri8, BTS32ri8, BTS64ri8,
             BTS16mr, BTS32mr, BTS64mr,
             BTS16mi8, BTS32mi8, BTS64mi8] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_bt "bts", $src1, $src2));
  }
foreach I = [BTR16rr, BTR32rr, BTR64rr,
             BTR16ri8, BTR32ri8, BTR64ri8,
             BTR16mr, BTR32mr, BTR64mr,
             BTR16mi8, BTR32mi8, BTR64mi8] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_bt "btr", $src1, $src2));
  }
foreach I = [BTC16rr, BTC32rr, BTC64rr,
             BTC16ri8, BTC32ri8, BTC64ri8,
             BTC16mr, BTC32mr, BTC64mr,
             BTC16mi8, BTC32mi8, BTC64mi8] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $src1, (rv_bt "btc", $src1, $src2));
  }
foreach I = [BSF16rr, BSF32rr, BSF64rr,
             BSF16rm, BSF32rm, BSF64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_bsf $src));
  }
foreach I = [BSR16rr, BSR32rr, BSR64rr,
             BSR16rm, BSR32rm, BSR64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_bsr $src));
  }
foreach I = [TZCNT16rr, TZCNT32rr, TZCNT64rr,
             TZCNT16rm, TZCNT32rm, TZCNT64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_tzcnt $src));
  }
foreach I = [LZCNT16rr, LZCNT32rr, LZCNT64rr,
             LZCNT16rm, LZCNT32rm, LZCNT64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_lzcnt $src));
  }
foreach I = [POPCNT16rr, POPCNT32rr, POPCNT64rr,
             POPCNT16rm, POPCNT32rm, POPCNT64rm] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set $dst, (rv_popcnt $src));
  }
foreach I = [BSWAP32r, BSWAP64r] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_set (rv_out 0), (rv_bswap (rv_op 0)));
  }

// ---- exchange / cmpxchg (reg forms) -----------------------------------------------------
// Memory forms are owned by StringAtomic.td (atomicrmw spellings).

foreach I = [XCHG8rr, XCHG16rr, XCHG32rr, XCHG64rr] in
  def : RVLift { let Insn = I; dag Body = (rv_xchg $src1, $src2); }
// XCHG*ar: the accumulator side is implicit.
def : RVLift { let Insn = XCHG16ar; dag Body = (rv_xchg $src, (rv_reg "AX")); }
def : RVLift { let Insn = XCHG32ar; dag Body = (rv_xchg $src, (rv_reg "EAX")); }
def : RVLift { let Insn = XCHG64ar; dag Body = (rv_xchg $src, (rv_reg "RAX")); }
foreach I = [XADD8rr, XADD16rr, XADD32rr, XADD64rr] in
  def : RVLift { let Insn = I; dag Body = (rv_xadd $dst, $src); }
// CMPXCHG rr: accumulator vs dst reg, $src is the new value.
foreach I = [CMPXCHG8rr, CMPXCHG16rr, CMPXCHG32rr, CMPXCHG64rr] in
  def : RVLift {
    let Insn = I;
    dag Body = (rv_cmpxchgr $dst, $src);
  }

// ---- prefixes (separate MCInsts) -----------------------------------------------------------

def : RVLift { let Insn = LOCK_PREFIX;  dag Body = (rv_prefix "lock"); }
def : RVLift { let Insn = REP_PREFIX;   dag Body = (rv_prefix "rep"); }
def : RVLift { let Insn = REPNE_PREFIX; dag Body = (rv_prefix "repne"); }
def : RVLift { let Insn = XACQUIRE_PREFIX; dag Body = (rv_prefix "repne"); }
def : RVLift { let Insn = XRELEASE_PREFIX; dag Body = (rv_prefix "rep"); }
def : RVLift { let Insn = REX64_PREFIX; dag Body = (rv_nop); }
def : RVLift { let Insn = CS_PREFIX; dag Body = (rv_nop); }
def : RVLift { let Insn = DS_PREFIX; dag Body = (rv_nop); }
def : RVLift { let Insn = ES_PREFIX; dag Body = (rv_nop); }
def : RVLift { let Insn = SS_PREFIX; dag Body = (rv_nop); }
def : RVLift { let Insn = FS_PREFIX; dag Body = (rv_prefix "seg", (rv_reg "FS")); }
def : RVLift { let Insn = GS_PREFIX; dag Body = (rv_prefix "seg", (rv_reg "GS")); }

// ---- no-ops / hints --------------------------------------------------------------------------

foreach I = [NOOP, NOOPL, NOOPW] in
  def : RVLift { let Insn = I; dag Body = (rv_nop); }
def : RVMatch { let Glob = "PAUSE*";    dag Body = (rv_nop); }
def : RVMatch { let Glob = "PREFETCH*"; dag Body = (rv_nop); }
def : RVMatch { let Glob = "LFENCE*";   dag Body = (rv_nop); }
def : RVMatch { let Glob = "SFENCE*";   dag Body = (rv_nop); }
def : RVMatch { let Glob = "MFENCE*";   dag Body = (rv_fence); }
// ENDBR landing pads have no data effect.
def : RVMatch { let Glob = "ENDBR*"; dag Body = (rv_nop); }
def : RVLift { let Insn = INT3; dag Body = (rv_nop); }
def : RVLift { let Insn = VZEROUPPER; dag Body = (rv_nop); }
def : RVLift { let Insn = VZEROALL;   dag Body = (rv_nop); }
// Traps and serializing/system instructions stay opaque escapes.
def : RVLift { let Insn = TRAP; dag Body = (rv_opaque); }
def : RVLift { let Insn = FNINIT; dag Body = (rv_x87init); }
