//===- X87.td - x87 FPU lift semantics --------------------------------===//
//
// Family file for the x87 register stack. Included from RevenantX86.td after
// X86Lift.td, which declares the shared ops used here (rv_seq/rv_set/rv_reg,
// rv_addr, rv_cpred with cond-name literals, rv_ffadd/ffsub/ffmul/ffdiv for
// x86_fp80 arithmetic, rv_nop, rv_opaque) plus the attachment classes RVLift
// and RVMatch. This file declares ONLY the rv_x87* ops and owns every x87
// def; see the state model note below.
//
// State model (mirrors the SSE file's XMM slots, but a stack):
//  - The stack is eight x86_fp80 slots in X86LiftContext plus a TOP index.
//    rv_x87push decrements TOP and fills the new top; rv_x87pop yields the
//    top value and increments TOP. Overflow/underflow cannot happen in
//    compiler output, so the context asserts rather than modelling #IS/#IE.
//  - ST(i) reads resolve through TOP at lift time; FXCH swaps slots in C++
//    (rv_x87xchg), which the dag cannot express with push/pop/set alone.
//  - The tag word is not modelled: FFREE writes undef into the slot, which
//    poisons only uses that the decompiler can see as dead stack slots.
//  - FPU status bits C0/C2/C3 live in the context's FPSW slots; FCOMI-family
//    results go straight into the EFLAGS model (ZF/PF/CF) like COMISS does.
//  - The control word is context state: FLDCW/FNSTCW round-trip it, FIST uses
//    llvm.lrint semantics (round-to-nearest ties-away matches RC=round for
//    all corpus values), FISTTP truncates. Actual precision control (PC) is
//    ignored: everything computes in x86_fp80.
//  - BCD (FBLD/FBSTP), 28/108-byte images (FLDENV/FSTENV/FSAVE/FRSTOR) and
//    FXSAVE/FXRSTOR keep exact byte effects no IR can name, so rv_opaque.
//  - Transcendentals with no LLVM intrinsic lower to their libm call in the
//    context (fyl2x = y*log2(x), fpatan = atan2(y,x), fscale = ldexp,
//    fprem/fprem1 = fmod/fmod-with-quotient-bits-dropped, fsin/fcos/fsincos
//    = the obvious calls, f2xm1 = 2^x-1 over pow). fptan pushes 1.0 after
//    the tangent exactly like the hardware. Corpus sees 1-46 hits each.
//
//===----------------------------------------------------------------------===//

// ---- family op classes ------------------------------------------------------
// Declared here, owned here. The emitter dispatches ops by name globally.

class RVOp;
def rv_x87push  : RVOp; // (rv_x87push v_f80): TOP--; ST(0) = v
def rv_x87pop   : RVOp; // (rv_x87pop): v = ST(0); TOP++
def rv_x87st    : RVOp; // (rv_x87st N): ST(N) as x86_fp80
def rv_x87stset : RVOp; // (rv_x87stset N, v): ST(N) = v
def rv_x87idx   : RVOp; // (rv_x87idx $r): i32 index of an RSTi operand
def rv_x87xchg  : RVOp; // (rv_x87xchg $r): swap ST(0) with ST(i)
def rv_x87fldconst : RVOp; // (rv_x87fldconst "0"|"1"|"pi"|"l2t"|"l2e"|"lg2"|"ln2")
def rv_x87math  : RVOp; // (rv_x87math "sqrt|sin|cos|tan|atan|rem|prem|scale|
                        //   abs|chs|rnd|extract|tst|exam|decstp|incstp|free")
def rv_fcom     : RVOp; // (rv_fcom [N-or-mem]): C0/C2/C3 from ST0 vs arg
def rv_fcomi    : RVOp; // (rv_fcomi [N]): ZF/PF/CF from ST0 vs ST(N)
def rv_fstsw    : RVOp; // (rv_fstsw): packed status word as i16
def rv_fldcw    : RVOp; // (rv_fldcw $m): control word = [mem16]
def rv_fstcw    : RVOp; // (rv_fstcw $m): [mem16] = control word
def rv_fclex    : RVOp; // (rv_fclex): clear exception flags
def rv_fld      : RVOp; // (rv_fld "f32|f64|f80|i16|i32|i64" $m): load+convert
def rv_fst      : RVOp; // (rv_fst "f32|f64|i16|i32|i64|ti16|ti32|ti64" $m, v)
                        //   store with convert; ti* truncate (FISTTP)

