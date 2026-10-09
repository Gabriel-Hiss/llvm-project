//==- revenant/Analysis/JumpTable.h - jump-table recovery -*- C++ -*-==//
//
// Port of crates/analysis/src/jump_table.rs. Target-agnostic engine over
// a small instruction-ops interface (`JtInsnOps`); the x86 implementation
// of that interface lives in lib/Target/X86/X86JumpTables.cpp and is
// reached through `TargetHooks::jumpTableInsnOps`.
//
// Callers (function discovery): `resolveJumpTable` to turn a block's
// indirect `jmp` into switch edges, `jumpTableDataSpan` to mark table
// bytes as data when resolution fails, and `bogusTableEntryIndex` to
// learn enumeration limits for unbounded tables.
//
//===------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_JUMPTABLE_H
#define REVENANT_ANALYSIS_JUMPTABLE_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"
#include "revenant/Target/TargetHooks.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

namespace revenant {

//==--------------------------------------------------------------------==//
// Instruction view (iced `Instruction` analogue)
//==--------------------------------------------------------------------==//

/// Mnemonic families the jump-table engine reasons about (iced Mnemonic
/// subset). Everything else is `Other`.
enum class JtMnemonic : uint8_t {
  Other,
  Mov,
  Movzx,
  Movsxd,
  Lea,
  Add,
  Sub,
  Cmp,
  And,
  Shl,
  Shr,
  Imul,
  Nop,
  Int3,
  Xchg,
};

/// Operand kinds used by the engine (iced `OpKind` subset).
enum class JtOpKind : uint8_t {
  None,
  Reg,
  /// Immediate; `ImmForm` records the iced width class.
  Imm,
  /// Memory operand; the `Mem*` fields describe its address.
  Mem,
};

/// Iced immediate kind for the operand carrying an immediate, needed to
/// reproduce its sign/zero-extension quirks (`bound_imm`, `imm_val`).
enum class JtImmForm : uint8_t {
  None,
  /// imm8 for an 8-bit operand: read unsigned.
  I8,
  /// imm8 sign-extended for a 16-bit operand: iced's `bound_imm`/`imm_val`
  /// do not cover this kind (yield no bound / 0).
  I8To16,
  /// imm8 sign-extended for a 32/64-bit operand (iced Immediate8to32 /
  /// Immediate8to64; both read via `immediate8to32`).
  I8Wide,
  /// imm16 for a 16-bit operand.
  I16,
  /// imm32 for a 32-bit operand, or an 8-bit imm sign-extended to 32 for a
  /// 32-bit move (`MOV32ImmSExti8` / iced Immediate8to32).
  I32,
  /// imm32 sign-extended for a 64-bit operand (iced Immediate32to64).
  I32To64,
  /// imm64 (iced Immediate64).
  I64,
};

/// Condition of a decoded conditional branch (generic view of x86 COND_*,
/// so the guard reader stays target-agnostic). `Other` = not a known jcc.
enum class JtCond : uint8_t {
  Other,
  A,
  Ae,
  B,
  Be,
  L,
  Le,
  G,
  Ge,
};

/// Target view of one decoded instruction. Register ids are target
/// register numbers (x86: LLVM MC regs); `0` is "no register" and
/// `JtInsnOps::fullReg` widens a sub-register to its 64-bit parent
/// (iced `Register::full_register`). `MemDisp` follows iced's
/// `memory_displacement64()`: the raw signed displacement in general, but
/// the absolute target address when the base is the instruction pointer.
struct JtInsn {
  JtMnemonic Mn = JtMnemonic::Other;
  JtOpKind Op0 = JtOpKind::None;
  JtOpKind Op1 = JtOpKind::None;
  /// Register operands (0 when the operand isn't a register).
  unsigned Reg0 = 0;
  unsigned Reg1 = 0;
  /// Immediate operand value (LLVM sign-extends decoded immediates).
  int64_t Imm = 0;
  JtImmForm ImmForm = JtImmForm::None;
  /// Memory operand address parts (raw, non-widened registers).
  unsigned MemBase = 0;
  unsigned MemIndex = 0;
  uint32_t MemScale = 0;
  uint64_t MemDisp = 0;
  /// Whether the address base is instruction pointer relative.
  bool MemIpRel = false;
  /// Byte size of a `mov`/`movzx`/`movsxd` memory load (0 when unknown).
  uint32_t MemSize = 0;
  /// Condition code for `JtCond`-mapped jcc opcodes.
  JtCond Cond = JtCond::Other;
};

/// What `extend_filler_tail` needs from one decode: byte length, whether
/// the instruction is alignment filler (`nop`/`int3`/`xchg ax,ax`), and
/// whether it's a multi-byte nop (an alignment pad cell's closing
/// instruction; only meaningful when `Filler`).
struct JtFill {
  uint32_t Size = 0;
  bool Filler = false;
  bool MultiNop = false;
};

/// Stateful single-instruction decoder used by `extend_filler_tail`.
class JtFillDecoder {
public:
  virtual ~JtFillDecoder() = default;
  /// Decode one instruction at `va`; nullopt when undecodable.
  virtual std::optional<JtFill> decodeOne(Va Addr) = 0;
};

/// Per-target instruction operations the engine needs (the iced-x86
/// knowledge `jump_table.rs` bakes in). Implemented per target; the x86
/// version lives in lib/Target/X86/X86JumpTables.cpp.
class JtInsnOps {
public:
  virtual ~JtInsnOps() = default;
  /// Decode `instr`'s operands into the engine's view.
  virtual JtInsn operands(const Instr &I) const = 0;
  /// 64-bit parent register (iced `full_register`); pass-through for
  /// registers that have no wider parent, 0 for "no register".
  virtual unsigned fullReg(unsigned Reg) const = 0;
  /// A fresh byte-stream decoder over `img` for filler tail scans.
  virtual std::unique_ptr<JtFillDecoder>
  fillDecoder(const Image &Img) const = 0;
};

//==--------------------------------------------------------------------==//
// Engine context and entry points
//==--------------------------------------------------------------------==//

/// Shared read-only inputs for one function's table work (Rust
/// `jump_table::Ctx`).
struct JumpTableCtx {
  const Image &Img;
  /// The owning function's code region (primary `.pdata` range plus its
  /// chained ranges); empty when the function has no `.pdata`.
  llvm::ArrayRef<std::pair<Va, Va>> CodeRanges;
  /// RIP-relative memory-operand targets seen in this function (data
  /// references: other tables live next to this one).
  const std::set<Va> &LocalRipRefs;
  /// Same, merged across the whole program.
  const std::set<Va> &SharedRipRefs;
  /// Learned enumeration limits for unbounded tables: enumeration base
  /// (case table, or byte table for two-level) -> exclusive index.
  const std::map<Va, uint64_t> &Limits;

