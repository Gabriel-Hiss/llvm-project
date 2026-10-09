//==- revenant/Target/TargetHooks.h - Target-specific analysis ----*- C++ -*-==//
//
// Everything x86-specific that analysis needs, behind an interface so
// another architecture can plug in later. x86 implementation lives in
// lib/Target/X86. Mirrors the x86-iced knowledge baked into
// crates/analysis/{lib.rs,jump_table.rs,noreturn.rs,stackptr.rs}.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TARGET_TARGETHOOKS_H
#define REVENANT_TARGET_TARGETHOOKS_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <map>
#include <utility>

#include "llvm/ADT/ArrayRef.h"

namespace revenant {

/// Per-target instruction operations for jump-table recovery
/// (revenant/Analysis/JumpTable.h). Forward-declared so this header
/// stays free of the Analysis layer.
class JtInsnOps;
const JtInsnOps *getX86JumpTableInsnOps();

/// Result of jump-table shape matching at an indirect `jmp` (Rust
/// `analysis::jump_table::SwitchResolution`).
struct JumpTableInfo {
  /// Location holding the case-value operand at the guard block's end,
  /// plus a constant to add (case value = read(loc) + off).
  std::optional<std::pair<CmpOperand, int64_t>> IndexLoc;
  /// VA of the table's first entry (the *case* table for two-level).
  Va Table;
  /// Enumeration base for limit bookkeeping: the case table, or the
  /// byte index table for two-level switches.
  Va EnumBase;
  /// Byte ranges the table occupies: (index/case table, first-level
  /// byte table for two-level). Unset when unknown.
  std::optional<std::pair<VaRange, std::optional<VaRange>>> Spans;
  /// (target, case values reaching it) — one entry per distinct target.
  std::vector<std::pair<Va, std::vector<int64_t>>> Targets;
  /// Index into the caller's block list of the guard block whose
  /// default edge should be renamed `SwitchDefault`.
  std::optional<size_t> Guard;
};

enum class StackBase : uint8_t { Rsp, Rbp };

struct StackSlots {
  std::vector<std::pair<StackBase, int64_t>> Slots;
  bool HasDynamicMem = false;
};

struct FpEstablish {
  enum class Kind : uint8_t { None, MovRbpRsp, LeaRbpRsp } Tag = Kind::None;
  int64_t Disp = 0;
};

/// Target-specific queries and shape matchers used by function
/// discovery, CFG building and padding classification.
class TargetHooks {
public:
  virtual ~TargetHooks() = default;

  /// Instruction name used for filler / trap / mnemonic tests.
  virtual llvm::StringRef mnemonic(const Instr &I) const = 0;

  /// Alignment filler the compiler emits between code: `nop` (any form),
  /// `int3`, and `xchg ax,ax` (`66 90` decodes as an xchg, not nop).
  virtual bool isFiller(const Instr &I) const = 0;

  enum class TrapKind : uint8_t { NotTrap, Exception, Absorbable, Interrupt };
  virtual TrapKind trapKind(const Instr &I) const = 0;

  /// Instruction that never returns, derived from its classified trap kind.
  bool isNoreturnTrap(const Instr &I) const {
    return trapKind(I) != TrapKind::NotTrap;
  }

  /// The strict alignment-nop set used to reject filler-only candidates.
  virtual bool isNopOrInt3(const Instr &I) const = 0;

  virtual bool isOpaqueTailJmp(const Image &Img, const Instr &I) const = 0;

  /// Entry-relative stack depth change: push +8, pop -8,
  /// `sub rsp, imm` +imm, and `add rsp, imm` -imm.
  virtual std::optional<int64_t> stackDelta(const Instr &I) const = 0;
  virtual StackSlots stackSlots(const Instr &I) const = 0;
  virtual FpEstablish fpEstablish(const Instr &I) const = 0;
  virtual bool restoresSpFromFp(const Instr &I) const = 0;
  virtual std::optional<int64_t> movEaxImm(const Instr &I) const = 0;
  virtual bool writesSpDynamically(const Instr &I) const = 0;

  /// Instruction operand decoding for jump-table analysis (iced-level
  /// operand view). Implemented by the target's X86JumpTables; null when
  /// the target has no jump-table support.
  /// Return the target's jump-table instruction adapter.
  virtual const JtInsnOps *jumpTableInsnOps() const { return nullptr; }

  /// Try to resolve the indirect `jmp` terminating `Blocks[Dispatch]`
  /// through a jump table. Mirrors `analysis::jump_table::resolve`.
  /// `CodeRanges` is the owning function's code region; `LocalRipRefs`
  /// and `SharedRipRefs` are RIP-relative memory-operand targets (local
  /// to the function / merged across the program); `Limits` maps
  /// enumeration bases to exclusive case counts for unbounded tables.
  virtual std::optional<JumpTableInfo>
  matchJumpTable(const Image &Img,
                 llvm::ArrayRef<std::pair<Va, Va>> CodeRanges,
                 llvm::ArrayRef<BasicBlock> Blocks,
                 llvm::ArrayRef<Instr> Instrs, size_t Dispatch,
                 const std::vector<Va> &LocalRipRefs,
                 const std::vector<Va> &SharedRipRefs,
                 const std::map<Va, uint64_t> &Limits) const = 0;
};

/// The x86-64 implementation (lib/Target/X86).
std::unique_ptr<TargetHooks> createX86TargetHooks();

} // end namespace revenant

#endif // REVENANT_TARGET_TARGETHOOKS_H
