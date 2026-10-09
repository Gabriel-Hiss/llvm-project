//==- lib/Analysis/JumpTable.cpp - jump-table recovery -------------------==//
//
// Port of crates/analysis/src/jump_table.rs, behavior 1:1. All
// instruction-level knowledge goes through `JtInsnOps` (the x86
// implementation lives in lib/Target/X86/X86JumpTables.cpp); the engine
// itself is target-agnostic except for the CFG/edge model it shares with
// the Rust original.
//
// Rust naming -> C++ naming:
//   resolve -> resolveJumpTable, data_span -> jumpTableDataSpan,
//   bogus_entry_index -> bogusTableEntryIndex, Ctx -> JumpTableCtx,
//   TableAccess/Trace/IndexTrace/CmpOperand keep their shapes.
//
//===------------------------------------------------------------------===//

#include "revenant/Analysis/JumpTable.h"

#include <algorithm>
#include <utility>

using namespace llvm;

namespace revenant {
namespace {

static unsigned full(const JtInsnOps &Ops, unsigned Reg) {
  return Ops.fullReg(Reg);
}

static CmpOperand regCmp(unsigned FullReg) {
  CmpOperand O;
  O.Tag = CmpOperand::Kind::Reg;
  O.Reg = FullReg;
  return O;
}

static CmpOperand memCmp(const JtInsn &I) {
  CmpOperand O;
  O.Tag = CmpOperand::Kind::Mem;
  O.Base = I.MemBase;
  O.Index = I.MemIndex;
  O.Scale = I.MemScale;
  O.Disp = I.MemDisp;
  return O;
}

/// How the indirect jump reaches the table (Rust `TableAccess`).
struct TableAccess {
  /// VA of the first table entry.
  Va Table;
  /// Bytes per entry (4 = RVA/table-relative i32, 8 = absolute u64).
  uint64_t EntrySize = 0;
  /// i32 entries are RVAs (MSVC) when true, table-relative when false.
  /// Only meaningful for EntrySize == 4.
  bool RvaEntries = false;
  /// The register used as the index in the load (`memory_index`).
  unsigned IndexReg = 0;
  /// Window into `Instrs` holding the setup that produced this access:
  /// the shared-tail dispatch's own window, or a trampoline
  /// predecessor's. The index trace walks this window, not an arbitrary
  /// lookbehind from the dispatch block.
  std::optional<std::pair<size_t, size_t>> Origin;