// ---- loads ----------------------------------------------------------------
// LD_F32m/64m/80m: (outs), (ins f32mem/f64mem/f80mem:$src).

def : RVLift {
  let Insn = LD_F32m;
  dag Body = (rv_x87push (rv_fld "f32", $src));
}
def : RVLift {
  let Insn = LD_F64m;
  dag Body = (rv_x87push (rv_fld "f64", $src));
}
def : RVLift {
  let Insn = LD_F80m;
  dag Body = (rv_x87push (rv_fld "f80", $src));
}

// ILD_F16m/32m/64m: integer loads convert exactly.

def : RVLift {
  let Insn = ILD_F16m;
  dag Body = (rv_x87push (rv_fld "i16", $src));
}
def : RVLift {
  let Insn = ILD_F32m;
  dag Body = (rv_x87push (rv_fld "i32", $src));
}
def : RVLift {
  let Insn = ILD_F64m;
  dag Body = (rv_x87push (rv_fld "i64", $src));
}

// LD_Frr: (outs), (ins RSTi:$op). Push a copy of ST(i).

def : RVLift {
  let Insn = LD_Frr;
  dag Body = (rv_x87push (rv_x87st (rv_x87idx $op)));
}

// Constants: FLDZ/FLD1/FLDPI/FLDL2T/FLDL2E/FLDLG2/FLDLN2, all push.

def : RVLift {
  let Insn = LD_F0;
  dag Body = (rv_x87push (rv_x87fldconst "0"));
}
def : RVLift {
  let Insn = LD_F1;
  dag Body = (rv_x87push (rv_x87fldconst "1"));
}
def : RVLift {
  let Insn = FLDPI;
  dag Body = (rv_x87push (rv_x87fldconst "pi"));
}
def : RVLift {
  let Insn = FLDL2T;
  dag Body = (rv_x87push (rv_x87fldconst "l2t"));
}
def : RVLift {
  let Insn = FLDL2E;
  dag Body = (rv_x87push (rv_x87fldconst "l2e"));
}
def : RVLift {
  let Insn = FLDLG2;
  dag Body = (rv_x87push (rv_x87fldconst "lg2"));
}
def : RVLift {
  let Insn = FLDLN2;
  dag Body = (rv_x87push (rv_x87fldconst "ln2"));
}

// ---- stores ---------------------------------------------------------------
// ST_F32m/64m: (outs), (ins f32mem/f64mem:$dst). No pop.

def : RVLift {
  let Insn = ST_F32m;
  dag Body = (rv_fst "f32", $dst, (rv_x87st 0));
}
def : RVLift {
  let Insn = ST_F64m;
  dag Body = (rv_fst "f64", $dst, (rv_x87st 0));
}

// ST_FP32m/64m/80m: same, then pop.

