//===- StringAtomic.td - string/atomic/fence lift semantics ----------------===//
//
// Bodies for x86 string instructions (with/without rep), atomic RMW
// (xchg/xadd/cmpxchg/cmpxchg16b) and fences. Ports crates/lift/src/string.rs
// and atomics.rs (h-merge reference).
//
// Fragment: the RVOp/RVLift classes come from X86Lift.td; this file holds
// only its family op declarations and its RVLift defs. Included from
// RevenantX86.td after X86Lift.td (include line owned by Swe2LiftGen).
//
// Family op declarations (single source of truth; the emitter dispatches
// by op name globally, so X86Lift.td's plain-ALU defs use rv_frmw from here):
def rv_prefix : RVOp;     // (rv_prefix "lock"|"rep"|"repne"): latch prefix
                          //   state, consumed by the next real instruction
def rv_hasrep : RVOp;     // i1: rep prefix pending
def rv_hasrepne : RVOp;   // i1: repne/repnz prefix pending
def rv_haslock : RVOp;    // i1: lock prefix pending
def rv_df : RVOp;         // i1: direction flag (1 = decrement)
def rv_memmove : RVOp;    // (rv_memmove dst_i64, src_i64, len_i64)
def rv_memset : RVOp;     // (rv_memset dst_i64, val_i8, len_i64)
def rv_atomicrmw : RVOp;  // (rv_atomicrmw "add"|"sub"|"and"|"or"|"xor"|"xchg",
                          //   addr_i64, val) -> old, seq_cst
def rv_cmpxchg : RVOp;    // (rv_cmpxchg addr_i64, expected, new) -> old;
                          //   writes ZF=success internally
def rv_cmpxchg16b : RVOp; // (rv_cmpxchg16b addr_i64): owns the EDX:EAX/RDX:RAX
                          //   expected and EBX:ECX/RBX:RCX new pairs, writes
                          //   the registers and ZF itself
def rv_fence : RVOp;      // seq_cst fence
def rv_frmw : RVOp;       // (rv_frmw "add"|"sub"|"and"|"or"|"xor"|"inc"|"dec"|
                          //   "neg"|"not", memdst, src): statement doing
                          //   load+op+flags+store, atomicrmw when rv_haslock
                          //   (used by X86Lift.td plain-ALU mr/mi defs)