  std::optional<Va> read(const JumpTableCtx &Ctx, uint64_t I) const {
    Va EntryVa = Table.wrappingAdd(I * EntrySize);
    if (EntrySize == 8) {
      auto V = Ctx.Img.readU64(EntryVa);
      if (!V || !Ctx.Img.isExecutable(Va(*V)))
        return std::nullopt;
      return Va(*V);
    }
    auto U = Ctx.Img.readU32(EntryVa);
    if (!U)
      return std::nullopt;
    int64_t Raw = (int64_t)(int32_t)*U;
    Va V = RvaEntries
               ? Va(Ctx.Img.Base.Value + (uint64_t)(uint32_t)Raw)
               : Table.wrappingAdd((uint64_t)Raw);
    if (!Ctx.Img.isExecutable(V))
      return std::nullopt;
    return V;
  }
};

/// Result of tracing the table index back (Rust `Trace`).
struct Trace {
  enum KindT : uint8_t {
    /// index maps directly to cases 0..count; `Offset` shifts case values.
    Direct,
    /// First-level byte table maps case index -> second table index.
    TwoLevel,
    /// The compare bounds `IndexReg` itself, but the index is produced by
    /// an opaque load, so the producer chain can't be traced. The bound
    /// still applies: enumerate 0..count entries.
    DeadEnd,
    /// Couldn't tie the index back to the guard's compare.
    Unmatched,
  } Kind = Unmatched;
  Va FirstTable;
  int64_t Offset = 0;
};

struct IndexTrace {
  Trace Tr;
  /// Where the case value lives at the guard block's end.
  CmpOperand Loc;
  /// case value = read(Loc @ guard end) + LiftOff.
  int64_t LiftOff = 0;
};

/// Signed immediate value of operand 1 (iced `imm_val`).
static int64_t immVal(const JtInsn &I) {
  switch (I.ImmForm) {
  case JtImmForm::I8:
  case JtImmForm::I8Wide:
    // iced Immediate8 -> imm8 as i8; Immediate8to32/64 -> sext8. LLVM's
    // stored imm is already sign-extended from the encoded width, so both
    // reduce to the low byte's signed value.
    return (int64_t)(int8_t)I.Imm;
  case JtImmForm::I16:
    return (int64_t)(int16_t)I.Imm;
  case JtImmForm::I32:
  case JtImmForm::I32To64:
  case JtImmForm::I64:
    // iced Immediate32 -> imm32 as i32; Immediate32to64 -> sext32;
    // Immediate64 falls to the same arm in the Rust match (a latent quirk
    // reading only the low 32 bits) — reproduce it.
    return (int64_t)(int32_t)I.Imm;
  case JtImmForm::I8To16:
  case JtImmForm::None:
    // iced's match covers no arm for Immediate8to16: yields 0.
    return 0;
  }
  return 0;
}

/// Unsigned bound of a `cmp` or `sub` immediate (iced `bound_imm`). A
/// plain imm8 is read as encoded, never sign-extended: `sub dl, 0B7h; ja`
/// bounds the index by 0xB7 (184 entries), not by -73.
static std::optional<uint64_t> boundImm(const JtInsn &I) {
  switch (I.ImmForm) {
  case JtImmForm::I8:
    return (uint64_t)(uint8_t)I.Imm;
  case JtImmForm::I8Wide:
    // iced Immediate8to32 / Immediate8to64: `immediate8to32() as u64`
    // zero-extends the sign-extended low 32 bits.
    return (uint64_t)(uint32_t)I.Imm;
  case JtImmForm::I32:
  case JtImmForm::I32To64:
    return (uint64_t)(uint32_t)I.Imm;
  default:
    return std::nullopt;
  }
}

/// Does `ins`'s operand 0 match the compare operand / tracked location?
static bool cmpMatches(const CmpOperand &CmpOp, const JtInsn &I,
                       const CmpOperand &Cur) {
  if (CmpOp.Tag == CmpOperand::Kind::Reg && Cur.Tag == CmpOperand::Kind::Reg &&
      I.Op0 == JtOpKind::Reg)
    return I.Reg0 == CmpOp.Reg && Cur.Reg == CmpOp.Reg;
  if (CmpOp.Tag == CmpOperand::Kind::Mem && Cur.Tag == CmpOperand::Kind::Mem &&
      I.Op0 == JtOpKind::Mem)
    return I.MemBase == CmpOp.Base && I.MemIndex == CmpOp.Index &&
           I.MemScale == CmpOp.Scale && I.MemDisp == CmpOp.Disp;
  return false;
}

/// Scan window for a dispatch's table setup: the block's own instructions
/// plus contiguous preceding instructions. A speculative leader mid-block
/// can split the `lea`/`movsxd`/`add` sequence across block boundaries;
/// the analysis only makes sense over the undivided stream. Stops at any
/// flow-control boundary — setup instructions belong to fallthrough-
/// adjacent code.
static std::pair<size_t, size_t> blockWindow(ArrayRef<Instr> Instrs,
                                             const BasicBlock &B) {
  size_t Lo = B.InstrsBegin;
  while (Lo > 0 && Instrs[Lo - 1].nextVa() == Instrs[Lo].Addr) {
    switch (Instrs[Lo - 1].Flow) {
    case FlowKind::Return:
    case FlowKind::Branch:
    case FlowKind::CondBranch:
    case FlowKind::IndirectBranch:
    case FlowKind::Trap:
      return {Lo, B.InstrsEnd};
    default:
      --Lo;
    }
  }
  return {Lo, B.InstrsEnd};
}

/// Value loaded into `reg` by the nearest preceding `lea`/`mov imm64`.
static std::optional<uint64_t> loadedValue(const JtInsnOps &Ops,
                                           ArrayRef<Instr> Instrs, size_t Lo,
                                           size_t Hi, unsigned Reg) {
  for (size_t I = Hi; I-- > Lo;) {
    JtInsn Ins = Ops.operands(Instrs[I]);
    if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Reg)
      continue;
    if (Ins.Mn != JtMnemonic::Lea && Ins.Mn != JtMnemonic::Mov)
      continue;
    if (Ins.Op1 == JtOpKind::Mem && Ins.MemIpRel)
      return Ins.MemDisp;
    // Only a pure displacement is a constant; an index makes it an
    // address computation (`lea rdx,[rax*4]`).
    if (Ins.Op1 == JtOpKind::Mem && full(Ops, Ins.MemBase) == 0 &&
        full(Ops, Ins.MemIndex) == 0)
      return Ins.MemDisp;
    if (Ins.Op1 == JtOpKind::Imm && Ins.ImmForm == JtImmForm::I64)
      return (uint64_t)Ins.Imm;
  }
  return std::nullopt;
}

/// `loadedValue` with last-write semantics: the nearest earlier write to
/// `Reg` decides. An opaque producer (`mov r10,[r11]`, arithmetic) hides
/// every older constant, so the register is not a known table base.
static std::optional<uint64_t> strictLoadedValue(const JtInsnOps &Ops,
                                                 ArrayRef<Instr> Instrs,
                                                 size_t Lo, size_t Hi,
                                                 unsigned Reg) {
  for (size_t I = Hi; I-- > Lo;) {
    JtInsn Ins = Ops.operands(Instrs[I]);
    if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Reg)
      continue;
    if (Ins.Mn == JtMnemonic::Lea || Ins.Mn == JtMnemonic::Mov) {
      if (Ins.Op1 == JtOpKind::Mem && Ins.MemIpRel)
        return Ins.MemDisp;
      if (Ins.Op1 == JtOpKind::Mem && full(Ops, Ins.MemBase) == 0 &&
          full(Ops, Ins.MemIndex) == 0)
        return Ins.MemDisp;
      if (Ins.Op1 == JtOpKind::Imm && Ins.ImmForm == JtImmForm::I64)
        return (uint64_t)Ins.Imm;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

/// The VA-highest `lea`/`mov imm` writing `reg` that sits before `va`:
/// the load of a callee-saved table base from an earlier block. Using VA
/// order rather than decode order avoids grabbing an unrelated block's
/// write that happened to decode first.
static std::optional<uint64_t> vaNearestWrite(const JtInsnOps &Ops,
                                              ArrayRef<Instr> Instrs, Va Addr,
                                              unsigned Reg) {
  std::optional<std::pair<uint64_t, uint64_t>> Best;
  for (const Instr &It : Instrs) {
    if (It.Addr >= Addr)
      continue;
    JtInsn Ins = Ops.operands(It);
    if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Reg)
      continue;
    uint64_t V;
    if (Ins.Mn == JtMnemonic::Lea || Ins.Mn == JtMnemonic::Mov) {
      if (Ins.Op1 == JtOpKind::Mem && Ins.MemIpRel)
        V = Ins.MemDisp;
      else if (Ins.Op1 == JtOpKind::Mem && full(Ops, Ins.MemBase) == 0 &&
               full(Ops, Ins.MemIndex) == 0)
        V = Ins.MemDisp;
      else if (Ins.Op1 == JtOpKind::Imm && Ins.ImmForm == JtImmForm::I64)
        V = (uint64_t)Ins.Imm;
      else
        continue;
    } else {
      continue;
    }
    if (!Best || It.Addr.Value > Best->first)
      Best = {It.Addr.Value, V};
  }
  if (Best)
    return Best->second;
  return std::nullopt;
}

/// The value flowing into `reg` at `Blocks[Dispatch]`'s entry, proven by
/// walking the CFG backwards: every in-edge's last `lea`/`mov imm` write
/// to `reg`, recursing into a predecessor that never writes it. Returns a
/// value only when all resolved paths agree (join of equal bases); any
/// untraceable write (non-constant producer), conflicting predecessors,
/// or a walk that exceeds the bound makes it `None`. VA order can't prove
/// this — a function may load a callee-saved base several times on
/// different paths, and the VA-nearest one need not dominate the
/// dispatch.
static std::optional<uint64_t> flowBaseValue(const JtInsnOps &Ops,
                                             ArrayRef<Instr> Instrs,
                                             ArrayRef<BasicBlock> Blocks,
                                             size_t Dispatch, unsigned Reg) {
  constexpr size_t MaxSteps = 128;
  std::optional<uint64_t> Result;
  std::set<size_t> Visited;
  std::vector<size_t> Todo;
  bool Ambiguous = false;

  // Constant produced by the last write to `reg` inside `range`, or
  // `nullopt` when no write exists; `HasUntraceable` when the last write
  // isn't a constant producer.
  auto lastConstWrite = [&](size_t Lo, size_t Hi,
                            bool &HasUntraceable) -> std::optional<uint64_t> {
    HasUntraceable = false;
    for (size_t I = Hi; I-- > Lo;) {
      JtInsn Ins = Ops.operands(Instrs[I]);
      if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Reg)
        continue;
      if (Ins.Mn == JtMnemonic::Lea || Ins.Mn == JtMnemonic::Mov) {
        if (Ins.Op1 == JtOpKind::Mem && Ins.MemIpRel)
          return Ins.MemDisp;
        if (Ins.Op1 == JtOpKind::Mem && full(Ops, Ins.MemBase) == 0)
          return Ins.MemDisp;
        if (Ins.Op1 == JtOpKind::Imm && Ins.ImmForm == JtImmForm::I64)
          return (uint64_t)Ins.Imm;
      }
      HasUntraceable = true;
      return std::nullopt;
    }
    return std::nullopt;
  };

  // The producer can live inside the dispatch block itself, before the
  // tail window `loadedValue` scans (e.g. `lea` between an earlier jcc
  // and the table load). A write here overrides everything upstream.
  {
    bool Untraceable;
    std::optional<uint64_t> V = lastConstWrite(Blocks[Dispatch].InstrsBegin,
                                               Blocks[Dispatch].InstrsEnd,
                                               Untraceable);
    if (V)
      return V;
    if (Untraceable)
      return std::nullopt;
  }

  // Seed with the in-edges of the dispatch block.
  for (size_t I = 0; I < Blocks.size(); ++I) {
    if (I == Dispatch)
      continue;
    for (const Edge &E : Blocks[I].Edges)
      if (E.To == Blocks[Dispatch].Start) {
        Todo.push_back(I);
        break;
      }
  }
  size_t Steps = 0;
  while (!Todo.empty()) {
    size_t Bi = Todo.back();
    Todo.pop_back();
    if (!Visited.insert(Bi).second)
      continue;
    if (++Steps > MaxSteps)
      return std::nullopt;
    bool Untraceable;
    std::optional<uint64_t> Hit = lastConstWrite(Blocks[Bi].InstrsBegin,
                                                 Blocks[Bi].InstrsEnd,
                                                 Untraceable);
    if (Untraceable)
      return std::nullopt;
    if (Hit) {
      if (!Result)
        Result = Hit;
      else if (*Result != *Hit) {
        Ambiguous = true;
        break;
      }
    } else {
      // No write in this block: inherit from all its predecessors.
      for (size_t I = 0; I < Blocks.size(); ++I) {
        if (Visited.count(I))
          continue;
        for (const Edge &E : Blocks[I].Edges)
          if (E.To == Blocks[Bi].Start) {
            Todo.push_back(I);
            break;
          }
      }
    }
  }
  // A leftover `todo` means predecessor expansion was cut only by
  // `visited` cycles — equal-value joins are idempotent, so `result`
  // still describes every path that carried a provable write.
  if (Ambiguous)
    return std::nullopt;
  return Result;
}

/// Byte width of the memory load producing the table index register,
/// following register renames backwards inside the dispatch block.
/// `nullopt` for a non-load producer (the index could be arbitrarily
/// wide).
static std::optional<uint64_t> indexLoadWidth(const JtInsnOps &Ops,
                                              ArrayRef<Instr> Instrs,
                                              const BasicBlock &B,
                                              unsigned Index) {
  unsigned Cur = full(Ops, Index);
  auto [Lo, Hi] = blockWindow(Instrs, B);
  if (Hi == Lo)
    return std::nullopt;
  --Hi; // drop the jmp
  for (size_t I = Hi; I-- > Lo;) {
    JtInsn Ins = Ops.operands(Instrs[I]);
    if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Cur)
      continue;
    switch (Ins.Mn) {
    case JtMnemonic::Mov:
    case JtMnemonic::Movzx:
    case JtMnemonic::Movsxd:
      if (Ins.Op1 == JtOpKind::Reg)
        Cur = full(Ops, Ins.Reg1);
      else if (Ins.Op1 == JtOpKind::Mem && Ins.MemIndex == 0)
        return Ins.MemSize;
      break;
    default:
      break;
    }
  }
  return std::nullopt;
}