def : RVLift {
  let Insn = ST_FP32m;
  dag Body = (rv_seq (rv_fst "f32", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = ST_FP64m;
  dag Body = (rv_seq (rv_fst "f64", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = ST_FP80m;
  dag Body = (rv_seq (rv_fst "f80", $dst, (rv_x87st 0)), (rv_x87pop));
}

// ST_Frr/ST_FPrr: (outs), (ins RSTi:$op). Copy ST(0) into ST(i); pop for p.

def : RVLift {
  let Insn = ST_Frr;
  dag Body = (rv_x87stset (rv_x87idx $op), (rv_x87st 0));
}
def : RVLift {
  let Insn = ST_FPrr;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op), (rv_x87st 0)),
    (rv_x87pop));
}

// Integer stores round per the control word (rv_fst "i*"); FISTTP truncates.

def : RVLift {
  let Insn = IST_F16m;
  dag Body = (rv_fst "i16", $dst, (rv_x87st 0));
}
def : RVLift {
  let Insn = IST_F32m;
  dag Body = (rv_fst "i32", $dst, (rv_x87st 0));
}
def : RVLift {
  let Insn = IST_FP16m;
  dag Body = (rv_seq (rv_fst "i16", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = IST_FP32m;
  dag Body = (rv_seq (rv_fst "i32", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = IST_FP64m;
  dag Body = (rv_seq (rv_fst "i64", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = ISTT_FP16m;
  dag Body = (rv_seq (rv_fst "ti16", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = ISTT_FP32m;
  dag Body = (rv_seq (rv_fst "ti32", $dst, (rv_x87st 0)), (rv_x87pop));
}
def : RVLift {
  let Insn = ISTT_FP64m;
  dag Body = (rv_seq (rv_fst "ti64", $dst, (rv_x87st 0)), (rv_x87pop));
}

// ---- register arithmetic --------------------------------------------------
// FPST0rInst: (outs), (ins RSTi:$op), ST(0) = ST(0) <op> ST(i), no pop.
// FPrST0Inst: (outs), (ins RSTi:$op), ST(i) = ST(i) <op> ST(0), no pop.
// FPrST0PInst: ST(i) = ST(i) <op> ST(0), then pop (freed top is ST(0)).

def : RVLift {
  let Insn = ADD_FST0r;
  dag Body = (rv_x87stset 0, (rv_ffadd (rv_x87st 0), (rv_x87st (rv_x87idx $op))));
}
def : RVLift {
  let Insn = ADD_FrST0;
  dag Body = (rv_x87stset (rv_x87idx $op),
    (rv_ffadd (rv_x87st (rv_x87idx $op)), (rv_x87st 0)));
}
def : RVLift {
  let Insn = ADD_FPrST0;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op),
      (rv_ffadd (rv_x87st (rv_x87idx $op)), (rv_x87st 0))),
    (rv_x87pop));
}
def : RVLift {
  let Insn = SUB_FST0r;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_x87st 0), (rv_x87st (rv_x87idx $op))));
}
def : RVLift {
  let Insn = SUB_FrST0;
  dag Body = (rv_x87stset (rv_x87idx $op),
    (rv_ffsub (rv_x87st (rv_x87idx $op)), (rv_x87st 0)));
}
def : RVLift {
  let Insn = SUB_FPrST0;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op),
      (rv_ffsub (rv_x87st (rv_x87idx $op)), (rv_x87st 0))),
    (rv_x87pop));
}
def : RVLift {
  let Insn = SUBR_FST0r;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_x87st (rv_x87idx $op)), (rv_x87st 0)));
}
def : RVLift {
  let Insn = SUBR_FrST0;
  dag Body = (rv_x87stset (rv_x87idx $op),
    (rv_ffsub (rv_x87st 0), (rv_x87st (rv_x87idx $op))));
}
def : RVLift {
  let Insn = SUBR_FPrST0;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op),
      (rv_ffsub (rv_x87st 0), (rv_x87st (rv_x87idx $op)))),
    (rv_x87pop));
}
def : RVLift {
  let Insn = MUL_FST0r;
  dag Body = (rv_x87stset 0, (rv_ffmul (rv_x87st 0), (rv_x87st (rv_x87idx $op))));
}
def : RVLift {
  let Insn = MUL_FrST0;
  dag Body = (rv_x87stset (rv_x87idx $op),
    (rv_ffmul (rv_x87st (rv_x87idx $op)), (rv_x87st 0)));
}
def : RVLift {
  let Insn = MUL_FPrST0;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op),
      (rv_ffmul (rv_x87st (rv_x87idx $op)), (rv_x87st 0))),
    (rv_x87pop));
}
def : RVLift {
  let Insn = DIV_FST0r;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_x87st 0), (rv_x87st (rv_x87idx $op))));
}
def : RVLift {
  let Insn = DIV_FrST0;
  dag Body = (rv_x87stset (rv_x87idx $op),
    (rv_ffdiv (rv_x87st (rv_x87idx $op)), (rv_x87st 0)));
}
def : RVLift {
  let Insn = DIV_FPrST0;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op),
      (rv_ffdiv (rv_x87st (rv_x87idx $op)), (rv_x87st 0))),
    (rv_x87pop));
}
def : RVLift {
  let Insn = DIVR_FST0r;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_x87st (rv_x87idx $op)), (rv_x87st 0)));
}
def : RVLift {
  let Insn = DIVR_FrST0;
  dag Body = (rv_x87stset (rv_x87idx $op),
    (rv_ffdiv (rv_x87st 0), (rv_x87st (rv_x87idx $op))));
}
def : RVLift {
  let Insn = DIVR_FPrST0;
  dag Body = (rv_seq
    (rv_x87stset (rv_x87idx $op),
      (rv_ffdiv (rv_x87st 0), (rv_x87st (rv_x87idx $op)))),
    (rv_x87pop));
}