//
// Opcode census (RvLiftGen/opcount-out, 249 PEs, 51.4M decoded instr):
//   string (plain forms; rep arrives as separate prefix MCInsts, see below):
//   MOVSB 4759, STOSW 1405, STOSQ 1165, STOSB 1032, STOSL 586, LODSB 320,
//   MOVSQ 304, MOVSL 273, SCASB 239, LODSL 228, CMPSB 225, CMPSL 221,
//   SCASL 219, SCASQ 15, LODSQ 5, CMPSQ 2, CMPSW 1
//   (CLD 272 / STD 228 belong to Swe2LiftGen's flag-image group, not here.)
//   atomics: XADD32rm 13423, XCHG32rm 2891, XCHG64rm 2599, CMPXCHG64rm 2046,
//   XCHG8rm 2006, CMPXCHG32rm 1799, XCHG32ar 1572, XADD64rm 900, PAUSE 873,
//   SFENCE 618, CMPXCHG8rm 507, XCHG8rr 80, XCHG64rr 63, XCHG32rr 45,
//   XCHG64ar 41, MFENCE 5, XADD8rm 3, XCHG16ar 1
//   LOCK_PREFIX 53450 (almost all plain ALU with lock: covered by rv_frmw
//   in the intops defs, not here). REP_PREFIX/REPNE_PREFIX decode count 0
//   in this corpus sweep: every observed string op is single-step; rep
//   forms are still covered below via pending-prefix state because llvm-mc
//   confirms `f3 a4` decodes as REP_PREFIX + MOVSB (two MCInsts).
//
// Design notes:
// - Prefixes are standalone MCInsts (LOCK_PREFIX/REP_PREFIX/REPNE_PREFIX),
//   so bodies read pending state via rv_hasrep/rv_hasrepne/rv_haslock.
//   REP_* codegen pseudos (REP_MOVSB_64 etc.) never decode; no entries.
//   LOCK_* ALU pseudos never decode either; locked ALU comes out of
//   rv_frmw in the plain ADD/SUB/AND/OR/XOR/INC/DEC bodies (X86Lift.td).
// - MOVS/STOS unify rep and single-step branch-free: len selects on
//   rv_hasrep (rep ? RCX*size : size), one memmove/memset, index updates
//   by signed len, RCX zeroed only under rep. (Reference string.rs never
//   updates RDI/RSI/RCX and ignores DF; this file does, matching what the
//   compiler's pre-opt IR carries as explicit updates around the
//   intrinsic. Deviation is intentional, see IR-1:1 priority.)
// - SCAS/CMPS are single-step compare + index update. repe/repne loops
//   need early exit on ZF, which needs real control flow from the
//   function driver; unobserved in corpus, left for later.
// - LODS is single-step (rep-lods is unobserved; same loop reason).
// - CMPS polarity is [RSI] - [RDI] (Intel SDM "TEMP <- SRC - DEST",
//   SRC=[rSI]); SCAS is acc - [RDI].
// - XCHG rr/ar lower to plain swap (rv_xchg), matching the reference
//   lift_xchg. XCHG rm uses atomicrmw "xchg" + explicit reg write: xchg
//   is implicitly locked on hardware, and spelling the RMW out keeps the
//   ordering guarantee visible in the IR.
// - XADD rm uses explicit atomicrmw "add" + rv_fadd on the returned old
//   value rather than the rv_xadd statement: every write target stays
//   explicit and $dst is evaluated once. XADD rr has zero corpus hits
//   and needs a temp under tied operands: omitted (-> opaque).
// - CMPXCHG rm: rv_cmpxchg returns old, writes ZF internally; storing old
//   into the accumulator is correct on both paths (success keeps acc).
//   CMPXCHG rr needs old twice under tied operands, zero hits: omitted.
// - CMPXCHG8B/16B: rv_cmpxchg16b owns the register pairs and ZF; the body
//   is the single call.
// - INSB/OUTSB/IN/OUT (port IO) and XLAT have no memory-intrinsic form:
//   omitted (-> opaque @revenant.asm.*).
// - REX64/segment prefixes need no entries (encoding / DS:SI-ES:DI
//   assumed by the address helper).
// - LFENCE/SFENCE are rv_nop (no data reordering / NT stores modeled);
//   only MFENCE is a real fence. PAUSE is rv_nop.
// - Implicit regs are written with (rv_set (rv_reg "NAME") v); rv_reg
//   yields the register's own width, so memset values truncate to i8.
//
//===----------------------------------------------------------------------===//

// ---- prefix pseudo-ops: latch state, consumed by the next real op ---------

def : RVLift { let Insn = LOCK_PREFIX; dag Body = (rv_prefix "lock"); }
def : RVLift { let Insn = REP_PREFIX; dag Body = (rv_prefix "rep"); }
def : RVLift { let Insn = REPNE_PREFIX; dag Body = (rv_prefix "repne"); }

// ---- MOVS: memmove, rep-aware length, DF-aware index update ----------------
// Unified shape (SZ = element bytes):
//   len = rep ? RCX*SZ : SZ; memmove(rdi, rsi, len);
//   rdi += df ? -len : len; rsi += df ? -len : len; rcx = rep ? 0 : rcx

