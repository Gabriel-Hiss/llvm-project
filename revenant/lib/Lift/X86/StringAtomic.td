//===- StringAtomic.td - string/atomic/fence lift semantics ----------------===//
//
// Bodies for x86 string instructions (with/without rep). Ports
// crates/lift/src/string.rs (h-merge reference). Atomic RMW and fences
// live in X86Lift.td (Swe2LiftGen): the mem forms were briefly here,
// then moved back so each owner file stays self-contained.
//
// Fragment: the RVOp/RVLift classes come from X86Lift.td; this file holds
// only its family op declarations and its RVLift defs. Swe2LiftGen adds
// the `include "StringAtomic.td"` line to RevenantX86.td on integration
// (never on this branch: his build would break on a missing include).
//
// Family op declarations (single source of truth for these names; the
// emitter dispatches by op name globally. All other ops this file's
// bodies use (rv_seq/set/reg/immw/add/sub/mul/select/load/fcmp, rv_df,
// rv_hasrep, rv_prefix) are declared in X86Lift.td; this file declares
// only the two its string bodies need and nothing else):
def rv_memmove : RVOp;    // (rv_memmove dst_i64, src_i64, len_i64)
def rv_memset : RVOp;     // (rv_memset dst_i64, val_i8, len_i64)
//
// Opcode census (RvLiftGen/opcount-out, 249 PEs, 51.4M decoded instr):
//   string (plain forms; rep arrives as separate prefix MCInsts, see below):
//   MOVSB 4759, STOSW 1405, STOSQ 1165, STOSB 1032, STOSL 586, LODSB 320,
//   MOVSQ 304, MOVSL 273, SCASB 239, LODSL 228, CMPSB 225, CMPSL 221,
//   SCASL 219, SCASQ 15, LODSQ 5, CMPSQ 2, CMPSW 1
//   (CLD 272 / STD 228 belong to Swe2LiftGen's flag-image group, not here.)
//   Atomics/prefixes/fences are Swe2LiftGen's (X86Lift.td); the high-hitters
//   there are XADD32rm 13423, XCHG32rm 2891, CMPXCHG64rm 2046, PAUSE 873.
//   LOCK_PREFIX 53450 (plain ALU with lock: his rv_frmw). REP_PREFIX/REPNE
//   decode count 0 in this corpus sweep: every observed string op is
//   single-step; rep forms are still covered below via pending-prefix
//   state because llvm-mc confirms `f3 a4` decodes as REP_PREFIX + MOVSB
//   (two MCInsts).
//
// Design notes:
// - Prefixes are standalone MCInsts; bodies read pending state via
//   rv_hasrep (stable for repeated reads: cleared only after the body).
//   REP_* codegen pseudos (REP_MOVSB_64 etc.) never decode; no entries.
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
//   SRC=[rSI]; Swe2LiftGen confirms rv_fcmp(a, b) flags a-b). SCAS is
//   acc - [RDI].
// - INSB/OUTSB/IN/OUT (port IO) and XLAT have no memory-intrinsic form:
//   omitted (-> opaque @revenant.asm.*).
// - REX64/segment prefixes need no entries (encoding / DS:SI-ES:DI
//   assumed by the address helper).
//
//===----------------------------------------------------------------------===//

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