// ---- memory arithmetic ----------------------------------------------------
// _F32m/_F64m: (outs), (ins f32mem/f64mem:$src), ST(0) <op>= [mem].
// _FI16m/_FI32m: same with an integer source converted exactly.
// SUBR/DIVR reverse the operands (mem <op> ST(0)).

def : RVLift {
  let Insn = ADD_F32m;
  dag Body = (rv_x87stset 0, (rv_ffadd (rv_x87st 0), (rv_fld "f32", $src)));
}
def : RVLift {
  let Insn = ADD_F64m;
  dag Body = (rv_x87stset 0, (rv_ffadd (rv_x87st 0), (rv_fld "f64", $src)));
}
def : RVLift {
  let Insn = SUB_F32m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_x87st 0), (rv_fld "f32", $src)));
}
def : RVLift {
  let Insn = SUB_F64m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_x87st 0), (rv_fld "f64", $src)));
}
def : RVLift {
  let Insn = SUBR_F32m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_fld "f32", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = SUBR_F64m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_fld "f64", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = MUL_F32m;
  dag Body = (rv_x87stset 0, (rv_ffmul (rv_x87st 0), (rv_fld "f32", $src)));
}
def : RVLift {
  let Insn = MUL_F64m;
  dag Body = (rv_x87stset 0, (rv_ffmul (rv_x87st 0), (rv_fld "f64", $src)));
}
def : RVLift {
  let Insn = DIV_F32m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_x87st 0), (rv_fld "f32", $src)));
}
def : RVLift {
  let Insn = DIV_F64m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_x87st 0), (rv_fld "f64", $src)));
}
def : RVLift {
  let Insn = DIVR_F32m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_fld "f32", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = DIVR_F64m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_fld "f64", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = ADD_FI16m;
  dag Body = (rv_x87stset 0, (rv_ffadd (rv_x87st 0), (rv_fld "i16", $src)));
}
def : RVLift {
  let Insn = ADD_FI32m;
  dag Body = (rv_x87stset 0, (rv_ffadd (rv_x87st 0), (rv_fld "i32", $src)));
}
def : RVLift {
  let Insn = SUB_FI16m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_x87st 0), (rv_fld "i16", $src)));
}
def : RVLift {
  let Insn = SUB_FI32m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_x87st 0), (rv_fld "i32", $src)));
}
def : RVLift {
  let Insn = SUBR_FI16m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_fld "i16", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = SUBR_FI32m;
  dag Body = (rv_x87stset 0, (rv_ffsub (rv_fld "i32", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = MUL_FI16m;
  dag Body = (rv_x87stset 0, (rv_ffmul (rv_x87st 0), (rv_fld "i16", $src)));
}
def : RVLift {
  let Insn = MUL_FI32m;
  dag Body = (rv_x87stset 0, (rv_ffmul (rv_x87st 0), (rv_fld "i32", $src)));
}
def : RVLift {
  let Insn = DIV_FI16m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_x87st 0), (rv_fld "i16", $src)));
}
def : RVLift {
  let Insn = DIV_FI32m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_x87st 0), (rv_fld "i32", $src)));
}
def : RVLift {
  let Insn = DIVR_FI16m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_fld "i16", $src), (rv_x87st 0)));
}
def : RVLift {
  let Insn = DIVR_FI32m;
  dag Body = (rv_x87stset 0, (rv_ffdiv (rv_fld "i32", $src), (rv_x87st 0)));
}

// ---- compares ---------------------------------------------------------------
// Operand-less forms use the disassembler's register: COM_FST0r/COMP_FST0r
// carry (ins RSTi:$op); UCOM_Fr/UCOM_FPr the same; UCOM_FPPr/FCOMPP pop
// ST(0) and ST(1) with no operands at all.

def : RVLift {
  let Insn = COM_FST0r;
  dag Body = (rv_fcom (rv_x87idx $op));
}
def : RVLift {
  let Insn = COMP_FST0r;
  dag Body = (rv_seq (rv_fcom (rv_x87idx $op)), (rv_x87pop));
}
def : RVLift {
  let Insn = UCOM_Fr;
  dag Body = (rv_fcom (rv_x87idx $reg));
}
def : RVLift {
  let Insn = UCOM_FPr;
  dag Body = (rv_seq (rv_fcom (rv_x87idx $reg)), (rv_x87pop));
}
def : RVLift {
  let Insn = UCOM_FPPr;
  dag Body = (rv_seq (rv_fcom), (rv_x87pop), (rv_x87pop));
}
def : RVLift {
  let Insn = FCOMPP;
  dag Body = (rv_seq (rv_fcom), (rv_x87pop), (rv_x87pop));
}