/// Table setup search over one instruction window (the dispatch block's
/// own window, or a trampoline predecessor's): the final
/// `add jmp_reg, a+b` (or `lea jmp_reg, [a+b]`), fed by an indexed table
/// load writing one of its operands.
static std::optional<TableAccess>
setupInWindow(const JumpTableCtx &Ctx, const JtInsnOps &Ops,
              ArrayRef<Instr> Instrs, ArrayRef<BasicBlock> Blocks,
              size_t Dispatch, const BasicBlock &Block,
              std::pair<size_t, size_t> WinRange, unsigned JmpReg) {
  size_t WLo = WinRange.first;
  size_t WHi = WinRange.second;
  if (WHi <= WLo + 1)
    return std::nullopt;
  --WHi; // drop the jmp

  // Direct absolute-indexed load: `lea idx,[base*s]` (index-only address
  // computation), `lea r,[TABLE]`, `mov jmp_reg,[r+idx]` (or with the
  // operands swapped), then the jump. GCC threaded dispatch (`-O0`)
  // never materializes an add: the table holds absolute targets.
  for (size_t I = WHi; I-- > WLo;) {
    JtInsn Ins = Ops.operands(Instrs[I]);
    if ((Ins.Mn != JtMnemonic::Mov && Ins.Mn != JtMnemonic::Movsxd) ||
        Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != JmpReg ||
        Ins.Op1 != JtOpKind::Mem || Ins.MemIndex == 0)
      continue;
    // The load must be the last write to the jump register: an `add`
    // after it rebases the entry (MSVC `mov ecx,[..]; add rcx,rdx;
    // jmp rcx` holds RVAs, not absolute targets).
    bool LaterWrite = false;
    for (size_t J = I + 1; J < WHi; ++J) {
      JtInsn X = Ops.operands(Instrs[J]);
      if (X.Op0 == JtOpKind::Reg && full(Ops, X.Reg0) == JmpReg) {
        LaterWrite = true;
        break;
      }
    }
    if (LaterWrite)
      continue;
    // Either addressing operand may hold the table base while the other
    // holds the scaled case index: `lea r,[TABLE]` + `lea idx,[base*s]`
    // with `mov jmp,[r+idx]`, or (rarely) the reverse. Try the
    // base-first reading, then the swapped one.
    unsigned LoadBase = full(Ops, Ins.MemBase);
    unsigned LoadIndex = full(Ops, Ins.MemIndex);
    if (LoadBase == 0 || LoadIndex == 0)
      continue;
    std::optional<TableAccess> Cand;
    for (unsigned TableReg : {LoadBase, LoadIndex}) {
      unsigned IndexReg = TableReg == LoadBase ? LoadIndex : LoadBase;
      // The scaled index must come from an index-only computation, not
      // a plain value: `lea idx,[base*s]`, a shift of the case value,
      // or a multiply of it. A base-relative `lea idx,[idx+K]` is a
      // case offset, not a scale, and does not qualify. The scale
      // itself is the entry size: the load is often `[table+scaled]`
      // with scale 1 while the `lea` carries the x8.
      uint32_t EntrySize = 0;
      for (size_t J = I; J-- > WLo;) {
        JtInsn P = Ops.operands(Instrs[J]);
        if (P.Mn == JtMnemonic::Lea && P.Op0 == JtOpKind::Reg &&
            full(Ops, P.Reg0) == IndexReg && P.Op1 == JtOpKind::Mem &&
            full(Ops, P.MemBase) == 0 && full(Ops, P.MemIndex) != 0) {
          EntrySize = P.MemScale;
          break;
        }
        if (P.Mn == JtMnemonic::Shl && P.Op0 == JtOpKind::Reg &&
            full(Ops, P.Reg0) == IndexReg && P.Op1 == JtOpKind::Imm) {
          EntrySize = 1u << ((uint32_t)immVal(P) & 63);
          break;
        }
        if (P.Mn == JtMnemonic::Imul && P.Op0 == JtOpKind::Reg &&
            full(Ops, P.Reg0) == IndexReg && P.Op1 == JtOpKind::Imm) {
          EntrySize = (uint32_t)immVal(P);
          break;
        }
      }
      if (EntrySize != 4 && EntrySize != 8) {
        // Fall back to the load's own scale when no computation was
        // found (plain scaled load, no separate lea).
        if (Ins.MemScale == 4 || Ins.MemScale == 8)
          EntrySize = Ins.MemScale;
        else
          continue;
      }
      auto BaseVal = loadedValue(Ops, Instrs, WLo, I, TableReg);
      if (!BaseVal)
        BaseVal = vaNearestWrite(Ops, Instrs, Block.Start, TableReg);
      if (!BaseVal || *BaseVal == Ctx.Img.Base.Value)
        continue;
      Va TableVa = Va(*BaseVal + Ins.MemDisp);
      if (!Ctx.Img.sectionOf(TableVa))
        continue;
      Cand = TableAccess{TableVa, (uint64_t)EntrySize, false, IndexReg,
                         {WinRange}};
      break;
    }
    if (Cand)
      return Cand;
  }

  // The final `add jmp_reg, a+b` (or `lea jmp_reg, [a+b]`).
  std::optional<size_t> AddI;
  for (size_t I = WHi; I-- > WLo;) {
    JtInsn Ins = Ops.operands(Instrs[I]);
    bool IsAdd = Ins.Mn == JtMnemonic::Add && Ins.Op0 == JtOpKind::Reg &&
                 full(Ops, Ins.Reg0) == JmpReg && Ins.Op1 == JtOpKind::Reg;
    bool IsLea = Ins.Mn == JtMnemonic::Lea && Ins.Op0 == JtOpKind::Reg &&
                 full(Ops, Ins.Reg0) == JmpReg && Ins.Op1 == JtOpKind::Mem &&
                 Ins.MemBase != 0 && Ins.MemIndex != 0;
    if (IsAdd || IsLea) {
      AddI = I;
      break;
    }
  }
  if (!AddI)
    return std::nullopt;
  JtInsn Add = Ops.operands(Instrs[*AddI]);
  unsigned Op0, Op1;
  if (Add.Mn == JtMnemonic::Lea) {
    Op0 = full(Ops, Add.MemBase);
    Op1 = full(Ops, Add.MemIndex);
  } else {
    Op0 = full(Ops, Add.Reg0);
    Op1 = full(Ops, Add.Reg1);
  }

  // Table load writes one operand of the add from [r + r*s + disp].
  // Either addressing operand can hold the table base: MSVC's
  // `mov ecx,[rcx+rax*4+disp]` keeps the image base in the load's base
  // register, while GCC's `mov eax,[rdx+rax]` keeps the table VA in the
  // load's *index* register (the scaled index sits in base).
  for (unsigned Loaded : {Op0, Op1}) {
    for (size_t I = *AddI; I-- > WLo;) {
      JtInsn Ins = Ops.operands(Instrs[I]);
      if (Ins.Mn != JtMnemonic::Mov && Ins.Mn != JtMnemonic::Movzx &&
          Ins.Mn != JtMnemonic::Movsxd)
        continue;
      if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Loaded)
        continue;
      if (Ins.Op1 != JtOpKind::Mem || Ins.MemIndex == 0)
        continue;

      uint64_t Base = Ctx.Img.Base.Value;
      uint64_t Disp = Ins.MemDisp;
      // Absolute indexed load `mov r,[idx*s + VA]`: the table is the
      // displacement itself.
      if (Ins.MemBase == 0 || Ins.MemIpRel) {
        Va TableVa = Va(Disp);
        if (!Ctx.Img.sectionOf(TableVa))
          continue;
        return TableAccess{TableVa, uint64_t(Ins.MemScale == 8 ? 8 : 4), false,
                           Ins.MemIndex, {WinRange}};
      }
      if (full(Ops, Ins.MemBase) != Op0 && full(Ops, Ins.MemBase) != Op1 &&
          full(Ops, Ins.MemIndex) != Op0 && full(Ops, Ins.MemIndex) != Op1)
        continue;
      // Try the load's base register as the table base first (index is
      // `memory_index`); fall back to the index register (index is then
      // `memory_base`). The table base often lives in a callee-saved reg
      // loaded many blocks earlier (`lea r15,[TABLE]` before the match).
      // Order of trust: the contiguous window, then the CFG back-walk
      // (a dominating `lea` that VA order misattributes to a closer
      // sibling path), then the VA-nearest earlier write.
      for (unsigned TableReg : {full(Ops, Ins.MemBase),
                                full(Ops, Ins.MemIndex)}) {
        auto BaseVal = loadedValue(Ops, Instrs, WLo, I, TableReg);
        if (!BaseVal)
          BaseVal =
              flowBaseValue(Ops, Instrs, Blocks, Dispatch, TableReg);
        if (!BaseVal)
          BaseVal = vaNearestWrite(Ops, Instrs, Block.Start, TableReg);
        if (!BaseVal)
          continue;
        Va TableVa;
        bool Rva;
        if (*BaseVal == Base) {
          TableVa = Va(Base + Disp);
          Rva = true;
        } else {
          TableVa = Va(*BaseVal + Disp);
          Rva = false;
        }
        if (!Ctx.Img.sectionOf(TableVa))
          continue;
        unsigned IndexReg = TableReg == full(Ops, Ins.MemBase)
                                ? Ins.MemIndex
                                : Ins.MemBase;
        return TableAccess{TableVa, uint64_t(Ins.MemScale == 8 ? 8 : 4), Rva,
                           IndexReg, {WinRange}};
      }
    }
  }
  return std::nullopt;
}