def : RVLift {
  let Insn = MOVSB;
  dag Body = (rv_seq
    (rv_memmove (rv_reg "RDI"), (rv_reg "RSI"),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))),
      (rv_add (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}
def : RVLift {
  let Insn = MOVSW;
  dag Body = (rv_seq
    (rv_memmove (rv_reg "RDI"), (rv_reg "RSI"),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))),
      (rv_add (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}
def : RVLift {
  let Insn = MOVSL;
  dag Body = (rv_seq
    (rv_memmove (rv_reg "RDI"), (rv_reg "RSI"),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))),
      (rv_add (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}
def : RVLift {
  let Insn = MOVSQ;
  dag Body = (rv_seq
    (rv_memmove (rv_reg "RDI"), (rv_reg "RSI"),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))),
      (rv_add (rv_reg "RSI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}

// ---- STOS: memset from the accumulator, low byte only ----------------------

def : RVLift {
  let Insn = STOSB;
  dag Body = (rv_seq
    (rv_memset (rv_reg "RDI"), (rv_reg "AL"),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 1)), (rv_immw 64, 1))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}
def : RVLift {
  let Insn = STOSW;
  dag Body = (rv_seq
    (rv_memset (rv_reg "RDI"), (rv_trunc 8, (rv_reg "AX")),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 2)), (rv_immw 64, 2))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}
def : RVLift {
  let Insn = STOSL;
  dag Body = (rv_seq
    (rv_memset (rv_reg "RDI"), (rv_trunc 8, (rv_reg "EAX")),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 4)), (rv_immw 64, 4))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}
def : RVLift {
  let Insn = STOSQ;
  dag Body = (rv_seq
    (rv_memset (rv_reg "RDI"), (rv_trunc 8, (rv_reg "RAX")),
      (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))),
      (rv_add (rv_reg "RDI"), (rv_select (rv_hasrep), (rv_mul (rv_reg "RCX"), (rv_immw 64, 8)), (rv_immw 64, 8))))),
    (rv_set (rv_reg "RCX"), (rv_select (rv_hasrep), (rv_immw 64, 0), (rv_reg "RCX"))));
}

// ---- LODS: single load into the accumulator, RSI steps ---------------------

def : RVLift {
  let Insn = LODSB;
  dag Body = (rv_seq
    (rv_set (rv_reg "AL"), (rv_load 8, (rv_reg "RSI"))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 1)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 1)))));
}
def : RVLift {
  let Insn = LODSW;
  dag Body = (rv_seq
    (rv_set (rv_reg "AX"), (rv_load 16, (rv_reg "RSI"))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 2)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 2)))));
}
def : RVLift {
  let Insn = LODSL;
  dag Body = (rv_seq
    (rv_set (rv_reg "EAX"), (rv_load 32, (rv_reg "RSI"))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 4)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 4)))));
}
def : RVLift {
  let Insn = LODSQ;
  dag Body = (rv_seq
    (rv_set (rv_reg "RAX"), (rv_load 64, (rv_reg "RSI"))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 8)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 8)))));
}

// ---- SCAS: single compare acc vs [RDI], flags only, RDI steps --------------

def : RVLift {
  let Insn = SCASB;
  dag Body = (rv_seq
    (rv_fcmp (rv_reg "AL"), (rv_load 8, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 1)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 1)))));
}
def : RVLift {
  let Insn = SCASW;
  dag Body = (rv_seq
    (rv_fcmp (rv_reg "AX"), (rv_load 16, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 2)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 2)))));
}
def : RVLift {
  let Insn = SCASL;
  dag Body = (rv_seq
    (rv_fcmp (rv_reg "EAX"), (rv_load 32, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 4)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 4)))));
}
def : RVLift {
  let Insn = SCASQ;
  dag Body = (rv_seq
    (rv_fcmp (rv_reg "RAX"), (rv_load 64, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 8)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 8)))));
}

// ---- CMPS: single compare [RSI] vs [RDI], both indices step ----------------

def : RVLift {
  let Insn = CMPSB;
  dag Body = (rv_seq
    (rv_fcmp (rv_load 8, (rv_reg "RSI")), (rv_load 8, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 1)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 1)))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 1)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 1)))));
}
def : RVLift {
  let Insn = CMPSW;
  dag Body = (rv_seq
    (rv_fcmp (rv_load 16, (rv_reg "RSI")), (rv_load 16, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 2)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 2)))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 2)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 2)))));
}
def : RVLift {
  let Insn = CMPSL;
  dag Body = (rv_seq
    (rv_fcmp (rv_load 32, (rv_reg "RSI")), (rv_load 32, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 4)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 4)))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 4)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 4)))));
}
def : RVLift {
  let Insn = CMPSQ;
  dag Body = (rv_seq
    (rv_fcmp (rv_load 64, (rv_reg "RSI")), (rv_load 64, (rv_reg "RDI"))),
    (rv_set (rv_reg "RDI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RDI"), (rv_immw 64, 8)),
      (rv_add (rv_reg "RDI"), (rv_immw 64, 8)))),
    (rv_set (rv_reg "RSI"), (rv_select (rv_df),
      (rv_sub (rv_reg "RSI"), (rv_immw 64, 8)),
      (rv_add (rv_reg "RSI"), (rv_immw 64, 8)))));
}