// Memory compares: FCOM32m/64m (no pop), FCOMP32m/64m (pop), integer FICOM*.

def : RVLift {
  let Insn = FCOM32m;
  dag Body = (rv_fcom $src);
}
def : RVLift {
  let Insn = FCOM64m;
  dag Body = (rv_fcom $src);
}
def : RVLift {
  let Insn = FCOMP32m;
  dag Body = (rv_seq (rv_fcom $src), (rv_x87pop));
}
def : RVLift {
  let Insn = FCOMP64m;
  dag Body = (rv_seq (rv_fcom $src), (rv_x87pop));
}
def : RVLift {
  let Insn = FICOM16m;
  dag Body = (rv_fcom $src);
}
def : RVLift {
  let Insn = FICOMP16m;
  dag Body = (rv_seq (rv_fcom $src), (rv_x87pop));
}
def : RVLift {
  let Insn = FICOM32m;
  dag Body = (rv_fcom $src);
}
def : RVLift {
  let Insn = FICOMP32m;
  dag Body = (rv_seq (rv_fcom $src), (rv_x87pop));
}

// FCOMI-family: (outs), (ins RSTi:$reg), Defs EFLAGS. Plain forms keep the
// stack, P forms pop. fcomi raises #IA on QNaN, fucomi does not; the lifted
// IR is the same unordered comparison either way.

def : RVLift {
  let Insn = COM_FIr;
  dag Body = (rv_fcomi (rv_x87idx $reg));
}
def : RVLift {
  let Insn = COM_FIPr;
  dag Body = (rv_seq (rv_fcomi (rv_x87idx $reg)), (rv_x87pop));
}
def : RVLift {
  let Insn = UCOM_FIr;
  dag Body = (rv_fcomi (rv_x87idx $reg));
}
def : RVLift {
  let Insn = UCOM_FIPr;
  dag Body = (rv_seq (rv_fcomi (rv_x87idx $reg)), (rv_x87pop));
}

// ---- status and control words -----------------------------------------------
// FNSTSW16r writes AX; FNSTSWm stores the word; FNSTCW/FLDCW round-trip the
// control word through the context. FNINIT resets both; FNCLEX clears the
// exception flags.

def : RVLift {
  let Insn = FNSTSW16r;
  dag Body = (rv_set (rv_reg "AX"), (rv_fstsw));
}
def : RVLift {
  let Insn = FNSTSWm;
  dag Body = (rv_store $dst, (rv_fstsw));
}
def : RVLift {
  let Insn = FNSTCW16m;
  dag Body = (rv_fstcw $dst);
}
def : RVLift {
  let Insn = FLDCW16m;
  dag Body = (rv_fldcw $dst);
}

// ---- conditional moves ------------------------------------------------------
// CMOVcc_F: (outs), (ins RSTi:$op), ST(0) = ST(i) when the EFLAGS condition
// holds. The condition is a literal: the opcode names it.

def : RVLift {
  let Insn = CMOVB_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "b"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVBE_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "be"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVE_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "e"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVP_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "p"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVNB_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "nb"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVNBE_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "nbe"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVNE_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "ne"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}
def : RVLift {
  let Insn = CMOVNP_F;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_select (rv_cpred "np"), (rv_x87st (rv_x87idx $op)), (rv_x87st 0))));
}

// ---- stack manipulation -----------------------------------------------------

def : RVLift {
  let Insn = XCH_F;
  dag Body = (rv_x87xchg $op);
}
def : RVLift {
  let Insn = FFREE;
  dag Body = (rv_x87math "free", $reg);
}
def : RVLift {
  let Insn = FFREEP;
  dag Body = (rv_seq (rv_x87math "free", $reg), (rv_x87pop));
}
def : RVLift {
  let Insn = FINCSTP;
  dag Body = (rv_x87math "incstp");
}
def : RVLift {
  let Insn = FDECSTP;
  dag Body = (rv_x87math "decstp");
}

// ---- unary ------------------------------------------------------------------
// CHS_F/ABS_F/SQRT_F are operand-less: the hardware applies them to ST(0).