/// Identify the table access shape from the dispatch block's tail.
static std::optional<TableAccess>
describeTable(const JumpTableCtx &Ctx, const JtInsnOps &Ops,
              ArrayRef<BasicBlock> Blocks, ArrayRef<Instr> Instrs,
              size_t Dispatch, const JtInsn &Jmp) {
  const BasicBlock &Block = Blocks[Dispatch];
  // Shape A: `jmp qword ptr [rip + idx*8 + disp]` — absolute pointers.
  if (Jmp.Op0 == JtOpKind::Mem) {
    if (Jmp.MemIndex == 0)
      return std::nullopt;
    Va TableVa;
    if (Jmp.MemIpRel) {
      TableVa = Va(Jmp.MemDisp);
    } else {
      auto W = blockWindow(Instrs, Block);
      auto V = loadedValue(Ops, Instrs, W.first, W.second,
                           full(Ops, Jmp.MemBase));
      if (!V)
        return std::nullopt;
      TableVa = Va(*V);
    }
    return TableAccess{TableVa, uint64_t(Jmp.MemScale == 8 ? 8 : 4), false,
                       Jmp.MemIndex, {}};
  }

  if (Jmp.Op0 != JtOpKind::Reg)
    return std::nullopt;
  // The dispatch block's own setup window first; when the table load and
  // the final add sit in a trampoline predecessor (GCC `-O0` threaded
  // dispatch: setup, then an unconditional jump or fallthrough into a
  // shared `jmp reg` tail), each trampoline's own window describes the
  // same shape.
  std::vector<std::pair<size_t, size_t>> Wins;
  auto Own = blockWindow(Instrs, Block);
  Wins.push_back({Own.first, Block.InstrsEnd});
  // Trampoline predecessors: blocks running straight into the dispatch
  // block (fallthrough, or an unconditional jump there) whose setup
  // (table load + final add) ran just before. GCC `-O0` threaded
  // dispatch shares one `jmp reg` tail across dozens of such
  // trampolines, all feeding one table.
  for (const BasicBlock &B : Blocks) {
    if (B.Start == Block.Start)
      continue;
    bool Reaches = B.End == Block.Start;
    if (!Reaches)
      for (const Edge &E : B.Edges)
        if (E.To == Block.Start &&
            (E.Kind == EdgeKind::Jump || E.Kind == EdgeKind::Fallthrough)) {
          Reaches = true;
          break;
        }
    if (!Reaches)
      continue;
    if (B.InstrsEnd - B.InstrsBegin < 2)
      continue;
    auto W = blockWindow(Instrs, B);
    if (llvm::any_of(Wins, [&](auto &X) { return X.first == W.first; }))
      continue;
    Wins.push_back({W.first, B.InstrsEnd});
  }
  unsigned JmpReg = full(Ops, Jmp.Reg0);
  for (auto W : Wins)
    if (auto Access =
            setupInWindow(Ctx, Ops, Instrs, Blocks, Dispatch, Block, W,
                          JmpReg))
      return Access;
  return std::nullopt;
}

/// Find a CFG predecessor of `DispatchStart` whose terminator is a jcc
/// guarding the table (one successor is the dispatch block).
static std::vector<size_t> findGuards(ArrayRef<BasicBlock> Blocks,
                                      Va DispatchStart) {
  std::vector<size_t> Out;
  for (size_t I = 0; I < Blocks.size(); ++I)
    for (const Edge &E : Blocks[I].Edges)
      if (E.To == DispatchStart &&
          (E.Kind == EdgeKind::CondTaken ||
           E.Kind == EdgeKind::CondNotTaken)) {
        Out.push_back(I);
        break;
      }
  return Out;
}

/// The jcc + dominating `cmp` in the guard block give the entry count.
/// Returns (entry count, compare operand, was-sub).
static std::optional<std::tuple<uint64_t, CmpOperand, bool>>
guardBound(const JtInsnOps &Ops, ArrayRef<Instr> Instrs,
           const BasicBlock &Guard) {
  if (Guard.InstrsEnd <= Guard.InstrsBegin)
    return std::nullopt;
  JtInsn Jcc = Ops.operands(Instrs[Guard.InstrsEnd - 1]);
  if (Instrs[Guard.InstrsEnd - 1].Flow != FlowKind::CondBranch)
    return std::nullopt;
  // The cmp that sets the flags: the last compare/sub before the jcc.
  std::optional<CmpOperand> Operand;
  uint64_t Imm = 0;
  bool WasSub = false;
  for (size_t I = Guard.InstrsEnd - 1; I-- > Guard.InstrsBegin;) {
    JtInsn Ins = Ops.operands(Instrs[I]);
    if (Ins.Mn == JtMnemonic::Cmp) {
      if (Ins.Op0 == JtOpKind::Reg)
        Operand = regCmp(full(Ops, Ins.Reg0));
      else if (Ins.Op0 == JtOpKind::Mem)
        Operand = memCmp(Ins);
      else
        return std::nullopt;
      auto B = boundImm(Ins);
      if (!B)
        return std::nullopt;
      Imm = *B;
      break;
    }
    // `sub x, K; jcc` doubles as the bounds check: the subtraction's
    // carry/zero flags test `x` against `K` directly.
    if (Ins.Mn == JtMnemonic::Sub && Ins.Op1 == JtOpKind::Imm) {
      if (Ins.Op0 != JtOpKind::Reg)
        return std::nullopt;
      Operand = regCmp(full(Ops, Ins.Reg0));
      auto B = boundImm(Ins);
      if (!B)
        return std::nullopt;
      Imm = *B;
      WasSub = true;
      break;
    }
  }
  if (!Operand)
    return std::nullopt;
  // Count from the jcc direction; the jcc's out-of-range successor is
  // the default in either orientation.
  uint64_t Count;
  switch (Jcc.Cond) {
  // in-range iff x <= N
  case JtCond::A:
  case JtCond::Be:
  case JtCond::G:
  case JtCond::Le:
    Count = Imm + 1;
    break;
  // in-range iff x < N
  case JtCond::Ae:
  case JtCond::B:
  case JtCond::Ge:
  case JtCond::L:
    Count = Imm;
    break;
  default:
    return std::nullopt;
  }
  return std::tuple{Count, *Operand, WasSub};
}

