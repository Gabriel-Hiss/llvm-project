//===- Sse.td - SSE/SSE2/SSE4.1/SSE4.2/SSSE3 lift semantics -------------===//
//
// Lane moves, scalar float ops on lane 0, cvt*, shuffles/unpack/pshufb,
// packed integer ops, compare masks, movmsk, ptest, pblendvb, pcmpe stri.
// Ports crates/lift/src/sse.rs and packed.rs (h-merge reference).
//
// Conventions (per Swe2LiftGen's frozen hook list):
// - Vector regs are i128 values in the DSL. rv_op N reads the Nth declared
//   operand (outs first); use positional form everywhere except $dst, whose
//   name is stable across every def here.
// - Legacy SSE forms merge into the destination (rv_set); VEX 128-bit forms
//   zero the upper lanes of the root (rv_setz).
// - Scalar float ops work on lane 0 only: rv_xlane0f32/f64 extracts it,
//   rv_xsetlane0f* merges a new lane 0 over the old upper lanes (legacy
//   reg-reg, VEX merge), rv_xnewlane0f* builds a fresh vector with undef
//   upper lanes (memory loads, matching the reference).
// - rv_fcmpf and rv_ptest are flags-only and store ZF/PF/CF eagerly;
//   rv_cpred reads the flag slots directly.
// - RVMatch globs cover _ND/_EVEX/_NF twins of the same base name; exact
//   RVLift entries are used where operand names or arity differ.
//
//===----------------------------------------------------------------------===//

// DSL operators (emitted by -gen-revenant-lift; implemented on
// X86LiftContext::vop or as direct IRBuilder calls).
class RVOp;
def rv_setz         : RVOp; // (rv_setz $dst, val): write + zero upper root bits (VEX)
def rv_xlane0f32    : RVOp; // (rv_xlane0f32 x_i128): float lane 0
def rv_xlane0f64    : RVOp; // (rv_xlane0f64 x_i128): double lane 0
def rv_xsetlane0f32 : RVOp; // (rv_xsetlane0f32 x_i128, f): keep upper lanes
def rv_xsetlane0f64 : RVOp; // (rv_xsetlane0f64 x_i128, f): keep upper lanes
def rv_xnewlane0f32 : RVOp; // (rv_xnewlane0f32 f): fresh vector, undef upper
def rv_xnewlane0f64 : RVOp; // (rv_xnewlane0f64 f): fresh vector, undef upper
def rv_floadf32     : RVOp; // (rv_floadf32 addr_i64): float load
def rv_floadf64     : RVOp; // (rv_floadf64 addr_i64): double load
def rv_ffadd        : RVOp; // float add (type-polymorphic)
def rv_ffsub        : RVOp; // float sub
def rv_ffmul        : RVOp; // float mul
def rv_ffdiv        : RVOp; // float div
def rv_fminnum      : RVOp; // llvm.minnum
def rv_fmaxnum      : RVOp; // llvm.maxnum
def rv_fsqrt        : RVOp; // llvm.sqrt
def rv_sitofpf32    : RVOp; // (rv_sitofpf32 int): any width -> float
def rv_sitofpf64    : RVOp; // (rv_sitofpf64 int): any width -> double
def rv_fptosi       : RVOp; // (rv_fptosi W, float): float -> iW
def rv_fpext        : RVOp; // float -> double (dest width from context)
def rv_fptrunc      : RVOp; // double -> float
def rv_fcmpf        : RVOp; // (rv_fcmpf fa, fb): flags-only FP compare
def rv_pshufd       : RVOp; // (rv_pshufd x, imm)
def rv_pshufhw      : RVOp; // (rv_pshufhw x, imm)
def rv_pshuflw      : RVOp; // (rv_pshuflw x, imm)
def rv_shufps       : RVOp; // (rv_shufps a, b, imm)
def rv_shufpd       : RVOp; // (rv_shufpd a, b, imm)
def rv_punpckl      : RVOp; // (rv_punpckl ebits, a, b): interleave low half
def rv_punpckh      : RVOp; // (rv_punpckh ebits, a, b): interleave high half
def rv_pshufb       : RVOp; // (rv_pshufb a, b): SSSE3 byte shuffle
def rv_pinsrw       : RVOp; // (rv_pinsrw x, val, imm)
def rv_vshift       : RVOp; // (rv_vshift "psrl"|"psll"|"psra", ebits, vec, cnt)
def rv_pslldq       : RVOp; // (rv_pslldq x, imm): whole-register shift left
def rv_psrldq       : RVOp; // (rv_psrldq x, imm): whole-register shift right
def rv_pmovmskb     : RVOp; // (rv_pmovmskb x): i128 -> i32 sign mask
def rv_movmskps     : RVOp; // (rv_movmskps x): i128 -> i32
def rv_movmskpd     : RVOp; // (rv_movmskpd x): i128 -> i32
def rv_ptest        : RVOp; // (rv_ptest a, b): flags-only
def rv_vbroadcastb  : RVOp; // (rv_vbroadcastb val): low byte splat -> i128
def rv_vcvtdq2pd    : RVOp; // (rv_vcvtdq2pd x): low 2xi32 -> 2xf64 as i128
def rv_pcmpextest   : RVOp; // (rv_pcmpextest kind, x, m, imm): pcmpestr flags-only
def rv_pcmpistest   : RVOp; // (rv_pcmpistest kind, x, m, imm): pcmpistr flags-only