// ---- XCHG -----------------------------------------------------------------
// rr/ar: plain swap, no flags (reference lift_xchg); ins-operand names,
// tied outs follow. rm: spelled-out atomicrmw "xchg" + reg write.

foreach I = [XCHG8rr, XCHG16rr, XCHG32rr, XCHG64rr] in
def : RVLift { let Insn = I; dag Body = (rv_xchg $src1, $src2); }

def : RVLift {
  let Insn = XCHG16ar;
  dag Body = (rv_xchg $src, (rv_reg "AX"));
}
def : RVLift {
  let Insn = XCHG32ar;
  dag Body = (rv_xchg $src, (rv_reg "EAX"));
}
def : RVLift {
  let Insn = XCHG64ar;
  dag Body = (rv_xchg $src, (rv_reg "RAX"));
}

foreach I = [XCHG8rm, XCHG16rm, XCHG32rm, XCHG64rm] in
def : RVLift {
  let Insn = I;
  dag Body = (rv_set $dst, (rv_atomicrmw "xchg", (rv_addr $ptr), $val));
}

// ---- XADD rm: old -> reg, flags of (old + val) ------------------------------
// $dst holds old after the write, so rv_fadd($dst, $val) sets exactly the
// add flags with no second atomic. rr forms need a temp under tied
// operands and have zero corpus hits: omitted (-> opaque).

foreach I = [XADD8rm, XADD16rm, XADD32rm, XADD64rm] in
def : RVLift {
  let Insn = I;
  dag Body = (rv_seq
    (rv_set $dst, (rv_atomicrmw "add", (rv_addr $ptr), $val)),
    (rv_fadd $dst, $val));
}

// ---- CMPXCHG rm: hook returns old and writes ZF, body writes acc ------------
// rr forms need old twice under tied operands (zero hits): omitted.

def : RVLift {
  let Insn = CMPXCHG8rm;
  dag Body = (rv_set (rv_reg "AL"),
    (rv_cmpxchg (rv_addr $dst), (rv_reg "AL"), $src));
}
def : RVLift {
  let Insn = CMPXCHG16rm;
  dag Body = (rv_set (rv_reg "AX"),
    (rv_cmpxchg (rv_addr $dst), (rv_reg "AX"), $src));
}
def : RVLift {
  let Insn = CMPXCHG32rm;
  dag Body = (rv_set (rv_reg "EAX"),
    (rv_cmpxchg (rv_addr $dst), (rv_reg "EAX"), $src));
}
def : RVLift {
  let Insn = CMPXCHG64rm;
  dag Body = (rv_set (rv_reg "RAX"),
    (rv_cmpxchg (rv_addr $dst), (rv_reg "RAX"), $src));
}

// ---- CMPXCHG8B/16B: hook owns the register pairs and ZF ---------------------

def : RVLift {
  let Insn = CMPXCHG8B;
  dag Body = (rv_cmpxchg16b (rv_addr $dst));
}
def : RVLift {
  let Insn = CMPXCHG16B;
  dag Body = (rv_cmpxchg16b (rv_addr $dst));
}

// ---- fences and pause -------------------------------------------------------

def : RVLift { let Insn = MFENCE; dag Body = (rv_fence); }
def : RVLift { let Insn = LFENCE; dag Body = (rv_nop); }
def : RVLift { let Insn = SFENCE; dag Body = (rv_nop); }

def : RVLift { let Insn = PAUSE; dag Body = (rv_nop); }