/// Walk the index register back through mov/movsxd/movzx and offset ops.
/// When the index was produced by a `movzx b, byte [base + i]` load, the
/// switch is two-level: `FirstTable` = that byte table.
static IndexTrace traceIndex(const JumpTableCtx &Ctx, const JtInsnOps &Ops,
                             ArrayRef<Instr> Instrs,
                             const BasicBlock &Dispatch,
                             const TableAccess &Access,
                             std::optional<CmpOperand> CmpOp,
                             bool GuardWasSub) {
  int64_t Offset = 0;
  // `cur` is the location currently holding the index value.
  CmpOperand Cur = regCmp(full(Ops, Access.IndexReg));
  std::optional<Va> FirstTable;
  // `shift`: difference between `cur`'s value at its earliest chain point
  // and at the guard end, for in-place transforms on `cur`.
  int64_t Shift = 0;
  // `pending`: case = value(cur at scan point) + pending.
  int64_t Pending = 0;

  // The index chain lives around the dispatch: the classic lookbehind
  // covers the guard block and the dispatch block itself. When the setup
  // came from a trampoline predecessor (shared `jmp reg` tail), that
  // window holds the chain instead — trace the union so neither shape
  // loses its chain.
  size_t Classic =
      Dispatch.InstrsBegin > 48 ? Dispatch.InstrsBegin - 48 : 0;
  size_t Lo = Classic, Hi = Dispatch.InstrsEnd;
  if (Access.Origin && Access.Origin->first <= Access.Origin->second &&
      Access.Origin->second <= Instrs.size()) {
    Lo = std::min(Classic, Access.Origin->first);
    Hi = std::max(Dispatch.InstrsEnd, Access.Origin->second);
  }
  bool MatchedCmp = false;
  bool StopScan = false;
  for (size_t Pos = Hi; Pos-- > Lo && !StopScan;) {
    JtInsn Ins = Ops.operands(Instrs[Pos]);
    // The guard's compare on the tracked location (real `cmp`, or `sub`
    // when the subtraction doubles as the bound check).
    if (Ins.Mn == JtMnemonic::Cmp && CmpOp &&
        cmpMatches(*CmpOp, Ins, Cur)) {
      MatchedCmp = true;
      continue;
    }
    if (GuardWasSub && Ins.Mn == JtMnemonic::Sub &&
        Ins.Op1 == JtOpKind::Imm && Ins.Op0 == JtOpKind::Reg && CmpOp &&
        *CmpOp == regCmp(full(Ops, Ins.Reg0))) {
      MatchedCmp = true;
      continue;
    }
    if (Cur.Tag == CmpOperand::Kind::Reg) {
      if (Ins.Op0 != JtOpKind::Reg || full(Ops, Ins.Reg0) != Cur.Reg)
        continue;
      bool MovFam = Ins.Mn == JtMnemonic::Mov ||
                    Ins.Mn == JtMnemonic::Movsxd ||
                    Ins.Mn == JtMnemonic::Movzx;
      // Byte-table load into the index: MSVC two-level switch. The base
      // must resolve to readable bytes; a byte load like
      // `movzx edx,[r8+rdx-1]` (the discriminant itself) fits the shape
      // but reads runtime data, not a table — treat such producers as
      // untraceable and stop.
      if (Ins.Mn == JtMnemonic::Movzx && !FirstTable &&
          Ins.Op1 == JtOpKind::Mem && Ins.MemIndex != 0 &&
          Ins.MemScale == 1) {
        // The base must be a constant written near the load (the guard and
        // dispatch window) or the image base itself, hoisted anywhere
        // earlier. A constant from far away in decode order belongs to some
        // other switch while this register holds a runtime pointer (a text
        // buffer, a struct field).
        const unsigned BaseReg = full(Ops, Ins.MemBase);
        std::optional<uint64_t> BaseVal =
            strictLoadedValue(Ops, Instrs, Lo, Pos, BaseReg);
        if (!BaseVal) {
          BaseVal = strictLoadedValue(Ops, Instrs, 0, Pos, BaseReg);
          if (BaseVal && *BaseVal != Ctx.Img.Base.Value)
            BaseVal = std::nullopt;
        }
        // No constant base: only an RVA-shaped displacement (the base
        // register then holds the image base) can still name a table. A
        // zero displacement is a runtime pointer, such as a text buffer.
        if (!BaseVal && Ins.MemDisp == 0) {
          StopScan = true;
          break;
        }
        uint64_t Ft = BaseVal.value_or(Ctx.Img.Base.Value) + Ins.MemDisp;
        if (!Ctx.Img.read(Va(Ft), 1)) {
          StopScan = true;
          break;
        }
        FirstTable = Va(Ft);
        Cur = regCmp(full(Ops, Ins.MemIndex));
        continue;
      }
      // The nearest indexed byte load is the table; an older one (the
      // switch value itself came from a memory byte, e.g. rustc
      // `byte[idx]`) must not overwrite it.
      if (Ins.Mn == JtMnemonic::Movzx && FirstTable &&
          Ins.Op1 == JtOpKind::Mem && Ins.MemIndex != 0 &&
          Ins.MemScale == 1)
        continue;
      // The dword-table load itself (indexed, scale > 1): produces the
      // jump value, not the index — skip it.
      if (MovFam && Ins.Op1 == JtOpKind::Mem && Ins.MemIndex != 0)
        continue;
      if (MovFam && Ins.Op1 == JtOpKind::Mem) {
        // Index loaded from a memory slot: track the slot.
        Cur = memCmp(Ins);
        Shift = 0;
      } else if (MovFam && Ins.Op1 == JtOpKind::Reg) {
        Cur = regCmp(full(Ops, Ins.Reg1));
        Shift = 0;
      } else if (Ins.Mn == JtMnemonic::Sub && Ins.Op1 == JtOpKind::Imm) {
        // `sub cur, K` on the guard's operand is the bound check itself
        // (clang -O0 `sub rax,9; ja`), not a base offset.
        if (!MatchedCmp && CmpOp && *CmpOp == Cur) {
          MatchedCmp = true;
        } else {
          // cur = cur_old - K: the earlier value was larger.
          Pending -= immVal(Ins);
          Offset += immVal(Ins);
          Shift += immVal(Ins);
        }
      } else if (Ins.Mn == JtMnemonic::Add && Ins.Op1 == JtOpKind::Imm) {
        Pending += immVal(Ins);
        Offset -= immVal(Ins);
        Shift -= immVal(Ins);
      } else if (Ins.Mn == JtMnemonic::And && Ins.Op1 == JtOpKind::Imm) {
        // `and cur, MASK` caps the case value at MASK (GCC `-O0` masked
        // dispatch: `and eax,7Fh`, no `cmp` guard). The mask is a bound,
        // not a value shift: the trace stays on `cur` with no offset
        // change.
      } else if (Ins.Mn == JtMnemonic::Shr && Ins.Op1 == JtOpKind::Imm) {
        // `shr cur, K` keeps the low masked bits: no offset change
        // either.
      }
    } else {
      // Writes to the tracked memory slot.
      if (Ins.Op0 != JtOpKind::Mem || Ins.MemBase != Cur.Base ||
          Ins.MemIndex != Cur.Index || Ins.MemScale != Cur.Scale ||
          Ins.MemDisp != Cur.Disp)
        continue;
      if (Ins.Mn == JtMnemonic::Mov && Ins.Op1 == JtOpKind::Reg) {
        // `mov [m], reg` — the slot was stored from a register.
        Cur = regCmp(full(Ops, Ins.Reg1));
      } else if (Ins.Mn == JtMnemonic::Sub && Ins.Op1 == JtOpKind::Imm) {
        Offset += immVal(Ins);
      } else if (Ins.Mn == JtMnemonic::Add && Ins.Op1 == JtOpKind::Imm) {
        Offset -= immVal(Ins);
      }
    }
  }

  Trace Tr;
  if (FirstTable) {
    // A byte-table index load was seen: two-level regardless of whether
    // the index tied back to the guard's compare.
    Tr.Kind = Trace::TwoLevel;
    Tr.FirstTable = *FirstTable;
    Tr.Offset = Offset;
  } else if (MatchedCmp) {
    Tr.Kind = Trace::Direct;
    Tr.Offset = Offset;
  } else if (CmpOp &&
             *CmpOp == regCmp(full(Ops, Access.IndexReg))) {
    // The compare may bound `index_reg` even when its producer chain is
    // opaque (index computed via a data-table load): the case count
    // applies to the register directly.
    Tr.Kind = Trace::DeadEnd;
  } else {
    Tr.Kind = Trace::Unmatched;
  }
  IndexTrace It;
  It.Tr = Tr;
  // DeadEnd: the bound applies to index_reg itself, and case values are
  // the index values (opaque producer: we can't do better).
  It.Loc = Tr.Kind == Trace::DeadEnd
               ? regCmp(full(Ops, Access.IndexReg))
               : Cur;
  It.LiftOff = Tr.Kind == Trace::DeadEnd ? 0 : Pending + Shift;
  return It;
}

