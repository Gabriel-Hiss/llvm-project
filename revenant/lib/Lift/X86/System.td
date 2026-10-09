//===- System.td - bit ops, extends, xlat, opaque system ---------------===//
//
// Semantics for what X86Lift.td does not cover yet, in the lifter DSL
// (record shapes RVLift/RVMatch and leaf/operator tokens owned by
// LunaLiftGen2, ex Swe2LiftGen).
//
// Census (llvm-revenant-opcount over 288 corpus PEs, ~55.8M insns):
//   CDQE 30703, BT32ri8 26976, BTS32ri8 15613, INT 11339, BT64rr 11035,
//   TZCNT32rr 7393, BSWAP32r 5235, BSR32rr 4512, BSF32rr 4162,
//   LZCNT32rr 3951, CDQ 3488, BT64ri8 2619, BT32rr 2543, RORX32ri 2473,
//   BSR64rr 2438, CPUID 2131, CQO 1952, BTS32mi8 1779, BSWAP64r 1771,
//   BTR32ri8 1449, CWDE 1341, MULX64rr 913, PAUSE 883, BSF64rr 759,
//   BTS32rr 746, TZCNT64rr 677, SHLX32rr 625, RORX64ri 592, BTR64ri8 558,
//   SHRX64rr 525, BT16ri8 422, SHLX64rr 407, BTS64rr 393, XGETBV 382,
//   BTS32mr 274, SHRX32rr 269, ANDN32rr 260, XLAT 244, BTR64rr 242,
//   BTS64ri8 218, MOVBE32rm 215, POPCNT32rr 211, RDSEED32r 208,
//   SARX32rr 205, BTR32mi8 193, ADCX64rr 186, PREFETCHW 185, BTC64ri8 184,
//   ADOX64rr 178, MULX64rm 172, ... INT3 excluded: Rust lifts it as
//   llvm.debugtrap, never opaque.
//
// What the ISel patterns already give (do NOT repeat here):
//   BSWAP32r/BSWAP64r      = (bswap ...)
//   TZCNT/LZCNT/POPCNT rr  = (cttz/ctlz/ctpop ...), rm folds the load
//   BSF/BSR rr/rm          = X86bsf/X86bsr (flag+result node, invertible)
//   MOVBE rm/mr            = bswap(load)/store(bswap)
//   CRC32                  = int_x86_sse42_crc32_* (plain call, invertible)
//   PREFETCHT0/T1/T2/NTA/W = (prefetch ...)
//   CLFLUSH/CLFLUSHOPT/CLWB/CLDEMOTE = int_x86_* calls, invert for free.
//   PAUSE carries an int_x86_sse2_pause pattern but stays rv_opaque here:
//   it is a hardware hint with no IR semantics, and the contract routes
//   no-semantics instructions to call @revenant.asm.<mnemonic>.
//   INT carries int_x86_int but stays rv_opaque for the same reason
//   (INT3 is separate: llvm.debugtrap per the Rust reference).
//   RORX/SHLX/SHRX/SARX _EVEX and _NF variants carry no pattern; the base
//   VEX rr/ri/mi forms match RORX_Pats/ShiftX_Pats and invert for free.
//   Only the no-pattern forms get RVMatch below.
//   ANDN/BEXTR/BZHI/PDEP/PEXT rr/rm carry X86*_flag/X86* patterns;
//   the _EVEX/_NF twins carry null_frag and need RVMatch below.
//
// Reference: crates/lift/src/bitops.rs (lift_bt, lift_bitcount,
// lift_extend) and crates/lift/src/opaque.rs (lift_opaque) in the
// h-merge tree. Port is 1:1 first, per wave-2 rules.
//
// Emitter hooks this file needs (flagged for LunaLiftGen2):
//   rv_pdep/rv_pext      parallel deposit/extract (no ISD equivalent,
//                        X86ISD::PDEP/PEXT today)
//   rv_bextr             BMI1 field extract with ZF/CF/OF (X86ISD::BEXTR)
//   rv_bzhi              BMI2 zero-high-bits with CF-on-overflow (X86ISD::BZHI)
//   rv_bls               BLSR/BLSMSK/BLSI with polarity-correct CF
//                        (X86ISD lowering exists only rr-side; rm + EVEX/NF
//                        twins need the hook)
//   rv_shiftx            SHLX/SHRX/SARX with masked count, flags untouched
//                        (rotr/shl/srl pattern inversion covers base VEX;
//                        _EVEX/_NF + rm forms need the hook)
//   rv_rorx              RORX with masked imm8, flags untouched
//   rv_mulx              MULX full multiply, flags untouched (no ISD node)
//   rv_adcx/rv_adox      ADCX/ADOX split carry chains (CF-only / OF-only)
//   rv_opaque            force emitOpaque for one def (name defaults to the
//                        lowercased mnemonic)
// Until those land, the bodies below use only ops X86Lift.td already
// declares. Bodies that cannot be written without a new hook stay as
// comments naming the hook, so this file parses today and fills in
// without renames once the emitter catches up.
//
//===----------------------------------------------------------------------===//