  /// `va` is the target of some RIP-relative memory operand in the
  /// program (adjacent data structure boundary).
  bool isReferenced(Va Addr) const {
    return LocalRipRefs.count(Addr) || SharedRipRefs.count(Addr);
  }
};

/// Try to resolve the indirect `jmp` terminating `Blocks[Dispatch]`
/// through a jump table (Rust `jump_table::resolve`).
std::optional<JumpTableInfo>
resolveJumpTable(const JumpTableCtx &Ctx, const TargetHooks &Hooks,
                 llvm::ArrayRef<BasicBlock> Blocks,
                 llvm::ArrayRef<Instr> Instrs, size_t Dispatch);

/// Byte spans of the jump table feeding `Blocks[Dispatch]`'s indirect
/// jump, for dispatches `resolveJumpTable` left unresolved (Rust
/// `jump_table::data_span`). Marks table bytes as data for the gap
/// sweep and padding classifier; never produces edges.
std::optional<std::vector<std::pair<Va, Va>>>
jumpTableDataSpan(const JumpTableCtx &Ctx, const TargetHooks &Hooks,
                  llvm::ArrayRef<Instr> Instrs,
                  llvm::ArrayRef<BasicBlock> Blocks, size_t Dispatch);

/// First enumeration index past the table's real end, or unset when the
/// enumeration stays clean (Rust `jump_table::bogus_entry_index`). Only
/// meaningful for dispatches resolved via the unbounded fallback.
std::optional<uint64_t>
bogusTableEntryIndex(const JumpTableCtx &Ctx, const TargetHooks &Hooks,
                     llvm::ArrayRef<BasicBlock> Blocks,
                     llvm::ArrayRef<Instr> Instrs, size_t Dispatch,
                     Va EnumBase, llvm::function_ref<bool(Va)> Straddles,
                     llvm::function_ref<bool(Va)> IsIndirect);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_JUMPTABLE_H
