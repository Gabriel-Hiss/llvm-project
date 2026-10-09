//==- revenant/Analysis/Signatures.h - Win64 signature recovery --*- C++ -*-==//
//
// Port of crates/sigs: interprocedural signature recovery over the asm
// CFG. Which of rcx/rdx/r8/r9, xmm0-3 and the stack slots are live-in
// parameters, and whether the function returns a value in rax/xmm0.
// Bottom-up over the call graph: callee returns are inferred first, then
// call sites decide whether the caller reads them.
//
// The Rust original also overrides with PDB signatures when present; the
// fork pipeline never reads the PDB (the oracle comparison lives in
// tools/revenant-sig-oracle), so that override is dropped and
// `Param::name`/`pdb_type` are gone with it. `SigSource::Pdb` is kept for
// source compatibility with the lifter port.
//
// Register operands are LLVM MC register ids (X86::RCX, X86::XMM0, ...).
// Frame offsets follow the Rust convention: entry-rsp relative, so the
// first incoming stack argument is at 0x28 (return address + 4*8 shadow).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_SIGNATURES_H
#define REVENANT_ANALYSIS_SIGNATURES_H

#include "revenant/Core/Va.h"

#include <cstdint>
#include <map>
#include <vector>

namespace revenant {

class Disasm;
struct Program;

/// Where a parameter arrives (Rust `sigs::ParamLoc`).
struct ParamLoc {
  enum class Kind : uint8_t {
    /// One of rcx/rdx/r8/r9/xmm0-3.
    Reg,
    /// `[entry_rsp + off]`, off >= 0x28.
    Stack,
  } Tag = Kind::Reg;
  /// `Kind::Reg`: MC register id. `Kind::Stack`: frame offset.
  uint32_t Reg = 0;
  int64_t Off = 0;

  static ParamLoc reg(unsigned McReg) {
    ParamLoc L;
    L.Tag = Kind::Reg;
    L.Reg = McReg;
    return L;
  }
  static ParamLoc stack(int64_t Off) {
    ParamLoc L;
    L.Tag = Kind::Stack;
    L.Off = Off;
    return L;
  }
  bool isStack() const { return Tag == Kind::Stack; }

  bool operator==(const ParamLoc &O) const {
    return Tag == O.Tag && Reg == O.Reg && Off == O.Off;
  }
};

struct Param {
  ParamLoc Loc;
  /// Widest observed read: 8/16/32/64/128.
  uint32_t Bits = 0;
  /// Lives in an xmm register.
  bool Float = false;
};

struct RetLoc {
  bool Float = false;
  uint32_t Bits = 0;
};

struct Signature {
  std::vector<Param> Params;
  /// Set when a return value in rax/xmm0 is proven.
  bool HasRet = false;
  RetLoc Ret;
  bool Noreturn = false;
  /// Rust `sigs::SigSource`.
  enum class Source : uint8_t {
    /// Signature came from debug info. Never produced by this port;
    /// kept so lifter-side code shared with Rust still compiles.
    Pdb,
    Inferred,
    /// Nothing proven; conservative default (4 int regs, i64 ret).
    Default,
  } SigSource = Source::Inferred;
};

/// Function-start or IAT-slot keyed signatures (Rust `sigs::Signatures`).
struct Signatures {
  /// Function start VA -> signature.
  std::map<Va, Signature> Funcs;
  /// IAT slot VA -> signature for the imported symbol.
  std::map<Va, Signature> Externs;
  /// Call site VA -> argument prefix length (indirect calls only).
  std::map<Va, uint32_t> IndirectArgs;
  /// Call-site VA -> observed register-arg count: backstop for callees
  /// whose own signature came back empty.
  std::map<Va, uint32_t> CallsiteArgs;

  const Signature *of(Va Addr) const {
    auto It = Funcs.find(Addr);
    return It == Funcs.end() ? nullptr : &It->second;
  }
};

/// Compute signatures for all functions + externs (Rust
/// `sigs::analyze`). `Dis` decodes the compact `FuncRecord` instruction
/// lists back into `Instr`s; it must reference `*Prog.Img`.
Signatures analyzeSignatures(const Program &Prog, const Disasm &Dis);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_SIGNATURES_H