/// Clip an enumeration of `n` entries at the first entry (index >= 1)
/// whose address another operand references. A referenced address starts
/// the next data structure, so a table cannot run past it, whatever the
/// guard says.
static uint64_t clipAtReference(const JumpTableCtx &Ctx, uint64_t N,
                                Va Base, uint64_t Stride) {
  uint64_t Lim = std::min<uint64_t>(N, 1 << 16);
  for (uint64_t I = 1; I < Lim; ++I)
    if (Ctx.isReferenced(Base.wrappingAdd(I * Stride)))
      return I;
  return N;
}

/// Scan a case table until the first entry that isn't an executable
/// target or sits at a referenced data boundary. The span marks every
/// byte the table plausibly owns — the entry count from a guard is a
/// semantic bound, not a byte bound, and a wrong guard (a cousin `jcc`
/// picked over the real bound check) must not leave live table bytes
/// looking like code.
static uint64_t scanCaseTable(const JumpTableCtx &Ctx,
                              const TableAccess &Access) {
  uint64_t N = 0;
  for (uint64_t I = 0; I < 4096; ++I) {
    Va EntryVa = Access.Table.wrappingAdd(I * Access.EntrySize);
    if (I > 0 && Ctx.isReferenced(EntryVa))
      break;
    if (!Access.read(Ctx, I))
      break;
    ++N;
  }
  return N;
}

/// Scan a first-level byte table: bytes index case-table slots, so each
/// must stay below `case_slots`. Stops at the first out-of-range byte,
/// the first referenced boundary, unreadable bytes, or a hard cap.
static uint64_t scanByteTable(const JumpTableCtx &Ctx, Va FirstTable,
                              uint64_t CaseSlots) {
  uint64_t N = 0;
  for (uint64_t I = 0; I < 4096; ++I) {
    Va ByteVa = FirstTable.wrappingAdd(I);
    if (I > 0 && Ctx.isReferenced(ByteVa))
      break;
    auto B = Ctx.Img.read(ByteVa, 1);
    if (!B || B->empty())
      break;
    if ((uint64_t)(*B)[0] >= CaseSlots)
      break;
    ++N;
  }
  return N;
}

/// Extend a table span over a run of filler immediately past its end.
/// Linkers pad merged table clusters with `int3`/`nop`/zero fill inside
/// the same byte island, and a multi-byte `nop` between two tables of
/// one cluster counts too. One boundary is visible: the canonical MSVC
/// pad cell (`int3` run closing on a multi-byte `nop` that ends at the
/// aligned next contribution) starts the *next* contribution, so the
/// walk gives that whole cell back.
static Va extendFillerTail(const JumpTableCtx &Ctx, const JtInsnOps &Ops,
                           Va End) {
  const Section *Sec = Ctx.Img.sectionOf(End);
  if (!Sec)
    return End;
  Va Hard = std::min(Sec->end(), End.wrappingAdd(4096));
  auto Dec = Ops.fillDecoder(Ctx.Img);
  Va P = End;
  while (P < Hard) {
    if (Ctx.isReferenced(P))
      break;
    auto B = Ctx.Img.read(P, 1);
    if (!B || B->empty())
      break;
    uint8_t Byte = (*B)[0];
    if (Byte == 0x00 || Byte == 0x90 || Byte == 0xcc) {
      P = P.wrappingAdd(1);
      continue;
    }
    auto F = Dec->decodeOne(P);
    if (F && F->Filler && P.wrappingAdd(F->Size) <= Hard)
      P = P.wrappingAdd(F->Size);
    else
      break;
  }
  Va FillEnd = P;
  // A filler window that starts at the first alignment boundary past
  // the table end and closes on a multi-byte nop is the next
  // contribution's alignment pad, not this table's fill. Plain `int3`
  // runs without a closing nop stay: they are table-cluster fill.
  uint64_t CellRaw = (End.Value | 15);
  Va Cell(CellRaw == UINT64_MAX ? UINT64_MAX : CellRaw + 1);
  if (Cell < FillEnd) {
    Va Q = Cell;
    bool LastNop = false;
    bool Clean = true;
    while (Q < FillEnd) {
      auto F = Dec->decodeOne(Q);
      if (F && F->Filler && Q.wrappingAdd(F->Size) <= FillEnd) {
        LastNop = F->MultiNop;
        Q = Q.wrappingAdd(F->Size);
      } else {
        Clean = false;
        break;
      }
    }
    if (Clean && Q == FillEnd && LastNop)
      return Cell;
  }
  return FillEnd;
}

/// Entries an unbounded table enumeration accepts, in order:
/// `(enumeration index, case-table slot index, target VA)`. Stops at the
/// first unreadable entry, the first entry address some other
/// RIP-relative operand references (adjacent data structure), the first
/// target outside the function's code region, or the cap.
/// `IgnoreRefs` skips the referenced-data stop: overrun proofs only
/// exist past the table's real end, so the boundary check must not hide
/// them.
static std::vector<std::tuple<uint64_t, uint64_t, Va>>
unboundedEntries(const JumpTableCtx &Ctx, const TableAccess &Access,
                 const Trace &Tr, uint64_t Cap, bool IgnoreRefs) {
  auto inOwn = [&](Va Addr) {
    if (Ctx.CodeRanges.empty())
      return Ctx.Img.isExecutable(Addr);
    for (auto [S, E] : Ctx.CodeRanges)
      if (Addr >= S && Addr < E)
        return true;
    return false;
  };
  std::vector<std::tuple<uint64_t, uint64_t, Va>> Out;
  if (Tr.Kind == Trace::TwoLevel) {
    for (uint64_t I = 0; I < Cap; ++I) {
      Va ByteVa = Tr.FirstTable.wrappingAdd(I);
      // An entry whose address some other RIP-relative operand
      // references starts the next data structure: stop. Index 0 is
      // the table's own base and is always referenced.
      if (I > 0 && !IgnoreRefs && Ctx.isReferenced(ByteVa))
        break;
      auto B = Ctx.Img.read(ByteVa, 1);
      if (!B || B->empty())
        break;
      uint64_t B2 = (*B)[0];
      Va SlotVa = Access.Table.wrappingAdd(B2 * Access.EntrySize);
      // Slot 0 is the case table's base: self-referenced.
      if (B2 > 0 && !IgnoreRefs && Ctx.isReferenced(SlotVa))
        break;
      auto T = Access.read(Ctx, B2);
      if (!T)
        break;
      if (!inOwn(*T))
        break;
      Out.emplace_back(I, B2, *T);
    }
  } else {
    for (uint64_t I = 0; I < Cap; ++I) {
      Va EntryVa = Access.Table.wrappingAdd(I * Access.EntrySize);
      if (I > 0 && !IgnoreRefs && Ctx.isReferenced(EntryVa))
        break;
      auto T = Access.read(Ctx, I);
      if (!T)
        break;
      if (!inOwn(*T))
        break;
      Out.emplace_back(I, I, *T);
    }
  }
  return Out;
}