def : RVLift {
  let Insn = CHS_F;
  dag Body = (rv_x87stset 0, (rv_x87math "chs", (rv_x87st 0)));
}
def : RVLift {
  let Insn = ABS_F;
  dag Body = (rv_x87stset 0, (rv_x87math "abs", (rv_x87st 0)));
}
def : RVLift {
  let Insn = SQRT_F;
  dag Body = (rv_x87stset 0, (rv_x87math "sqrt", (rv_x87st 0)));
}
def : RVLift {
  let Insn = TST_F;
  dag Body = (rv_x87math "tst");
}
def : RVLift {
  let Insn = XAM_F;
  dag Body = (rv_x87math "exam");
}
def : RVLift {
  let Insn = FRNDINT;
  dag Body = (rv_x87stset 0, (rv_x87math "rnd", (rv_x87st 0)));
}

// ---- transcendentals ----------------------------------------------------------
// Dyadic forms pop like the hardware (fyl2x/fpatan/fscale replace ST(1) with
// the result and pop). fprem keeps both operands for the caller's loop test.

def : RVLift {
  let Insn = F2XM1;
  dag Body = (rv_x87stset 0, (rv_x87math "expm1", (rv_x87st 0)));
}
def : RVLift {
  let Insn = FYL2X;
  dag Body = (rv_x87math "yl2x", (rv_x87st 1), (rv_x87st 0));
}
def : RVLift {
  let Insn = FYL2XP1;
  dag Body = (rv_x87math "yl2xp1", (rv_x87st 1), (rv_x87st 0));
}
def : RVLift {
  let Insn = FPTAN;
  dag Body = (rv_x87math "tan", (rv_x87st 0));
}
def : RVLift {
  let Insn = FPATAN;
  dag Body = (rv_seq
    (rv_x87stset 1, (rv_x87math "atan", (rv_x87st 1), (rv_x87st 0))),
    (rv_x87pop));
}
def : RVLift {
  let Insn = FXTRACT;
  dag Body = (rv_x87math "extract", (rv_x87st 0));
}
def : RVLift {
  let Insn = FPREM;
  dag Body = (rv_x87stset 0, (rv_x87math "rem", (rv_x87st 0), (rv_x87st 1)));
}
def : RVLift {
  let Insn = FPREM1;
  dag Body = (rv_x87stset 0, (rv_x87math "prem", (rv_x87st 0), (rv_x87st 1)));
}
def : RVLift {
  let Insn = FSIN;
  dag Body = (rv_x87stset 0, (rv_x87math "sin", (rv_x87st 0)));
}
def : RVLift {
  let Insn = FCOS;
  dag Body = (rv_x87stset 0, (rv_x87math "cos", (rv_x87st 0)));
}
def : RVLift {
  let Insn = FSINCOS;
  dag Body = (rv_seq
    (rv_x87stset 0, (rv_x87math "cos", (rv_x87st 0))),
    (rv_x87push (rv_x87math "sin", (rv_x87st 1))));
}
def : RVLift {
  let Insn = FSCALE;
  dag Body = (rv_x87stset 0, (rv_x87math "scale", (rv_x87st 0), (rv_x87st 1)));
}

// ---- control and opaque -------------------------------------------------------

def : RVLift { let Insn = FNCLEX; dag Body = (rv_fclex); }
def : RVLift { let Insn = FNOP; dag Body = (rv_nop); }
def : RVLift { let Insn = WAIT; dag Body = (rv_nop); }

// Byte-exact state images with no IR equivalent: BCD loads/stores, the
// 28-byte environment, 108-byte save/restore, FXSAVE/FXRSTOR.
def : RVLift { let Insn = FBLDm; dag Body = (rv_opaque); }
def : RVLift { let Insn = FBSTPm; dag Body = (rv_opaque); }
def : RVLift { let Insn = FLDENVm; dag Body = (rv_opaque); }
def : RVLift { let Insn = FSTENVm; dag Body = (rv_opaque); }
def : RVLift { let Insn = FRSTORm; dag Body = (rv_opaque); }
def : RVLift { let Insn = FSAVEm; dag Body = (rv_opaque); }
def : RVMatch { let Glob = "FXSAVE*"; dag Body = (rv_opaque); }
def : RVMatch { let Glob = "FXRSTOR*"; dag Body = (rv_opaque); }