// ---- unaligned/alignment-insensitive full-vector moves ----------------------
// The aligned forms (MOVAPS/MOVAPD/MOVDQA) come from pattern inversion for
// free; the unaligned/integer-domain twins have no direct patterns, so they
// are spelled out here. All are plain 128-bit copies.
foreach I = [MOVDQUrm, MOVDQArm] in
def : RVLift { let Insn = I; dag Body = (rv_set $dst, (rv_op 0)); }
foreach I = [MOVDQUmr, MOVDQAmr] in
def : RVLift { let Insn = I; dag Body = (rv_store (rv_addr $dst), (rv_op 1)); }
def : RVMatch { string Glob = "MOVDQArr"; dag Body = (rv_set $dst, (rv_op 0)); }
def : RVMatch { string Glob = "MOVAPSrr"; dag Body = (rv_set $dst, (rv_op 0)); }
def : RVMatch { string Glob = "MOVAPDrr"; dag Body = (rv_set $dst, (rv_op 0)); }
def : RVMatch { string Glob = "MOVUPSrr"; dag Body = (rv_set $dst, (rv_op 0)); }
def : RVMatch { string Glob = "VMOVDQUrr"; dag Body = (rv_setz $dst, (rv_op 0)); }
def : RVMatch { string Glob = "VMOVDQArr*"; dag Body = (rv_setz $dst, (rv_op 0)); }
def : RVMatch { string Glob = "VMOVAPSrr"; dag Body = (rv_setz $dst, (rv_op 0)); }
def : RVMatch { string Glob = "VMOVAPDrr*"; dag Body = (rv_setz $dst, (rv_op 0)); }
def : RVMatch { string Glob = "VMOVUPSrr"; dag Body = (rv_setz $dst, (rv_op 0)); }
def : RVMatch { string Glob = "VMOVDQUYrr"; dag Body = (rv_nop); }
def : RVMatch { string Glob = "VMOVDQAYm*"; dag Body = (rv_nop); }

// ---- movd/movq lane moves ---------------------------------------------------
// movd/movq xmm<->gpr/mem: zero-extend into the xmm, truncate out of it.
def : RVMatch {
  string Glob = "MOVDI2PDIrr";
  dag Body = (rv_set $dst, (rv_zext 128, (rv_op 0)));
}
def : RVMatch {
  string Glob = "VMOVDI2PDIrr";
  dag Body = (rv_setz $dst, (rv_zext 128, (rv_op 1)));
}
def : RVMatch {
  string Glob = "MOVDI2PDIrm";
  dag Body = (rv_set $dst, (rv_zext 128, (rv_load 32, (rv_addr $src))));
}
def : RVMatch {
  string Glob = "VMOVDI2PDIrm";
  dag Body = (rv_setz $dst, (rv_zext 128, (rv_load 32, (rv_addr $src))));
}
def : RVMatch {
  string Glob = "MOV64toPQIrr";
  dag Body = (rv_set $dst, (rv_zext 128, (rv_op 0)));
}
def : RVMatch {
  string Glob = "VMOV64toPQIrr";
  dag Body = (rv_setz $dst, (rv_zext 128, (rv_op 1)));
}
def : RVMatch {
  string Glob = "MOVQI2PQIrm";
  dag Body = (rv_set $dst, (rv_zext 128, (rv_load 64, (rv_addr $src))));
}
def : RVMatch {
  string Glob = "VMOVQI2PQIrm";
  dag Body = (rv_setz $dst, (rv_zext 128, (rv_load 64, (rv_addr $src))));
}
def : RVMatch {
  string Glob = "MOVPQIto64rr";
  dag Body = (rv_set $dst, (rv_trunc 64, (rv_op 0)));
}
def : RVMatch {
  string Glob = "VMOVPQIto64rr";
  dag Body = (rv_set $dst, (rv_trunc 64, (rv_op 1)));
}
// movd/movq xmm -> mem truncates the low lane.
def : RVMatch {
  string Glob = "MOVPDI2DImr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 32, (rv_op 1)));
}
def : RVMatch {
  string Glob = "MOVPQIto64mr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 64, (rv_op 1)));
}
def : RVMatch {
  string Glob = "VMOVPDI2DImr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 32, (rv_op 1)));
}
def : RVMatch {
  string Glob = "VMOVPQIto64mr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 64, (rv_op 1)));
}