// ---- bit test family: register-offset forms ------------------------------
// BT/BTC/BTR/BTS rr/ri8 carry (X86bt ...) patterns and invert for free.
// Memory-base + register-offset forms (BT32mr etc.) carry no pattern:
// the bit address crosses element boundaries, so they need rv_bt with
// the memory address. Memory-base + imm8 forms (BT32mi8 etc.) carry an
// (X86bt (load ...)) pattern and invert for free.
// Operand shape: (ins iNmem:$src1, GRN:$src2), no outs; RVLift bodies
// below address operands by name ($src1/$src2).

def : RVMatch {
  let Glob = "BT16mr";
  dag Body = (rv_bt "bt", $src1, $src2);
}
def : RVMatch {
  let Glob = "BT32mr";
  dag Body = (rv_bt "bt", $src1, $src2);
}
def : RVMatch {
  let Glob = "BT64mr";
  dag Body = (rv_bt "bt", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTC16mr";
  dag Body = (rv_bt "btc", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTC32mr";
  dag Body = (rv_bt "btc", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTC64mr";
  dag Body = (rv_bt "btc", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTR16mr";
  dag Body = (rv_bt "btr", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTR32mr";
  dag Body = (rv_bt "btr", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTR64mr";
  dag Body = (rv_bt "btr", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTS16mr";
  dag Body = (rv_bt "bts", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTS32mr";
  dag Body = (rv_bt "bts", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTS64mr";
  dag Body = (rv_bt "bts", $src1, $src2);
}

// Register-base and mem+imm8 BTC/BTR/BTS forms carry no pattern at all
// (only the BT forms do), so they need rv_bt as well. BitTestOp writes
// the modified base back for bts/btr/btc and sets CF to the tested bit;
// the tied $src1=$dst constraint is handled by the (rv_set $dst, ...)
// wrapper, matching how flag-setting binary ops spell it in X86Lift.td.
// Operand shape rr/ri8: (outs GRN:$dst), (ins GRN:$src1, ...).
// Operand shape mi8: (outs), (ins iNmem:$src1, imm:$src2).

def : RVMatch {
  let Glob = "BTC16r*";
  dag Body = (rv_set $dst, (rv_bt "btc", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTC32r*";
  dag Body = (rv_set $dst, (rv_bt "btc", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTC64r*";
  dag Body = (rv_set $dst, (rv_bt "btc", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTR16r*";
  dag Body = (rv_set $dst, (rv_bt "btr", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTR32r*";
  dag Body = (rv_set $dst, (rv_bt "btr", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTR64r*";
  dag Body = (rv_set $dst, (rv_bt "btr", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTS16r*";
  dag Body = (rv_set $dst, (rv_bt "bts", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTS32r*";
  dag Body = (rv_set $dst, (rv_bt "bts", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTS64r*";
  dag Body = (rv_set $dst, (rv_bt "bts", $src1, $src2));
}
def : RVMatch {
  let Glob = "BTC16m*";
  dag Body = (rv_bt "btc", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTC32m*";
  dag Body = (rv_bt "btc", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTC64m*";
  dag Body = (rv_bt "btc", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTR16m*";
  dag Body = (rv_bt "btr", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTR32m*";
  dag Body = (rv_bt "btr", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTR64m*";
  dag Body = (rv_bt "btr", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTS16m*";
  dag Body = (rv_bt "bts", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTS32m*";
  dag Body = (rv_bt "bts", $src1, $src2);
}
def : RVMatch {
  let Glob = "BTS64m*";
  dag Body = (rv_bt "bts", $src1, $src2);
}
// ---- sign/zero extends: cbw/cwde/cdqe/cwd/cdq/cqo --------------------------
// CBW: (outs), (ins), Uses [AL], Defs [AX]. CWDE: Uses [AX], Defs [EAX].
// CDQE: Uses [EAX], Defs [RAX]. CWD: Uses [AX], Defs [AX,DX].
// CDQ: Uses [EAX], Defs [EAX,EDX]. CQO: Uses [RAX], Defs [RAX,RDX].
// All six carry no pattern. rv_sextacc takes the target width W:
// 16=CBW, 32=CWDE, 64=CDQE accumulate-extend; negative -W fills the
// high half (CWD=-16, CDQ=-32, CQO=-64). Matches sextAcc in
// X86LiftContext.cpp and lift_extend in bitops.rs 1:1.
def : RVLift {
  let Insn = CBW;
  dag Body = (rv_sextacc 16);
}
def : RVLift {
  let Insn = CWDE;
  dag Body = (rv_sextacc 32);
}
def : RVLift {
  let Insn = CDQE;
  dag Body = (rv_sextacc 64);
}
def : RVLift {
  let Insn = CWD;
  dag Body = (rv_sextacc -16);
}
def : RVLift {
  let Insn = CDQ;
  dag Body = (rv_sextacc -32);
}
def : RVLift {
  let Insn = CQO;
  dag Body = (rv_sextacc -64);
}
// ---- xlat: no pattern, implicit table lookup -------------------------------
// XLAT: (outs), (ins), Uses [AL,EBX], Defs [AL]. AL = [RBX + AL]
// (address size picks BX/EBX/RBX; 64-bit default is RBX). No flags.
// The address addition needs the zexted AL plus the base; rv_reg names
// the physical registers explicitly since XLAT declares no operands.
def : RVLift {
  let Insn = XLAT;
  dag Body = (rv_set (rv_reg "AL"), (rv_load 8, (rv_add (rv_reg "RBX"), (rv_zext 64, (rv_reg "AL")))));
}
// ---- BLSR/BLSMSK/BLSI: no pattern, need rv_bls ----------------------------
// Bls<"blsr"...> defs carry no pattern (the Bls_Pats Pat<>s invert the
// base rr forms for free, but _EVEX/_NF twins and rm forms need the
// hook). Flags per SDM: BLSI CF=(src!=0); BLSR/BLSMSK CF=(src==0);
// OF=0; ZF/SF from result; AF/PF undefined.
// Operand shape rr: (outs GRN:$dst), (ins GRN:$src1).
// Operand shape rm: (outs GRN:$dst), (ins x86memop:$src).
// NEEDS-HOOK rv_bls ("blsr"|"blsmsk"|"blsi", $src1).

// ---- ANDN: base VEX inverts, twins need the same body ---------------------
// ANDN32rr/rm carry (X86and_flag (not...)) patterns; Andn_Pats covers
// them. _EVEX/_NF twins carry null_frag. Same operand shape as BinOpRR/RM:
// (outs GRN:$dst), (ins GRN:$src1, GRN:$src2 / MemOperand:$src2).
// NEEDS-HOOK rv_fandn? Cheaper: (rv_fand (rv_not $src1), $src2) once the
// emitter supports nesting rv_not inside a flag producer. Flagged.

 // ---- BEXTR/BZHI: base inverts, twins need hooks --------------------------
 // BEXTR32rr/rm carry (X86bextr ...); BZHI32rr/rm carry (X86bzhi ...).
 // _EVEX/_NF twins carry null_frag. Flags per SDM: BEXTR ZF=(res==0),
 // CF=OF=0, AF/SF/PF undef; BZHI ZF/SF from result, CF=(index>width-1),
 // OF=0, AF/PF undef.
 // Operand shape: (outs GRN:$dst), (ins GRN:$src1, GRN:$src2).
 // NEEDS-HOOK rv_bextr ($src1, $src2); rv_bzhi ($src1, $src2).

 // ---- PDEP/PEXT: base inverts, twins need hooks ----------------------------
 // PDEP32rr/rm carry (X86pdep ...); PEXT32rr/rm carry (X86pext ...).
 // _EVEX twins have identical patterns and invert the same way, so no
 // DSL is needed for them either. No _NF variants exist. Flags: none
 // affected. Nothing to write here; comment records the decision.
 // (If a future APX twin drops its pattern, add rv_pdep/rv_pext matches.)

 // ---- SHLX/SHRX/SARX/RORX: base VEX inverts, rest needs hooks -------------
 // Base rr/rm/ri/mi match ShiftX_Pats/RORX_Pats (shl/srl/sra/rotr) and
 // invert for free. _EVEX twins carry no pattern. Counts mask to 5/6
 // bits; flags untouched (unlike SHL/SHR/SAR/ROR). RORX count is imm8.
 // Operand shapes: ShiftX rr (ins GRN:$src1, GRN:$src2), rm (ins
 // MemOperand:$src1, GRN:$src2); RorX ri (ins GRN:$src1, u8imm:$src2),
 // mi (ins MemOperand:$src1, u8imm:$src2).
 // NEEDS-HOOK rv_shiftx ("shlx"|"shrx"|"sarx", $src1, $src2);
 // NEEDS-HOOK rv_rorx ($src1, $src2).

// ---- MULX: no pattern anywhere, needs rv_mulx ------------------------------
// MulX rr: (outs GRN:$dst1, GRN:$dst2), (ins GRN:$src), Uses [EDX/RDX]
// (implicit high multiplicand). rm folds the load. mulx_args prints
// "{$src, $dst2, $dst1|$dst1, $dst2, $src}". Semantics: unsigned
// EDX:EAX (RDX:RAX) * src -> dst2:dst1, flags untouched, so MULX never
// disturbs an ADCX/ADOX carry chain. 913 + 172 hits in the corpus.
// NEEDS-HOOK rv_mulx ($src) writing $dst1 (low) + $dst2 (high).

// ---- ADCX/ADOX: null_frag patterns, split carry chains ---------------------
// ADCX32rr: (outs GR32:$dst), (ins GR32:$src1, GR32:$src2),
// Uses/Defs [EFLAGS]. Semantics: dst = src1 + src2 + CF; only CF
// written, OF/SF/ZF/AF/PF preserved. ADOX identical with the OF chain.
// NEEDS-HOOK rv_adcx ($src1, $src2); rv_adox ($src1, $src2).

// ---- RDRAND/RDSEED: X86rdrand/X86rdseed nodes, need flag detail ------------
// RDRAND32r: (outs GR32:$dst), (ins), Defs [EFLAGS]. CF = success,
// OF/SF/ZF/AF/PF = 0. The raw intrinsic call inverts from the pattern,
// but the flag writes (CF live, rest cleared) need a hook to beat the
// reference opaque fallback's undef-flags.
// NEEDS-HOOK rv_rdrand / rv_rdseed writing $dst with the flag contract.
// Until then these correctly fall back to emitOpaque (see opaque section).

// ---- CRC32/MOVBE: patterns invert, nothing to write ------------------------
// CRC32r32r32: (outs GR32:$dst), (ins GR32:$src1, GR32:$src2) with
// int_x86_sse42_crc32_32_32; rm folds. MOVBE32rm: bswap(load);
// MOVBE32mr: store(bswap). Flags: CRC32 none affected. Nothing here.


// ---- opaque system instructions: defined operands, no IR semantics ---------
// Per the contract, instructions with no IR semantics lift as
// call @revenant.asm.<mnemonic>(...) with !revenant.opaque. The Rust
// reference (opaque.rs) feeds explicit operand values plus implicit
// register reads and slices the i256 result back into the written regs.
// In the DSL these stay opaque by having no RVLift/RVMatch and no
// invertible pattern, which routes them to emitOpaque automatically.
// rv_opaque forces that route for defs whose pattern exists but is
// wrong for lifting (int_x86_int carries a codegen intrinsic, not IR).
// Each entry below names the def and its operand contract so the
// fallback slices the right registers:
//
//   CPUID:   (outs), (ins), Uses [EAX,ECX], Defs [EAX,EBX,ECX,EDX]
//   RDTSC:   (outs), (ins), Defs [RAX,RDX]
//   RDTSCP:  (outs), (ins), Defs [RAX,RCX,RDX]
//   XGETBV:  (outs), (ins), Uses [ECX], Defs [EDX,EAX]
//   INT:     (outs), (ins u8imm:$trap), pattern int_x86_int -> rv_opaque
//   SYSCALL: (outs), (ins), no Defs/Uses in the .td (implicit RCX/R11)
//   SYSRET:  (outs), (ins), SYSRET64 twin
//   PAUSE:   (outs), (ins), pattern int_x86_sse2_pause; hardware hint,
//            no state change, but keep opaque (never delete: ASM 1:1
//            lists it, and the lead's order is asm fidelity first)
//   PREFETCH*: patterns are plain (prefetch ...) hints; keep them as
//            real prefetch calls via inversion, NOT opaque. Listed here
//            only to record the decision: PREFETCH, PREFETCHW,
//            PREFETCHT0/T1/T2/NTA, PREFETCHWT1 all invert for free.
//   CLFLUSH/CLFLUSHOPT/CLWB/CLDEMOTE: int_x86_* calls, invert for free.
//            Same decision as prefetch: real calls, not opaque.
//   RDPMC:   (outs), (ins), Uses [ECX], Defs [RAX,RDX] -> opaque
//   RDMSR:   (outs), (ins), Uses [ECX], Defs [RAX,RDX] -> opaque
//   RDTSCP covered above; RDFSBASE/RDGSBASE/WRFSBASE/WRGSBASE carry
//   int_x86_* patterns and invert for free (real semantics, not opaque).
//   INVEPT64/INVVPID64: (ins GR64:$src1, i128mem:$src2) -> opaque.
//   INT3/INTO stay out: INT3 is llvm.debugtrap per the Rust reference,
//   INTO is 32-bit-only (Requires Not64BitMode).

def : RVLift {
  let Insn = CPUID;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = RDTSC;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = RDTSCP;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = XGETBV;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = INT;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = SYSCALL;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = SYSRET;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = SYSRET64;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = PAUSE;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = RDPMC;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = RDMSR;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = INVEPT64;
  dag Body = (rv_opaque);
}
def : RVLift {
  let Insn = INVVPID64;
  dag Body = (rv_opaque);
}
