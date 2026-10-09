//==- revenant/Core/Instr.h - Decoded instruction -----------------*- C++ -*-==//
//
// One decoded machine instruction: its VA, size, MCInst and flow info from
// MCInstrAnalysis. Mirrors `Instr` in the contract; the lifter reads raw
// operand info straight off `Inst` the way the Rust pipeline reads iced
// fields off `Instruction`.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CORE_INSTR_H
#define REVENANT_CORE_INSTR_H

#include "revenant/Core/Va.h"

#include <cstdint>

#include "llvm/MC/MCInst.h"

namespace revenant {

/// Which kind of flow an instruction performs (from MCInstrAnalysis /
/// MCInstrDesc flags; first matching kind wins).
enum class FlowKind : uint8_t {
  /// Straight-line (no transfer).
  None,
  /// `call`, direct or through a register/memory operand.
  Call,
  /// Direct conditional branch (jcc / loop).
  CondBranch,
  /// Direct unconditional `jmp`.
  Branch,
  /// Indirect jump through a register, memory, or jump table.
  IndirectBranch,
  /// `ret`-family.
  Return,
  /// ud2 / int3-style trap: execution does not continue.
  Trap,
};

/// One decoded instruction.
struct Instr {
  /// Absolute VA of the first byte.
  Va Addr;
  /// Byte length of the encoding.
  uint32_t Size = 0;
  /// Opcode + operands.
  llvm::MCInst Inst;
  FlowKind Flow = FlowKind::None;
  /// Absolute branch/call target for direct branches and calls.
  bool HasTarget = false;
  Va Target;
  /// RIP-relative memory operand's absolute address, when the instruction
  /// has one (lea source, data access, indirect call/jmp slot).
  bool HasMemTarget = false;
  Va MemTarget;

  /// VA of the first byte of the next instruction.
  Va nextVa() const { return Addr.wrappingAdd(Size); }
};

} // end namespace revenant

#endif // REVENANT_CORE_INSTR_H