/// No guarding jcc: the index is trusted (rustc enum matches). Enumerate
/// entries from index 0, stopping at the first unreadable entry, the
/// first target outside the function's own code region, or the hard
/// cap. Byte and word index loads cap tighter (their value can't exceed
/// the width).
static std::optional<JumpTableInfo>
resolveUnbounded(const JumpTableCtx &Ctx, const JtInsnOps &Ops,
                 ArrayRef<Instr> Instrs, const BasicBlock &B,
                 const TableAccess &Access) {
  // Without .pdata there is no code region to bound the walk; the
  // section itself becomes the region (in_own falls back).
  uint64_t Cap = 4096;
  if (auto W = indexLoadWidth(Ops, Instrs, B, Access.IndexReg)) {
    if (*W == 1)
      Cap = std::min<uint64_t>(Cap, 256);
    else if (*W == 2)
      Cap = std::min<uint64_t>(Cap, 65536);
  }
  IndexTrace It = traceIndex(Ctx, Ops, Instrs, B, Access, std::nullopt,
                             false);
  Va EnumBase = It.Tr.Kind == Trace::TwoLevel ? It.Tr.FirstTable
                                               : Access.Table;
  auto LimIt = Ctx.Limits.find(EnumBase);
  if (LimIt != Ctx.Limits.end())
    Cap = std::min(Cap, LimIt->second);
  auto List = unboundedEntries(Ctx, Access, It.Tr, Cap, false);
  std::map<Va, std::vector<int64_t>> Targets;
  uint64_t MaxSlot = 0;
  int64_t Offset = 0;
  if (It.Tr.Kind == Trace::TwoLevel || It.Tr.Kind == Trace::Direct)
    Offset = It.Tr.Offset;
  for (auto &[I, Slot, T] : List) {
    MaxSlot = std::max(MaxSlot, Slot);
    Targets[T].push_back((int64_t)I + Offset);
  }
  uint64_t Entries = List.size();
  if (Entries == 0)
    return std::nullopt;

  JumpTableInfo Res;
  Res.Table = Access.Table;
  Res.EnumBase = EnumBase;
  Res.Guard = std::nullopt;
  Res.IndexLoc = std::pair{It.Loc, It.LiftOff};
  if (It.Tr.Kind == Trace::TwoLevel) {
    // `entries` bounded the enumeration; a scan that walks farther
    // reads sibling code as table bytes (a stale dword can decode to a
    // valid .text address and swallow the next function).
    uint64_t CaseSlots = scanCaseTable(Ctx, Access);
    uint64_t ByteSlots = std::min(
        scanByteTable(Ctx, It.Tr.FirstTable,
                      std::max(CaseSlots, MaxSlot + 1)),
        Entries);
    Va CaseEnd = extendFillerTail(
        Ctx, Ops, Access.Table.wrappingAdd(CaseSlots * Access.EntrySize));
    Va ByteEnd = extendFillerTail(
        Ctx, Ops, It.Tr.FirstTable.wrappingAdd(ByteSlots));
    Res.Spans = std::make_pair(
        VaRange{Access.Table, CaseEnd},
        std::optional<VaRange>{VaRange{It.Tr.FirstTable, ByteEnd}});
  } else {
    uint64_t Slots =
        std::min(scanCaseTable(Ctx, Access), Entries);
    Va End = extendFillerTail(
        Ctx, Ops, Access.Table.wrappingAdd(Slots * Access.EntrySize));
    Res.Spans =
        std::make_pair(VaRange{Access.Table, End}, std::optional<VaRange>{});
  }
  for (auto &[T, Vs] : Targets)
    Res.Targets.emplace_back(T, std::move(Vs));
  return Res;
}

} // end anonymous namespace

std::optional<JumpTableInfo>
resolveJumpTable(const JumpTableCtx &Ctx, const TargetHooks &Hooks,
                 ArrayRef<BasicBlock> Blocks, ArrayRef<Instr> Instrs,
                 size_t Dispatch) {
  const JtInsnOps *Ops = Hooks.jumpTableInsnOps();
  if (!Ops || Dispatch >= Blocks.size() ||
      Blocks[Dispatch].InstrsEnd == 0 ||
      Blocks[Dispatch].InstrsEnd > Instrs.size())
    return std::nullopt;
  const BasicBlock &B = Blocks[Dispatch];
  JtInsn Jmp = Ops->operands(Instrs[B.InstrsEnd - 1]);
  if (Instrs[B.InstrsEnd - 1].Flow != FlowKind::IndirectBranch)
    return std::nullopt;

  // Describe the table access; shape detection for both reg and mem
  // jumps.
  auto Access = describeTable(Ctx, *Ops, Blocks, Instrs, Dispatch, Jmp);
  if (!Access)
    return std::nullopt;

  // Guard: a predecessor block ending in a jcc that lands on this
  // block. Several jcc predecessors may exist (a shared dispatch tail,
  // or an unrelated nearby `cmp`/`jcc`); the guard is only usable when
  // the index trace ties back to its compare. Without any guard,
  // rustc-style enum matches index the table straight from the
  // discriminant with no bounds check — enumerate the table inside the
  // function's own code region instead.
  auto Guards = findGuards(Blocks, B.Start);
  std::optional<std::tuple<size_t, uint64_t, CmpOperand, bool, IndexTrace>>
      Picked;
  for (size_t Guard : Guards) {
    auto Gb = guardBound(*Ops, Instrs, Blocks[Guard]);
    if (!Gb)
      continue;
    auto &[Count, CmpOp, WasSub] = *Gb;
    IndexTrace It =
        traceIndex(Ctx, *Ops, Instrs, B, *Access, CmpOp, WasSub);
    if (It.Tr.Kind == Trace::Unmatched)
      continue;
    Picked = std::tuple{Guard, Count, CmpOp, WasSub, It};
    break;
  }
  if (!Picked)
    return resolveUnbounded(Ctx, *Ops, Instrs, B, *Access);
  auto &[Guard, Count, GuardCmpOp, GuardWasSub, It] = *Picked;
  Trace Tr = It.Tr;

  // ---- materialize case values & targets --------------------------
  // A table never spans sections, so indices past the table's section
  // end are dead reads. A wrong guard can claim billions of entries;
  // the cap keeps that from dominating analysis time.
  auto inSection = [&](Va Table, uint64_t EntrySize) -> uint64_t {
    const Section *S = Ctx.Img.sectionOf(Table);
    return S ? (S->end().Value - Table.Value) / EntrySize : 0;
  };
  std::map<Va, std::vector<int64_t>> Targets;
  JumpTableInfo Res;
  // When the two-level trace produced nothing, the byte load was
  // misclassified (a real switch never yields zero reachable cases):
  // retry against the dword table as if `idx` were the case value.
  Va EnumBase = Access->Table;
  if (Tr.Kind == Trace::Direct || Tr.Kind == Trace::DeadEnd) {
    int64_t Offset = Tr.Kind == Trace::Direct ? Tr.Offset : 0;
    uint64_t N =
        clipAtReference(Ctx,
                        std::min(Count, inSection(Access->Table,
                                                  Access->EntrySize)),
                        Access->Table, Access->EntrySize);
    for (uint64_t I = 0; I < N; ++I) {
      auto T = Access->read(Ctx, I);
      // The first entry that is not a code address ends the table: a
      // guard count borrowed from an unrelated compare must not run the
      // walk through the next data structure (adjacent tables).
      if (!T)
        break;
      Targets[*T].push_back((int64_t)I + Offset);
    }
    // Byte extent by content scan: the guard count bounds valid case
    // values AND the slot count — an entry past it that happens to
    // point into .text (a stale dword, a sibling function's code)
    // would swallow live bytes as "data".
    uint64_t Slots = std::min(scanCaseTable(Ctx, *Access), N);
    Va End = extendFillerTail(
        Ctx, *Ops,
        Access->Table.wrappingAdd(Slots * Access->EntrySize));
    Res.Spans = std::make_pair(VaRange{Access->Table, End},
                               std::optional<VaRange>{});
  } else if (Tr.Kind == Trace::TwoLevel) {
    // case value v -> first_table[v - offset] -> second-level index.
    uint64_t N = clipAtReference(
        Ctx, std::min(Count, inSection(Tr.FirstTable, 1)), Tr.FirstTable,
        1);
    uint64_t MaxB2 = 0;
    for (uint64_t I = 0; I < N; ++I) {
      Va ByteVa = Tr.FirstTable.wrappingAdd(I);
      auto B = Ctx.Img.read(ByteVa, 1);
      if (!B || B->empty())
        continue;
      uint64_t B2 = (*B)[0];
      auto T = Access->read(Ctx, B2);
      if (!T)
        continue;
      MaxB2 = std::max(MaxB2, B2);
      Targets[*T].push_back((int64_t)I + Tr.Offset);
    }
    // Spans follow the table's own bytes: the case table ends at its
    // last valid entry, the byte table at its last index under the
    // slot count — not at the guard's count, which can come from an
    // unrelated jcc and undershoot the real extent.
    uint64_t CaseSlots = scanCaseTable(Ctx, *Access);
    uint64_t ByteSlots = scanByteTable(
        Ctx, Tr.FirstTable, std::max(CaseSlots, MaxB2 + 1));
    Va CaseEnd = extendFillerTail(
        Ctx, *Ops, Access->Table.wrappingAdd(CaseSlots * Access->EntrySize));
    Va ByteEnd = extendFillerTail(
        Ctx, *Ops, Tr.FirstTable.wrappingAdd(ByteSlots));
    Res.Spans = std::make_pair(
        VaRange{Access->Table, CaseEnd},
        std::optional<VaRange>{VaRange{Tr.FirstTable, ByteEnd}});
    if (Targets.empty()) {
      Res.Spans = std::nullopt;
      uint64_t N2 = clipAtReference(
          Ctx,
          std::min(Count, inSection(Access->Table, Access->EntrySize)),
          Access->Table, Access->EntrySize);
      for (uint64_t I = 0; I < N2; ++I) {
        auto T = Access->read(Ctx, I);
        if (!T)
          continue;
        Targets[*T].push_back((int64_t)I);
      }
      if (!Targets.empty())
        Res.Spans = std::make_pair(
            VaRange{Access->Table,
                    Access->Table.wrappingAdd(N2 * Access->EntrySize)},
            std::optional<VaRange>{});
    } else {
      EnumBase = Tr.FirstTable;
    }
  } else {
    return resolveUnbounded(Ctx, *Ops, Instrs, B, *Access);
  }
  if (Targets.empty())
    return std::nullopt;
  Res.Table = Access->Table;
  Res.EnumBase = EnumBase;
  Res.Guard = Guard;
  Res.IndexLoc = std::pair{It.Loc, It.LiftOff};
  for (auto &[T, Vs] : Targets)
    Res.Targets.emplace_back(T, std::move(Vs));
  return Res;
}