// ---- scalar float moves (lane 0, keep/zero upper) ---------------------------
// Legacy reg-reg: keep upper lanes. Memory loads: fresh lane 0 (reference
// leaves the upper lanes undef). Memory stores: lane 0 only.
def : RVMatch {
  string Glob = "MOVSSrr";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0), (rv_xlane0f32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "MOVSDrr";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0), (rv_xlane0f64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "MOVSSrm";
  dag Body = (rv_set $dst, (rv_xnewlane0f32 (rv_floadf32 (rv_addr $src))));
}
def : RVMatch {
  string Glob = "MOVSDrm";
  dag Body = (rv_set $dst, (rv_xnewlane0f64 (rv_floadf64 (rv_addr $src))));
}
def : RVMatch {
  string Glob = "MOVSSmr";
  dag Body = (rv_store (rv_addr $dst), (rv_xlane0f32 (rv_op 1)));
}
def : RVMatch {
  string Glob = "MOVSDmr";
  dag Body = (rv_store (rv_addr $dst), (rv_xlane0f64 (rv_op 1)));
}
// VEX blends: dst = src1 with lane 0 from src2 (reg or memory).
def : RVMatch {
  string Glob = "VMOVSSr*";
  dag Body = (rv_setz $dst, (rv_xsetlane0f32 (rv_op 0), (rv_xlane0f32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "VMOVSDr*";
  dag Body = (rv_setz $dst, (rv_xsetlane0f64 (rv_op 0), (rv_xlane0f64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "VMOVSSrm";
  dag Body = (rv_setz $dst, (rv_xnewlane0f32 (rv_floadf32 (rv_addr $src))));
}
def : RVMatch {
  string Glob = "VMOVSDrm";
  dag Body = (rv_setz $dst, (rv_xnewlane0f64 (rv_floadf64 (rv_addr $src))));
}
def : RVMatch {
  string Glob = "VMOVSSmr";
  dag Body = (rv_store (rv_addr $dst), (rv_xlane0f32 (rv_op 1)));
}
def : RVMatch {
  string Glob = "VMOVSDmr";
  dag Body = (rv_store (rv_addr $dst), (rv_xlane0f64 (rv_op 1)));
}
// Partial low/high double moves keep the other lane.
def : RVMatch {
  string Glob = "MOVLPDrm";
  dag Body = (rv_set $dst, (rv_or (rv_and (rv_op 0), (rv_imm 0xFFFFFFFF00000000)),
                                     (rv_zext 128, (rv_load 64, (rv_addr $src)))));
}
def : RVMatch {
  string Glob = "MOVHPDrm";
  dag Body = (rv_set $dst, (rv_or (rv_and (rv_op 0), (rv_imm 0xFFFFFFFF)),
                                     (rv_shl (rv_zext 128, (rv_load 64, (rv_addr $src))), (rv_imm 64))));
}
def : RVMatch {
  string Glob = "MOVLPDmr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 64, (rv_op 1)));
}
def : RVMatch {
  string Glob = "MOVHPDmr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 64, (rv_lshr (rv_op 1), (rv_imm 64))));
}
def : RVMatch {
  string Glob = "VMOVLPSmr";
  dag Body = (rv_store (rv_addr $dst), (rv_trunc 64, (rv_op 1)));
}
def : RVMatch {
  string Glob = "MOVDDUPrm";
  dag Body = (rv_set $dst, (rv_or (rv_zext 128, (rv_load 64, (rv_addr $src))),
                                     (rv_shl (rv_zext 128, (rv_load 64, (rv_addr $src))), (rv_imm 64))));
}
def : RVMatch {
  string Glob = "MOVDDUPrr";
  dag Body = (rv_set $dst, (rv_or (rv_zext 128, (rv_trunc 64, (rv_op 0))),
                                     (rv_shl (rv_zext 128, (rv_trunc 64, (rv_op 0))), (rv_imm 64))));
}

// ---- scalar float arithmetic (lane 0) ---------------------------------------
// Legacy: dst lane 0 = dst lane 0 OP src (reg lane 0 or memory scalar),
// upper lanes kept. VEX: dst = src1 with lane 0 replaced.
def : RVMatch {
  string Glob = "ADDSSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffadd (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "ADDSDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffadd (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "SUBSSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffsub (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "SUBSDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffsub (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "MULSSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffmul (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "MULSDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffmul (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "DIVSSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffdiv (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "DIVSDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffdiv (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "MINSSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fminnum (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "MINSDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fminnum (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "MAXSSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fmaxnum (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "MAXSDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fmaxnum (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "SQRTSSr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fsqrt (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "SQRTSDr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fsqrt (rv_xlane0f64 (rv_op 1)))));
}
// Memory RHS forms: scalar loaded from the address operand.
def : RVMatch {
  string Glob = "ADDSSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffadd (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "ADDSDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffadd (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "SUBSSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffsub (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "SUBSDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffsub (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "MULSSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffmul (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "MULSDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffmul (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "DIVSSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_ffdiv (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "DIVSDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_ffdiv (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "MINSSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fminnum (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "MINSDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fminnum (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "MAXSSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fmaxnum (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "MAXSDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fmaxnum (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "SQRTSSm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fsqrt (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "SQRTSDm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fsqrt (rv_floadf64 (rv_addr $src2)))));
}

// ---- scalar float conversions -----------------------------------------------
def : RVMatch {
  string Glob = "CVTSI2SSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0), (rv_sitofpf32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSI642SSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0), (rv_sitofpf32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSI2SDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0), (rv_sitofpf64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSI642SDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0), (rv_sitofpf64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSI2SSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_sitofpf32 (rv_load 32, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "CVTSI642SSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_sitofpf32 (rv_load 64, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "CVTSI2SDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_sitofpf64 (rv_load 32, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "CVTSI642SDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_sitofpf64 (rv_load 64, (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "CVTTSS2SIrr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTTSS2SI64rr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTTSD2SIrr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTTSD2SI64rr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSS2SDrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fpext (rv_xlane0f32 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "CVTSD2SSrr_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fptrunc (rv_xlane0f64 (rv_op 1)))));
}
def : RVMatch {
  string Glob = "CVTSS2SDrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f64 (rv_op 0),
    (rv_fpext (rv_floadf32 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "CVTSD2SSrm_Int";
  dag Body = (rv_set $dst, (rv_xsetlane0f32 (rv_op 0),
    (rv_fptrunc (rv_floadf64 (rv_addr $src2)))));
}
def : RVMatch {
  string Glob = "CVTSD2SIrr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSD2SI64rr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f64 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSS2SIrr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 32, (rv_xlane0f32 (rv_op 1))));
}
def : RVMatch {
  string Glob = "CVTSS2SI64rr_Int";
  dag Body = (rv_set $dst, (rv_fptosi 64, (rv_xlane0f32 (rv_op 1))));
}

// ---- scalar float compares (flags only) --------------------------------------
def : RVMatch {
  string Glob = "COMISSrr";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)));
}
def : RVMatch {
  string Glob = "COMISDrr";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)));
}
def : RVMatch {
  string Glob = "UCOMISSrr";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_xlane0f32 (rv_op 1)));
}
def : RVMatch {
  string Glob = "UCOMISDrr";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_xlane0f64 (rv_op 1)));
}
def : RVMatch {
  string Glob = "COMISSrm";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src)));
}
def : RVMatch {
  string Glob = "COMISDrm";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src)));
}
def : RVMatch {
  string Glob = "UCOMISSrm";
  dag Body = (rv_fcmpf (rv_xlane0f32 (rv_op 0)), (rv_floadf32 (rv_addr $src)));
}
def : RVMatch {
  string Glob = "UCOMISDrm";
  dag Body = (rv_fcmpf (rv_xlane0f64 (rv_op 0)), (rv_floadf64 (rv_addr $src)));
}