std::optional<std::vector<std::pair<Va, Va>>>
jumpTableDataSpan(const JumpTableCtx &Ctx, const TargetHooks &Hooks,
                  ArrayRef<Instr> Instrs, ArrayRef<BasicBlock> Blocks,
                  size_t Dispatch) {
  const JtInsnOps *Ops = Hooks.jumpTableInsnOps();
  if (!Ops || Dispatch >= Blocks.size() ||
      Blocks[Dispatch].InstrsEnd == 0 ||
      Blocks[Dispatch].InstrsEnd > Instrs.size())
    return std::nullopt;
  const BasicBlock &B = Blocks[Dispatch];
  if (Instrs[B.InstrsEnd - 1].Flow != FlowKind::IndirectBranch)
    return std::nullopt;
  JtInsn Jmp = Ops->operands(Instrs[B.InstrsEnd - 1]);
  auto Access = describeTable(Ctx, *Ops, Blocks, Instrs, Dispatch, Jmp);
  if (!Access)
    return std::nullopt;
  uint64_t CaseSlots = scanCaseTable(Ctx, *Access);
  if (CaseSlots == 0)
    return std::nullopt;
  std::vector<std::pair<Va, Va>> Spans;
  Spans.push_back(
      {Access->Table,
       Access->Table.wrappingAdd(CaseSlots * Access->EntrySize)});
  // A two-level access has a first-level byte table; it is data too.
  IndexTrace It = traceIndex(Ctx, *Ops, Instrs, B, *Access, std::nullopt,
                             false);
  if (It.Tr.Kind == Trace::TwoLevel) {
    uint64_t ByteSlots =
        scanByteTable(Ctx, It.Tr.FirstTable, CaseSlots);
    if (ByteSlots > 0)
      Spans.push_back({It.Tr.FirstTable,
                       It.Tr.FirstTable.wrappingAdd(ByteSlots)});
  }
  // Filler right after each table in the cluster belongs to the same
  // byte island; keep gap walks from decoding it as instructions.
  for (auto &Span : Spans)
    Span.second = extendFillerTail(Ctx, *Ops, Span.second);
  return Spans;
}

std::optional<uint64_t>
bogusTableEntryIndex(const JumpTableCtx &Ctx, const TargetHooks &Hooks,
                     ArrayRef<BasicBlock> Blocks, ArrayRef<Instr> Instrs,
                     size_t Dispatch, Va EnumBase,
                     llvm::function_ref<bool(Va)> Straddles,
                     llvm::function_ref<bool(Va)> IsIndirect) {
  const JtInsnOps *Ops = Hooks.jumpTableInsnOps();
  if (!Ops || Dispatch >= Blocks.size() ||
      Blocks[Dispatch].InstrsEnd == 0 ||
      Blocks[Dispatch].InstrsEnd > Instrs.size())
    return std::nullopt;
  const BasicBlock &B = Blocks[Dispatch];
  if (Instrs[B.InstrsEnd - 1].Flow != FlowKind::IndirectBranch)
    return std::nullopt;
  JtInsn Jmp = Ops->operands(Instrs[B.InstrsEnd - 1]);
  auto Access = describeTable(Ctx, *Ops, Blocks, Instrs, Dispatch, Jmp);
  if (!Access)
    return std::nullopt;
  IndexTrace It = traceIndex(Ctx, *Ops, Instrs, B, *Access, std::nullopt,
                             false);
  Va TraceBase = It.Tr.Kind == Trace::TwoLevel ? It.Tr.FirstTable
                                              : Access->Table;
  if (TraceBase != EnumBase)
    return std::nullopt;
  uint64_t Cap = 4096;
  if (auto W = indexLoadWidth(*Ops, Instrs, B, Access->IndexReg)) {
    if (*W == 1)
      Cap = std::min<uint64_t>(Cap, 256);
    else if (*W == 2)
      Cap = std::min<uint64_t>(Cap, 65536);
  }
  auto LimIt = Ctx.Limits.find(EnumBase);
  if (LimIt != Ctx.Limits.end())
    Cap = std::min(Cap, LimIt->second);
  // The first entry whose address another operand references starts
  // the next data structure (a sibling table): this table ends there.
  // The reference may come from a block decoded after the first
  // resolution.
  std::optional<uint64_t> Referenced;
  for (uint64_t I = 1; I < std::min<uint64_t>(Cap, 1 << 16); ++I) {
    Va Addr = It.Tr.Kind == Trace::TwoLevel
                  ? It.Tr.FirstTable.wrappingAdd(I)
                  : Access->Table.wrappingAdd(I * Access->EntrySize);
    if (Ctx.isReferenced(Addr)) {
      Referenced = I;
      break;
    }
  }
  std::optional<uint64_t> Overrun;
  for (auto &[I, Slot, T] :
       unboundedEntries(Ctx, *Access, It.Tr, Cap, true)) {
    (void)Slot;
    if (Straddles(T) || IsIndirect(T)) {
      Overrun = I;
      break;
    }
  }
  if (Overrun && Referenced)
    return std::min(*Overrun, *Referenced);
  if (Overrun)
    return Overrun;
  return Referenced;
}

} // end namespace revenant