// ---- packed FP logic on the whole 128-bit value ------------------------------
// The PS forms come from patterns; the PD twins and memory folds do not.
def : RVMatch {
  string Glob = "ANDPDrr";
  dag Body = (rv_set $dst, (rv_and (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "ANDPDrm";
  dag Body = (rv_set $dst, (rv_and (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "ORPDrr";
  dag Body = (rv_set $dst, (rv_or (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "ORPDrm";
  dag Body = (rv_set $dst, (rv_or (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "XORPDrr";
  dag Body = (rv_set $dst, (rv_xor (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "XORPDrm";
  dag Body = (rv_set $dst, (rv_xor (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "ANDNPDrr";
  dag Body = (rv_set $dst, (rv_and (rv_not (rv_op 0)), (rv_op 1)));
}
def : RVMatch {
  string Glob = "ANDNPDrm";
  dag Body = (rv_set $dst, (rv_and (rv_not (rv_op 0)), (rv_op 1)));
}

// ---- packed integer arithmetic (lane-wise, from the mnemonic suffix) ---------
def : RVMatch {
  string Glob = "PADDBr*";
  dag Body = (rv_set $dst, (rv_add (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PADDWr*";
  dag Body = (rv_set $dst, (rv_add (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PADDD*r";
  dag Body = (rv_set $dst, (rv_add (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PADDQr*";
  dag Body = (rv_set $dst, (rv_add (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSUBBr*";
  dag Body = (rv_set $dst, (rv_sub (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSUBWr*";
  dag Body = (rv_set $dst, (rv_sub (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSUBDr*";
  dag Body = (rv_set $dst, (rv_sub (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSUBQr*";
  dag Body = (rv_set $dst, (rv_sub (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PMULLDr*";
  dag Body = (rv_set $dst, (rv_mul (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PMULUDQr*";
  dag Body = (rv_set $dst, (rv_mul (rv_op 0), (rv_op 1)));
}
// Whole-vector bitwise ops stay on i128.
def : RVMatch {
  string Glob = "PANDr*";
  dag Body = (rv_set $dst, (rv_and (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PANDNr*";
  dag Body = (rv_set $dst, (rv_and (rv_not (rv_op 0)), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PORr*";
  dag Body = (rv_set $dst, (rv_or (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PXORr*";
  dag Body = (rv_set $dst, (rv_xor (rv_op 0), (rv_op 1)));
}
// Per-lane compares produce all-ones/all-zero lanes; the generic integer
// icmp is type-agnostic so lane width rides on the operand types.
def : RVMatch {
  string Glob = "PCMPEQBr*";
  dag Body = (rv_set $dst, (rv_sext 8, (rv_icmp "eq", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPEQWr*";
  dag Body = (rv_set $dst, (rv_sext 16, (rv_icmp "eq", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPEQDr*";
  dag Body = (rv_set $dst, (rv_sext 32, (rv_icmp "eq", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPEQQr*";
  dag Body = (rv_set $dst, (rv_sext 64, (rv_icmp "eq", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPGTBr*";
  dag Body = (rv_set $dst, (rv_sext 8, (rv_icmp "sgt", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPGTWr*";
  dag Body = (rv_set $dst, (rv_sext 16, (rv_icmp "sgt", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPGTDr*";
  dag Body = (rv_set $dst, (rv_sext 32, (rv_icmp "sgt", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PCMPGTQr*";
  dag Body = (rv_set $dst, (rv_sext 64, (rv_icmp "sgt", (rv_op 0), (rv_op 1))));
}
def : RVMatch {
  string Glob = "PUNPCKLBWr*";
  dag Body = (rv_set $dst, (rv_punpckl 8, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKLWDr*";
  dag Body = (rv_set $dst, (rv_punpckl 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKLDQr*";
  dag Body = (rv_set $dst, (rv_punpckl 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKLQDQr*";
  dag Body = (rv_set $dst, (rv_punpckl 64, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKHBWr*";
  dag Body = (rv_set $dst, (rv_punpckh 8, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKHWDr*";
  dag Body = (rv_set $dst, (rv_punpckh 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKHDQr*";
  dag Body = (rv_set $dst, (rv_punpckh 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PUNPCKHQDQr*";
  dag Body = (rv_set $dst, (rv_punpckh 64, (rv_op 0), (rv_op 1)));
}

// ---- shuffles ------------------------------------------------------------------
def : RVMatch {
  string Glob = "PSHUFDri";
  dag Body = (rv_set $dst, (rv_pshufd (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSHUFDmi";
  dag Body = (rv_set $dst, (rv_pshufd (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSHUFHWri";
  dag Body = (rv_set $dst, (rv_pshufhw (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSHUFLWri";
  dag Body = (rv_set $dst, (rv_pshuflw (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSHUFDri";
  dag Body = (rv_setz $dst, (rv_pshufd (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSHUFHWri";
  dag Body = (rv_setz $dst, (rv_pshufhw (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSHUFLWri";
  dag Body = (rv_setz $dst, (rv_pshuflw (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "SHUFPSrri";
  dag Body = (rv_set $dst, (rv_shufps (rv_op 0), (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "SHUFPDrri";
  dag Body = (rv_set $dst, (rv_shufpd (rv_op 0), (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VSHUFPSYrri";
  dag Body = (rv_nop);
}
def : RVMatch {
  string Glob = "VSHUFPDYrri";
  dag Body = (rv_nop);
}
def : RVMatch {
  string Glob = "PSHUFBrr";
  dag Body = (rv_set $dst, (rv_pshufb (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSHUFBrm";
  dag Body = (rv_set $dst, (rv_pshufb (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSHUFBrr";
  dag Body = (rv_setz $dst, (rv_pshufb (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPSHUFBrm";
  dag Body = (rv_setz $dst, (rv_pshufb (rv_op 1), (rv_op 2)));
}

// ---- packed shifts ---------------------------------------------------------------
def : RVMatch {
  string Glob = "PSRLWri";
  dag Body = (rv_set $dst, (rv_vshift "psrl", 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSRLDri";
  dag Body = (rv_set $dst, (rv_vshift "psrl", 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSRLQri";
  dag Body = (rv_set $dst, (rv_vshift "psrl", 64, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSLLWri";
  dag Body = (rv_set $dst, (rv_vshift "psll", 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSLLDri";
  dag Body = (rv_set $dst, (rv_vshift "psll", 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSLLQri";
  dag Body = (rv_set $dst, (rv_vshift "psll", 64, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSRAWri";
  dag Body = (rv_set $dst, (rv_vshift "psra", 16, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSRADri";
  dag Body = (rv_set $dst, (rv_vshift "psra", 32, (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSRLWri";
  dag Body = (rv_setz $dst, (rv_vshift "psrl", 16, (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPSRLDri";
  dag Body = (rv_setz $dst, (rv_vshift "psrl", 32, (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPSRLQri";
  dag Body = (rv_setz $dst, (rv_vshift "psrl", 64, (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPSLLDri";
  dag Body = (rv_setz $dst, (rv_vshift "psll", 32, (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPSRAWri";
  dag Body = (rv_setz $dst, (rv_vshift "psra", 16, (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPSRADri";
  dag Body = (rv_setz $dst, (rv_vshift "psra", 32, (rv_op 1), (rv_op 2)));
}
// Whole-register byte shifts: imm8 byte count as an i128 logical shift.
def : RVMatch {
  string Glob = "PSRLDQri";
  dag Body = (rv_set $dst, (rv_psrldq (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "PSLLDQri";
  dag Body = (rv_set $dst, (rv_pslldq (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSRLDQri";
  dag Body = (rv_setz $dst, (rv_psrldq (rv_op 0), (rv_op 1)));
}
def : RVMatch {
  string Glob = "VPSLLDQri";
  dag Body = (rv_setz $dst, (rv_pslldq (rv_op 0), (rv_op 1)));
}

// ---- lane insert / extract ---------------------------------------------------------
def : RVMatch {
  string Glob = "PINSRWrri";
  dag Body = (rv_set $dst, (rv_pinsrw (rv_op 0), (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "PINSRWrmi";
  dag Body = (rv_set $dst, (rv_pinsrw (rv_op 0), (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPINSRWrri";
  dag Body = (rv_setz $dst, (rv_pinsrw (rv_op 0), (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "VPINSRWrmi";
  dag Body = (rv_setz $dst, (rv_pinsrw (rv_op 0), (rv_op 1), (rv_op 2)));
}
def : RVMatch {
  string Glob = "PEXTRDrri";
  dag Body = (rv_set $dst, (rv_zext 32, (rv_trunc 32, (rv_lshr (rv_op 0), (rv_mul (rv_and (rv_op 1), (rv_imm 3)), (rv_imm 32))))));
}
def : RVMatch {
  string Glob = "PEXTRQrri";
  dag Body = (rv_set $dst, (rv_lshr (rv_op 0), (rv_mul (rv_and (rv_op 1), (rv_imm 1)), (rv_imm 64))));
}
def : RVMatch {
  string Glob = "VPEXTRDrri";
  dag Body = (rv_set $dst, (rv_zext 32, (rv_trunc 32, (rv_lshr (rv_op 0), (rv_mul (rv_and (rv_op 1), (rv_imm 3)), (rv_imm 32))))));
}

// ---- masks, test, blend ---------------------------------------------------------------
def : RVMatch {
  string Glob = "PMOVMSKBrr";
  dag Body = (rv_set $dst, (rv_zext 32, (rv_pmovmskb (rv_op 0))));
}
def : RVMatch {
  string Glob = "VPMOVMSKBrr";
  dag Body = (rv_set $dst, (rv_zext 32, (rv_pmovmskb (rv_op 1))));
}
def : RVMatch {
  string Glob = "MOVMSKPSrr";
  dag Body = (rv_set $dst, (rv_zext 32, (rv_movmskps (rv_op 0))));
}
def : RVMatch {
  string Glob = "MOVMSKPDrr";
  dag Body = (rv_set $dst, (rv_zext 32, (rv_movmskpd (rv_op 0))));
}
def : RVMatch {
  string Glob = "PTESTrr";
  dag Body = (rv_ptest (rv_op 0), (rv_op 1));
}
def : RVMatch {
  string Glob = "PTESTrm";
  dag Body = (rv_ptest (rv_op 0), (rv_op 1));
}
def : RVMatch {
  string Glob = "VPTESTrr";
  dag Body = (rv_ptest (rv_op 0), (rv_op 1));
}
// pblendvb selects per byte from xmm0's sign bits: dst = (mask & b) | (~mask & a).
def : RVMatch {
  string Glob = "PBLENDVBrr0";
  dag Body = (rv_set $dst, (rv_or (rv_and (rv_pmovmskb (rv_reg "XMM0")), (rv_op 1)),
                                     (rv_and (rv_not (rv_pmovmskb (rv_reg "XMM0"))), (rv_op 0))));
}
def : RVMatch {
  string Glob = "PBLENDVBrm0";
  dag Body = (rv_set $dst, (rv_or (rv_and (rv_pmovmskb (rv_reg "XMM0")), (rv_op 1)),
                                     (rv_and (rv_not (rv_pmovmskb (rv_reg "XMM0"))), (rv_op 0))));
}

// ---- string-search compares (flags only; vector results stay opaque) ------------------
// The reference models these as opaque calls; the DSL keeps the flag effects
// the corpus actually consumes (ECX + ZF/SF/CF/OF per the SDM) so jcc/setcc
// after pcmpistri/pcmpestri lift without an opaque call.
def : RVMatch {
  string Glob = "PCMPESTRIrri";
  dag Body = (rv_pcmpextest "estri", (rv_op 0), (rv_op 1), (rv_op 2));
}
def : RVMatch {
  string Glob = "PCMPESTRMrri";
  dag Body = (rv_pcmpextest "estrm", (rv_op 0), (rv_op 1), (rv_op 2));
}
def : RVMatch {
  string Glob = "PCMPESTRMrmi";
  dag Body = (rv_pcmpextest "estrm", (rv_op 0), (rv_op 1), (rv_op 2));
}
def : RVMatch {
  string Glob = "PCMPISTRIrri";
  dag Body = (rv_pcmpistest "istri", (rv_op 0), (rv_op 1), (rv_op 2));
}
def : RVMatch {
  string Glob = "PCMPISTRMrri";
  dag Body = (rv_pcmpistest "istrm", (rv_op 0), (rv_op 1), (rv_op 2));
}
