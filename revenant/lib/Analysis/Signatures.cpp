//==- lib/Analysis/Signatures.cpp - Win64 signature recovery --------------==//
//
// Port of crates/sigs: interprocedural signature recovery over the asm
// CFG, 1:1 with the Rust reference (PDB override path dropped; the fork
// never reads the PDB inside the pipeline).
//
// The Rust code queries iced-x86's `InstructionInfoFactory` for per-
// operand register access. The fork decodes to `MCInst`, so the first
// section rebuilds iced's exact reporting on top of `MCInstrDesc`:
//   * explicit defs -> Write, widened: a 32-bit GPR write reports the
//     full 64-bit register, a vector write reports the zmm (iced
//     `add_register` widening; liveness groups registers anyway).
//   * explicit uses -> Read at operand width; a tied use is the read
//     half of a read-write (the def side already emitted the write).
//   * memory operand slots (base/scale/index/disp/segment) -> address
//     regs are reads; value access comes from mayLoad/mayStore.
//   * implicit uses/defs -> Read / widened Write; use+def overlap is
//     ReadWrite -> narrow Read + widened Write, except the cbw/cwd/cdq/
//     cqo family where iced reports the accumulator as read-only.
//   * `grp()` = iced `full_register()`: gpr8/16/32 -> r64, xmm/ymm ->
//     zmm. Watching xmm0-3 while grouping to zmm makes the xmm param/
//     return path dead in the reference; reproduced faithfully here.
//
// `frameModel` is a private subset of crates/analysis stackptr.rs
// (per-instruction rsp deltas + rbp frame-pointer delta). MuseStackPtr
// owns the canonical `analyzeStack`; swap when it lands — the seam is
// `frameModel()` only.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Signatures.h"

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include <array>

#include <cassert>
#include <cctype>
#include <memory>
#include <string>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/X86TargetParser.h"

#include "X86BaseInfo.h"

using namespace llvm;

namespace revenant {
namespace {

//==--------------------------------------------------------------------==//
// Shared MC tables (stateless; a second copy next to Disasm's own is
// fine — the generated tables are identical).
//==--------------------------------------------------------------------==//

struct McTables {
  std::unique_ptr<Triple> TT;
  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCInstrInfo> MII;
  /// reg -> iced `full_register()` (r64 for GPRs, zmm for vectors).
  DenseMap<unsigned, unsigned> Full;
  /// reg -> width in bits.
  DenseMap<unsigned, unsigned> Bits;

  static const McTables &get() {
    static const McTables T = make();
    return T;
  }

private:
  static McTables make() {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86Disassembler();

    McTables T;
    T.TT = std::make_unique<Triple>("x86_64-pc-windows-msvc");
    std::string Err;
    const Target *Tgt = TargetRegistry::lookupTarget(*T.TT, Err);
    assert(Tgt && "x86 target missing");
    T.MRI.reset(Tgt->createMCRegInfo(*T.TT));
    T.MII.reset(Tgt->createMCInstrInfo());
    assert(T.MRI && T.MII);

    // Group leader map: every sub-register of a GR64 member -> the GR64
    // member; every sub-register of a VR512 member -> the zmm.
    auto &MRI = *T.MRI;
    auto MapFamily = [&](const MCRegisterClass &RC) {
      for (MCPhysReg Root : RC) {
        T.Full[Root] = Root;
        for (MCSubRegIterator S(Root, &MRI); S.isValid(); ++S)
          T.Full[*S] = Root;
      }
    };
    MapFamily(MRI.getRegClass(X86::GR64RegClassID));
    MapFamily(MRI.getRegClass(X86::VR512RegClassID));

    // Widths: the widest known reg class the reg belongs to.
    struct ClassBits {
      unsigned Id, Width;
    };
    static const ClassBits Classes[] = {
        {X86::GR8RegClassID, 8},        {X86::GR8_ABCD_HRegClassID, 8},
        {X86::GR8_NOREXRegClassID, 8},  {X86::GR16RegClassID, 16},
        {X86::GR16_NOREXRegClassID, 16},{X86::GR32RegClassID, 32},
        {X86::GR32_NOREXRegClassID, 32},{X86::GR64RegClassID, 64},
        {X86::GR64_NOREXRegClassID, 64},
        {X86::SEGMENT_REGRegClassID, 16},
        {X86::FR32XRegClassID, 32},     {X86::FR32RegClassID, 32},
        {X86::FR64XRegClassID, 64},     {X86::FR64RegClassID, 64},
        {X86::VR128XRegClassID, 128},   {X86::VR128RegClassID, 128},
        {X86::VR256XRegClassID, 256},   {X86::VR256RegClassID, 256},
        {X86::VR512RegClassID, 512},    {X86::VK1RegClassID, 1},
        {X86::VK2RegClassID, 4},        {X86::VK4RegClassID, 4},
        {X86::VK8RegClassID, 8},        {X86::VK16RegClassID, 16},
        {X86::VK32RegClassID, 32},      {X86::VK64RegClassID, 64},
    };
    for (const ClassBits &C : Classes) {
      const MCRegisterClass &RC = MRI.getRegClass(C.Id);
      for (MCPhysReg R : RC)
        T.Bits[R] = std::max(T.Bits.lookup(R), C.Width);
    }
    return T;
  }
};

/// iced `full_register()`: the register's widest group leader.
static unsigned grp(unsigned R) {
  const McTables &T = McTables::get();
  auto It = T.Full.find(R);
  return It == T.Full.end() ? R : It->second;
}

/// Reported width (bits) of a register operand.
static unsigned regBits(unsigned R) {
  const McTables &T = McTables::get();
  auto It = T.Bits.find(R);
  if (It != T.Bits.end())
    return It->second;
  // Untyped regs (RIP, EFLAGS, pseudo regs): fall back to the group
  // leader's width, else 64 so misses stay conservative.
  unsigned G = grp(R);
  It = T.Bits.find(G);
  return It == T.Bits.end() ? 64 : It->second;
}

/// iced `add_register` write widening: 32-bit GPR writes report the
/// 64-bit register; vector writes report the zmm; narrow GPR and
/// non-GPR writes report as-is.
static unsigned widenWrite(unsigned R) {
  const McTables &T = McTables::get();
  if (T.MRI->getRegClass(X86::GR32RegClassID).contains(R) ||
      T.MRI->getRegClass(X86::GR32_NOREXRegClassID).contains(R) ||
      T.MRI->getRegClass(X86::FR32XRegClassID).contains(R) ||
      T.MRI->getRegClass(X86::FR64XRegClassID).contains(R) ||
      T.MRI->getRegClass(X86::VR128XRegClassID).contains(R) ||
      T.MRI->getRegClass(X86::VR128RegClassID).contains(R) ||
      T.MRI->getRegClass(X86::VR256XRegClassID).contains(R) ||
      T.MRI->getRegClass(X86::VR256RegClassID).contains(R) ||
      T.MRI->getRegClass(X86::VR512RegClassID).contains(R))
    return grp(R);
  return R;
}

/// The cbw/cwd/cdq/cqo accumulator family: LLVM marks the accumulator
/// as an implicit def too; iced reports it as a pure read.
static bool isSignExtendFamily(StringRef Name) {
  return Name == "CBW" || Name == "CWDE" || Name == "CDQE" ||
         Name == "CWD" || Name == "CDQ" || Name == "CQO";
}

//==--------------------------------------------------------------------==//
// Per-instruction usage (iced `used_registers` / `used_memory`)
//==--------------------------------------------------------------------==//

struct RegUse {
  unsigned Reg;
  bool Read, Write;
};

struct MemOp {
  unsigned Base = 0, Index = 0, Seg = 0;
  int64_t Disp = 0;
  bool HasGroup = false;
};

struct InsnInfo {
  SmallVector<RegUse, 8> Regs;
  SmallVector<MemOp, 2> Mems;
  /// Value access of the explicit memory operand(s): None/Read/Write/RW.
  uint8_t MemRead : 1, MemWrite : 1;
  /// Bits of the explicit memory operand's value (iced `memory_size()`).
  unsigned MemBits = 0;
};

/// Data width (bits) of the instruction's memory operand.
/// iced's `memory_size()`: prefer the `m<N>` name suffix (movzx64rm8 ->
/// 8), then a leading operand-size digit (CMP64rm -> 64), then the
/// paired register operand's width (MOVSDmr's FR64 src -> 64), else 64.
static unsigned memSizeBits(const MCInstrDesc &D, const MCInst &I,
                            int MemStart, const McTables &T) {
  StringRef N = T.MII->getName(I.getOpcode());
  if (MemStart >= 0) {
    // "m" followed by digits inside the name.
    for (size_t P = 0; (P = N.find('m', P)) != StringRef::npos; ++P) {
      size_t Q = P + 1, E = Q;
      while (E < N.size() && isdigit(N[E]))
        ++E;
      if (E > Q) {
        unsigned V = (unsigned)std::stoul(N.slice(Q, E).str());
        if (V == 8 || V == 16 || V == 32 || V == 64 || V == 80 ||
            V == 128 || V == 256 || V == 512)
          return V;
      }
    }
    // Leading operand-size digits (CMP64rm, ADD64mi8, ...).
    size_t E = 0;
    while (E < N.size() && !isdigit(N[E]))
      ++E;
    size_t B = E;
    while (E < N.size() && isdigit(N[E]))
      ++E;
    if (E > B) {
      unsigned V = (unsigned)std::stoul(N.slice(B, E).str());
      if (V >= 8 && V <= 512)
        return V;
    }
    // Paired register operand (the data side of mr/rm).
    int Bias = (int)X86II::getOperandBias(D);
    for (unsigned i = 0; i < I.getNumOperands(); ++i) {
      if ((int)i >= MemStart + Bias &&
          (int)i < MemStart + Bias + (int)X86::AddrNumOperands)
        continue;
      const MCOperand &Op = I.getOperand(i);
      if (Op.isReg() && Op.getReg() != 0) {
        unsigned W = regBits(Op.getReg());
        if (W >= 8 && W <= 512)
          return W;
      }
    }
  }
  return 64;
}

static InsnInfo insnInfo(const MCInst &I, const McTables &T) {
  InsnInfo Info;
  Info.MemRead = Info.MemWrite = 0;
  const MCInstrDesc &D = T.MII->get(I.getOpcode());
  StringRef Name = T.MII->getName(I.getOpcode());
  unsigned NumDefs = D.getNumDefs();
  int MemNo = X86II::getMemoryOperandNo(D.TSFlags);
  int MemStart = -1;
  if (MemNo >= 0)
    MemStart = MemNo + (int)X86II::getOperandBias(D);

  SmallVector<RegUse, 8> &R = Info.Regs;
  auto Emit = [&](unsigned Reg, bool Read, bool Write) {
    R.push_back({Reg, Read, Write});
  };

  for (unsigned i = 0; i < I.getNumOperands(); ++i) {
    const MCOperand &Op = I.getOperand(i);
    bool InMem = MemStart >= 0 && (int)i >= MemStart &&
                 (int)i < MemStart + (int)X86::AddrNumOperands;
    if (InMem && Op.isReg() && Op.getReg() != 0) {
      // Address computation registers are reads; the memory value's
      // access is tracked separately.
      Emit(Op.getReg(), true, false);
      continue;
    }
    if (Op.isReg() && Op.getReg() != 0) {
      unsigned Reg = Op.getReg();
      if (i < NumDefs) {
        // Def side: the write (widened, iced `add_register`).
        Emit(widenWrite(Reg), false, true);
      } else {
        // Use side: read at operand width. A tied use is the read half
        // of a read-write; the write was already emitted by the def.
        Emit(Reg, true, false);
      }
    }
  }

  // iced `command_lea`: when the lea destination is narrow, address
  // registers are reported at the destination's width family.
  if (Name.starts_with("LEA") && !R.empty() && I.getNumOperands() > 0 &&
      I.getOperand(0).isReg()) {
    unsigned Dst = I.getOperand(0).getReg();
    const MCRegisterInfo &MRI = *T.MRI;
    bool D32 = MRI.getRegClass(X86::GR32RegClassID).contains(Dst);
    bool D16 = MRI.getRegClass(X86::GR16RegClassID).contains(Dst);
    if (D32 || D16) {
      unsigned Wanted = D32 ? X86::GR32RegClassID : X86::GR16RegClassID;
      for (size_t K = 1; K < R.size(); ++K) {
        for (unsigned Sub = X86::sub_32bit; Sub <= X86::sub_8bit; ++Sub) {
          // Find the subregister of the reported 64-bit reg in the
          // wanted family; sub_32bit/sub_16bit cover it.
          (void)Sub;
          break;
        }
        unsigned S = MRI.getMatchingSuperReg(
            R[K].Reg, D32 ? X86::sub_32bit : X86::sub_16bit,
            &MRI.getRegClass(Wanted));
        if (S)
          R[K].Reg = S;
      }
    }
  }

  // Memory operand group(s): base/scale/index/disp/segment.
  if (MemStart >= 0 && MemStart + X86::AddrNumOperands <=
                           (int)I.getNumOperands()) {
    MemOp M;
    M.HasGroup = true;
    M.Base = I.getOperand(MemStart + X86::AddrBaseReg).isReg()
                 ? I.getOperand(MemStart + X86::AddrBaseReg).getReg().id()
                 : 0;
    M.Index = I.getOperand(MemStart + X86::AddrIndexReg).isReg()
                  ? I.getOperand(MemStart + X86::AddrIndexReg).getReg().id()
                  : 0;
    M.Seg = I.getOperand(MemStart + X86::AddrSegmentReg).isReg()
                ? I.getOperand(MemStart + X86::AddrSegmentReg).getReg().id()
                : 0;
    M.Disp = I.getOperand(MemStart + X86::AddrDisp).isImm()
                 ? I.getOperand(MemStart + X86::AddrDisp).getImm()
                 : 0;
    Info.Mems.push_back(M);
    Info.MemBits = memSizeBits(D, I, MemNo, T);
  }
  Info.MemRead = D.mayLoad() ? 1 : 0;
  Info.MemWrite = D.mayStore() ? 1 : 0;
  // iced reports the branch-target slot of `call [mem]` / `jmp [mem]`
  // as a memory read.
  if (MemStart >= 0 && (D.isCall() || D.isIndirectBranch()))
    Info.MemRead = 1;

  // Implicit register lists.
  SmallSet<unsigned, 8> ImpUses;
  for (MCPhysReg U : D.implicit_uses()) {
    ImpUses.insert(U);
    Emit(U, true, false);
  }
  bool SignExt = isSignExtendFamily(Name);
  for (MCPhysReg Def : D.implicit_defs()) {
    // cbw/cwd/cdq/cqo: iced keeps the accumulator as a pure read.
    if (SignExt && ImpUses.count(Def))
      continue;
    Emit(widenWrite(Def), false, true);
  }
  // LLVM models `enter` without implicit regs; iced reports the frame
  // setup (rsp/rbp read-write). Rare in compiler output; keep parity.
  if (Name == "ENTER") {
    Emit(X86::RSP, true, false);
    Emit(X86::RBP, true, false);
    Emit(X86::RSP, false, true);
    Emit(X86::RBP, false, true);
  }
  return Info;
}

static bool anyWrite(const InsnInfo &II, unsigned Reg) {
  for (const RegUse &U : II.Regs)
    if (grp(U.Reg) == Reg && U.Write)
      return true;
  return false;
}

//==--------------------------------------------------------------------==//
// FuncRecord -> fat Function (mirrors analysis::Program::materialize)
//==--------------------------------------------------------------------==//

static EdgeKind edgeKind(EdgeTag T) {
  switch (T) {
  case EdgeTag::Fallthrough:   return EdgeKind::Fallthrough;
  case EdgeTag::Jump:          return EdgeKind::Jump;
  case EdgeTag::CondTaken:     return EdgeKind::CondTaken;
  case EdgeTag::CondNotTaken:  return EdgeKind::CondNotTaken;
  case EdgeTag::Switch:        return EdgeKind::Switch;
  case EdgeTag::SwitchDefault: return EdgeKind::SwitchDefault;
  case EdgeTag::TailCall:      return EdgeKind::TailCall;
  }
  return EdgeKind::Fallthrough;
}

static Function materialize(const Program &P, const FuncRecord &R,
                            const Disasm &Dis) {
  const Image &Img = *P.Img;
  Function F;
  F.Start = R.Start;
  F.End = R.End;
  F.Name = R.Name;
  F.HasPdata = R.HasPdata;
  F.Instrs.reserve(R.InstrRvas.size());
  for (Rva R : R.InstrRvas) {
    // Rust substitutes a default (zero) Instruction on decode failure.
    if (auto I = Dis.decodeOne(Img.vaOfRva(R)))
      F.Instrs.push_back(*I);
    else {
      Instr Z;
      Z.Addr = Va(0);
      F.Instrs.push_back(Z);
    }
  }
  F.Blocks.reserve(R.Blocks.size());
  for (const BlockRec &B : R.Blocks) {
    BasicBlock BB;
    BB.Start = B.startVa(Img);
    BB.End = B.endVa(Img);
    BB.InstrsBegin = B.InstrsBegin;
    BB.InstrsEnd = B.InstrsEnd;
    for (size_t E = B.EdgesBegin; E < B.EdgesEnd; ++E) {
      Edge Ed;
      Ed.To = R.edgeTo(Img, E);
      Ed.Kind = edgeKind(R.Edges[E].Kind);
      BB.Edges.push_back(std::move(Ed));
    }
    BB.EndKind = B.EndKind;
    BB.SwitchIndex = R.blockSwitchIndex(B);
    F.Blocks.push_back(std::move(BB));
  }
  F.Chunks = R.Chunks;
  F.UnresolvedIndirects = R.UnresolvedIndirects;
  F.IsNoreturn = R.IsNoreturn;
  F.NoreturnCalls = R.NoreturnCalls;
  F.CallTargets = R.CallTargets;
  F.ExtraRoots = R.ExtraRoots;
  F.Orphan = R.Orphan;
  return F;
}

/// Materialized-function cache (`Program::function` re-materializes on
/// demand in Rust; same observable result, shared across call sites).
struct FnCache {
  const Program &P;
  const Disasm &Dis;
  std::map<Va, std::optional<Function>> Memo;

  const Function *get(Va Start) {
    auto It = Memo.find(Start);
    if (It == Memo.end()) {
      auto R = P.Functions.find(Start);
      It = Memo.emplace(
               Start, R == P.Functions.end()
                          ? std::optional<Function>()
                          : std::optional<Function>(materialize(P, R->second, Dis)))
           .first;
    }
    return It->second ? &*It->second : nullptr;
  }
};

//==--------------------------------------------------------------------==//
// Frame model — the subset of analysis::analyze_stack that stack_args
// consumes: rsp delta at every instruction VA + the rbp frame-pointer
// delta. Returns nullopt on the Inconsistent join error (the Rust code
// drops stack info the same way via `.ok()`).
//==--------------------------------------------------------------------==//

struct FrameModel {
  std::map<Va, int64_t> Deltas;
  std::optional<int64_t> RbpDelta;
  std::optional<Va> DynVa;

  int64_t deltaAt(Va Addr) const {
    auto It = Deltas.find(Addr);
    return It == Deltas.end() ? 0 : It->second;
  }
  /// Frame offset of `[rsp + disp]` at `va` (entry-rsp relative).
  int64_t rspOff(Va Addr, int64_t Disp) const {
    return Disp - deltaAt(Addr);
  }
  /// Frame offset of `[rbp + disp]` when an fp exists.
  std::optional<int64_t> rbpOff(int64_t Disp) const {
    if (!RbpDelta)
      return std::nullopt;
    return Disp - *RbpDelta;
  }
};

static bool isChkstk(const Program &P, Va Addr) {
  if (P.Externs)
    for (const auto &[Slot, E] : *P.Externs)
      if (E.Slot == Addr && E.Name.find("chkstk") != std::string::npos)
        return true;
  if (P.Names) {
    auto It = P.Names->find(Addr);
    if (It != P.Names->end() &&
        It->second.find("chkstk") != std::string::npos)
      return true;
  }
  return false;
}

/// Last immediate operand, sign-extended to 64 bits at its encoded size
/// (iced `immediate8to32`/`immediate32to64` semantics).
static std::optional<int64_t> lastImm(const MCInst &I,
                                      const McTables &T) {
  const MCInstrDesc &D = T.MII->get(I.getOpcode());
  for (int i = (int)I.getNumOperands() - 1; i >= 0; --i) {
    const MCOperand &Op = I.getOperand(i);
    if (!Op.isImm())
      continue;
    int64_t V = Op.getImm();
    // Sign-extend by the encoded immediate width.
    unsigned Sz = X86II::getSizeOfImm(D.TSFlags);
    if (Sz == 1)
      return (int64_t)(int8_t)V;
    if (Sz == 2)
      return (int64_t)(int16_t)V;
    if (Sz == 4)
      return (int64_t)(int32_t)V;
    return V;
  }
  return std::nullopt;
}

static std::optional<FrameModel> frameModel(const Program &P,
                                            const Function &F,
                                            const McTables &T) {
  FrameModel M;
  std::map<Va, int64_t> BlockIn;
  SmallVector<std::pair<Va, int64_t>, 16> Work;
  Work.push_back({F.Start, 0});
  std::optional<Va> Dyn;

  while (!Work.empty()) {
    auto Item = Work.pop_back_val();
    Va Start = Item.first;
    int64_t InDelta = Item.second;
    auto Bit = std::find_if(F.Blocks.begin(), F.Blocks.end(),
                            [&](const BasicBlock &B) {
                              return B.Start == Start;
                            });
    if (Bit == F.Blocks.end())
      continue;
    auto Known = BlockIn.find(Start);
    if (Known != BlockIn.end()) {
      if (Known->second != InDelta)
        return std::nullopt; // StackError::Inconsistent
      continue;
    }
    BlockIn[Start] = InDelta;
    const BasicBlock &B = *Bit;
    int64_t Delta = InDelta;
    std::optional<int64_t> ChkstkSize;
    for (size_t i = B.InstrsBegin; i < B.InstrsEnd && i < F.Instrs.size();
         ++i) {
      const Instr &Ins = F.Instrs[i];
      M.Deltas[Ins.Addr] = Delta;
      const MCInst &MI = Ins.Inst;
      StringRef Name = T.MII->getName(MI.getOpcode());

      // Frame pointer acquisition (first wins).
      if (!M.RbpDelta && MI.getNumOperands() >= 1 &&
          MI.getOperand(0).isReg()) {
        unsigned D0 = MI.getOperand(0).getReg();
        if (Name == "MOV64rr" && D0 == X86::RBP &&
            MI.getNumOperands() > 1 && MI.getOperand(1).isReg() &&
            MI.getOperand(1).getReg() == X86::RSP)
          M.RbpDelta = Delta;
        else if (Name.starts_with("LEA") && D0 == X86::RBP) {
          InsnInfo II = insnInfo(MI, T);
          if (!II.Mems.empty() && II.Mems[0].Base == X86::RSP &&
              II.Mems[0].Index == 0)
            M.RbpDelta = Delta - II.Mems[0].Disp;
        }
      }

      // `mov eax, imm` feeding a possible `call __chkstk`.
      if (Name == "MOV32ri" && MI.getNumOperands() >= 2 &&
          MI.getOperand(0).isReg() && MI.getOperand(0).getReg() == X86::EAX &&
          MI.getOperand(1).isImm())
        if (auto V = lastImm(MI, T))
          ChkstkSize = V;

      if (Ins.Flow == FlowKind::Call && Ins.HasTarget &&
          isChkstk(P, Ins.Target)) {
        if (ChkstkSize)
          Delta += *ChkstkSize;
        else if (!Dyn)
          Dyn = Ins.Addr;
        continue;
      }

      // `leave` / `mov rsp, rbp` restore the frame pointer's delta.
      if (Name == "LEAVE64" ||
          (Name == "MOV64rr" && MI.getNumOperands() >= 2 &&
           MI.getOperand(0).isReg() && MI.getOperand(0).getReg() == X86::RSP &&
           MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == X86::RBP)) {
        if (M.RbpDelta) {
          Delta = *M.RbpDelta;
          continue;
        }
      }

      // Persistent rsp delta (Rust `apply`).
      bool WritesRsp = false;
      if (MI.getNumOperands() >= 1 && MI.getOperand(0).isReg() &&
          grp(MI.getOperand(0).getReg()) == X86::RSP) {
        unsigned NumDefs = T.MII->get(MI.getOpcode()).getNumDefs();
        bool Def = 0 < NumDefs;
        if (!Def) {
          // tied-use RMW forms count op0 as a use too
          Def = T.MII->get(MI.getOpcode())
                    .getOperandConstraint(0, MCOI::TIED_TO) >= 0;
        }
        WritesRsp = Def;
      }
      bool Handled = true;
      if (Name.starts_with("PUSH") || Name == "PUSHF64") {
        Delta += 8;
      } else if ((Name.starts_with("POP") && !Name.starts_with("POPCNT")) ||
                 Name == "POPF64") {
        Delta -= 8;
      } else if (Name.starts_with("SUB") && WritesRsp) {
        if (auto V = lastImm(MI, T))
          Delta += *V;
        else
          Handled = false;
      } else if (Name.starts_with("ADD") && WritesRsp) {
        if (auto V = lastImm(MI, T))
          Delta -= *V;
        else
          Handled = false;
      } else if (Name.starts_with("LEA") && WritesRsp) {
        InsnInfo II = insnInfo(MI, T);
        if (!II.Mems.empty() && II.Mems[0].Base == X86::RSP &&
            II.Mems[0].Index == 0)
          Delta -= II.Mems[0].Disp;
        else
          Handled = false;
      } else if (WritesRsp) {
        Handled = false; // non-constant rsp write
      }
      if (!Handled && !Dyn)
        Dyn = Ins.Addr;
    }
    M.DynVa = M.DynVa ? M.DynVa : Dyn;

    // Successors (Return / TailCall / Noreturn blocks emit none; the
    // TailCall edge kind is filtered out like Rust's edge match).
    switch (B.EndKind) {
    case BlockEnd::Return:
    case BlockEnd::TailCall:
    case BlockEnd::Noreturn:
      break;
    default:
      for (const Edge &E : B.Edges) {
        switch (E.Kind) {
        case EdgeKind::Fallthrough:
        case EdgeKind::Jump:
        case EdgeKind::CondTaken:
        case EdgeKind::CondNotTaken:
        case EdgeKind::SwitchDefault:
        case EdgeKind::Switch:
          Work.push_back({E.To, Delta});
          break;
        case EdgeKind::TailCall:
          break;
        }
      }
    }
  }
  return M;
}

//==--------------------------------------------------------------------==//
// Liveness / width queries (direct ports)
//==--------------------------------------------------------------------==//

static const unsigned GRP[4] = {X86::RCX, X86::RDX, X86::R8, X86::R9};
static const unsigned XRP[4] = {X86::XMM0, X86::XMM1, X86::XMM2,
                                X86::XMM3};

using RegSet = SmallSet<unsigned, 8>;

/// Per-block gen (read-before-write) / kill (written) for `watched`.
static void blockGenKill(const Function &F, const McTables &T,
                         ArrayRef<unsigned> Watched,
                         std::vector<RegSet> &Gen,
                         std::vector<RegSet> &Kill) {
  Gen.clear();
  Kill.clear();
  SmallSet<unsigned, 8> Watch(Watched.begin(), Watched.end());
  for (const BasicBlock &B : F.Blocks) {
    RegSet G, K;
    for (size_t i = B.InstrsBegin;
         i < B.InstrsEnd && i < F.Instrs.size(); ++i) {
      InsnInfo II = insnInfo(F.Instrs[i].Inst, T);
      for (const RegUse &U : II.Regs) {
        unsigned R = grp(U.Reg);
        if (!Watch.count(R))
          continue;
        if (U.Read && !K.count(R))
          G.insert(R);
        if (U.Write)
          K.insert(R);
      }
    }
    Gen.push_back(std::move(G));
    Kill.push_back(std::move(K));
  }
}

/// Live-in registers at block entries: in = gen u (out - kill).
static std::vector<RegSet> liveIn(const Function &F,
                                  const std::vector<RegSet> &Gen,
                                  const std::vector<RegSet> &Kill) {
  size_t N = F.Blocks.size();
  std::vector<RegSet> In(N);
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t Bi = 0; Bi < N; ++Bi) {
      RegSet S = Gen[Bi];
      for (const Edge &E : F.Blocks[Bi].Edges) {
        // Only intra-function edges carry liveness.
        if (E.Kind == EdgeKind::TailCall)
          continue;
        size_t Si = N;
        for (size_t J = 0; J < N; ++J)
          if (F.Blocks[J].Start == E.To) {
            Si = J;
            break;
          }
        if (Si == N)
          continue;
        for (unsigned R : In[Si])
          if (!Kill[Bi].count(R))
            S.insert(R);
      }
      if (In[Bi] != S) {
        In[Bi] = S;
        Changed = true;
      }
    }
  }
  return In;
}

static size_t entryBlock(const Function &F) {
  for (size_t i = 0; i < F.Blocks.size(); ++i)
    if (F.Blocks[i].Start == F.Start)
      return i;
  return 0;
}

using HomeSpillReads = std::map<Va, std::array<bool, 4>>;

static bool isHomeSpillRead(const HomeSpillReads *Spills, Va Addr,
                            unsigned Reg);
/// Widest read width of `reg` among reads that can still see the
/// parameter value.
static unsigned widestRead(const Function &F, const McTables &T, unsigned Reg,
                           const HomeSpillReads *Spills = nullptr,
                           bool *Observed = nullptr) {
  if (Observed)
    *Observed = false;
  size_t N = F.Blocks.size();
  std::vector<bool> Valid(N, false), Kills(N, false);
  Valid[entryBlock(F)] = true;
  for (size_t Bi = 0; Bi < N; ++Bi) {
    const BasicBlock &B = F.Blocks[Bi];
    for (size_t i = B.InstrsBegin;
         i < B.InstrsEnd && i < F.Instrs.size() && !Kills[Bi]; ++i)
      Kills[Bi] = anyWrite(insnInfo(F.Instrs[i].Inst, T), Reg);
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t Bi = 0; Bi < N; ++Bi) {
      for (const Edge &E : F.Blocks[Bi].Edges) {
        size_t Si = N;
        for (size_t J = 0; J < N; ++J)
          if (F.Blocks[J].Start == E.To) {
            Si = J;
            break;
          }
        if (Si == N)
          continue;
        bool V = Valid[Bi] && !Kills[Bi];
        if (V && !Valid[Si]) {
          Valid[Si] = true;
          Changed = true;
        }
      }
    }
  }
  unsigned Bits = 0;
  for (size_t Bi = 0; Bi < N; ++Bi) {
    if (!Valid[Bi])
      continue;
    const BasicBlock &B = F.Blocks[Bi];
    for (size_t i = B.InstrsBegin;
         i < B.InstrsEnd && i < F.Instrs.size(); ++i) {
      const Instr &Ins = F.Instrs[i];
      InsnInfo II = insnInfo(Ins.Inst, T);
      bool Stop = false;
      for (const RegUse &U : II.Regs) {
        if (grp(U.Reg) != Reg)
          continue;
        if (U.Read && !isHomeSpillRead(Spills, Ins.Addr, U.Reg)) {
          Bits = std::max(Bits, regBits(U.Reg));
          if (Observed)
            *Observed = true;
        }
        if (U.Write)
          Stop = true;
      }
      if (Stop)
        break;
    }
  }
  return std::max(Bits, 8u);
}

/// Widest write width of `reg` among blocks that end in `ret`; falls
/// back to all writes.
static unsigned widestWrite(const Function &F, const McTables &T,
                            unsigned Reg) {
  unsigned Bits = 0;
  for (const BasicBlock &B : F.Blocks) {
    if (B.EndKind != BlockEnd::Return)
      continue;
    for (size_t i = B.InstrsEnd; i-- > B.InstrsBegin && i < F.Instrs.size();) {
      const Instr &Ins = F.Instrs[i];
      InsnInfo II = insnInfo(Ins.Inst, T);
      bool Wrote = false;
      for (const RegUse &U : II.Regs) {
        if (grp(U.Reg) == Reg && U.Write) {
          // A call's implicit rax write isn't the return write; keep
          // scanning past it (defensive: iced reports none).
          if (Ins.Flow == FlowKind::Call)
            continue;
          Bits = std::max(Bits, regBits(U.Reg));
          Wrote = true;
        }
      }
      if (Wrote)
        break;
    }
  }
  if (Bits == 0) {
    for (const Instr &Ins : F.Instrs) {
      InsnInfo II = insnInfo(Ins.Inst, T);
      for (const RegUse &U : II.Regs)
        if (grp(U.Reg) == Reg && U.Write)
          Bits = std::max(Bits, regBits(U.Reg));
    }
  }
  return std::max(Bits, 8u);
}

/// Stack-arg slots read (frame offsets >= 0x28) and their widest sizes.
static std::map<int64_t, uint32_t> stackArgs(const Function &F,
                                             const McTables &T,
                                             const FrameModel *Stack) {
  std::map<int64_t, uint32_t> Args;
  if (!Stack)
    return Args;
  for (const Instr &Ins : F.Instrs) {
    InsnInfo II = insnInfo(Ins.Inst, T);
    for (const MemOp &M : II.Mems) {
      std::optional<int64_t> Off;
      if (M.Base == X86::RSP || M.Base == X86::ESP)
        Off = Stack->rspOff(Ins.Addr, M.Disp);
      else if (M.Base == X86::RBP || M.Base == X86::EBP)
        Off = Stack->rbpOff(M.Disp);
      if (!Off || *Off < 0x28)
        continue;
      // Only reads count as parameters.
      if (!II.MemRead)
        continue;
      Args[*Off] = std::max(Args[*Off], (uint32_t)II.MemBits);
    }
  }
  return Args;
}


struct VarArgsInfo {
  bool Detected = false;
  HomeSpillReads Spills;
};

/// Detect the Win64 va_start setup that saves each register argument in its
/// corresponding home slot.
static VarArgsInfo detectVarArgs(const Function &F, const McTables &T,
                                 const FrameModel *Frame) {
  VarArgsInfo Info;
  if (!Frame)
    return Info;
  bool Seen[4] = {false, false, false, false};
  for (const Instr &Ins : F.Instrs) {
    InsnInfo II = insnInfo(Ins.Inst, T);
    if (!II.MemWrite)
      continue;
    for (const MemOp &M : II.Mems) {
      if (M.Index != 0)
        continue;
      std::optional<int64_t> Off;
      if (M.Base == X86::RSP || M.Base == X86::ESP)
        Off = Frame->rspOff(Ins.Addr, M.Disp);
      else if (M.Base == X86::RBP || M.Base == X86::EBP)
        Off = Frame->rbpOff(M.Disp);
      if (!Off || *Off < 8 || *Off > 32 || (*Off - 8) % 8 != 0)
        continue;
      unsigned K = static_cast<unsigned>((*Off - 8) / 8);
      for (const RegUse &U : II.Regs) {
        if (!U.Read || grp(U.Reg) != GRP[K])
          continue;
        Info.Spills[Ins.Addr][K] = true;
        Seen[K] = true;
      }
    }
  }
  Info.Detected = std::all_of(std::begin(Seen), std::end(Seen),
                              [](bool V) { return V; });
  if (!Info.Detected)
    Info.Spills.clear();
  return Info;
}
static bool isHomeSpillRead(const HomeSpillReads *Spills, Va Addr,
                            unsigned Reg) {
  if (!Spills)
    return false;
  auto It = Spills->find(Addr);
  if (It == Spills->end())
    return false;
  for (unsigned K = 0; K < 4; ++K)
    if (It->second[K] && grp(Reg) == GRP[K])
      return true;
  return false;
}
/// Is `reg` written on every path to `ret`? (must-define fixpoint)
static bool mustDefine(const Function &F, const McTables &T,
                       unsigned Reg) {
  size_t N = F.Blocks.size();
  std::vector<bool> Defs(N, false), MustIn(N, false);
  for (size_t Bi = 0; Bi < N; ++Bi) {
    const BasicBlock &B = F.Blocks[Bi];
    for (size_t i = B.InstrsBegin;
         i < B.InstrsEnd && i < F.Instrs.size(); ++i)
      if (anyWrite(insnInfo(F.Instrs[i].Inst, T), Reg)) {
        Defs[Bi] = true;
        break;
      }
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t Bi = 0; Bi < N; ++Bi) {
      const BasicBlock &B = F.Blocks[Bi];
      bool Need;
      switch (B.EndKind) {
      case BlockEnd::Return:
      case BlockEnd::TailCall:
        Need = true;
        break;
      case BlockEnd::Noreturn:
      case BlockEnd::UnresolvedIndirect:
      case BlockEnd::EndOfFunction:
        Need = false;
        break;
      default:
        Need = !B.Edges.empty();
        for (const Edge &E : B.Edges) {
          size_t Si = N;
          for (size_t J = 0; J < N; ++J)
            if (F.Blocks[J].Start == E.To) {
              Si = J;
              break;
            }
          if (Si == N || !MustIn[Si]) {
            Need = false;
            break;
          }
        }
      }
      Need = Need && !Defs[Bi];
      if (MustIn[Bi] != Need) {
        MustIn[Bi] = Need;
        Changed = true;
      }
    }
  }
  return !MustIn[entryBlock(F)];
}

/// Direct call sites by callee: (caller start, index of the call in its
/// instrs).
using CallSites = std::map<Va, SmallVector<std::pair<Va, uint32_t>, 2>>;

static CallSites callSites(const Program &P, FnCache &Fns) {
  CallSites Out;
  for (const auto &[Start, Rec] : P.Functions) {
    const Function *G = Fns.get(Start);
    for (size_t I = 0; G && I < G->Instrs.size(); ++I) {
      const Instr &Ins = G->Instrs[I];
      if (Ins.Flow != FlowKind::Call)
        continue;
      if (Ins.HasTarget)
        Out[Ins.Target].push_back({Start, (uint32_t)I});
    }
  }
  return Out;
}

/// Does any caller read `reg` right after a call to `callee`?
static bool callerReads(const CallSites &Sites, FnCache &Fns,
                        const McTables &T, Va Callee, unsigned Reg) {
  auto It = Sites.find(Callee);
  if (It == Sites.end())
    return false;
  for (auto [Start, Ii] : It->second) {
    const Function *G = Fns.get(Start);
    if (!G || Ii >= G->Instrs.size())
      continue;
    const BasicBlock *B = G->blockContaining(G->Instrs[Ii].Addr);
    if (!B)
      continue;
    size_t End = std::min(B->InstrsEnd, G->Instrs.size());
    for (size_t J = std::min<size_t>(Ii + 1, End); J < End; ++J) {
      const Instr &Nx = G->Instrs[J];
      InsnInfo II = insnInfo(Nx.Inst, T);
      for (const RegUse &U : II.Regs)
        if (grp(U.Reg) == Reg && U.Read)
          return true;
      // Stop scanning at the next call: it clobbers rax.
      if (Nx.Flow == FlowKind::Call)
        break;
    }
  }
  return false;
}

/// Widest width at which any caller reads `reg` right after calling
/// `callee` (bits). 0 = no observed read.
static unsigned callerReadWidth(const CallSites &Sites, FnCache &Fns,
                                const McTables &T, Va Callee,
                                unsigned Reg) {
  unsigned Bits = 0;
  auto It = Sites.find(Callee);
  if (It == Sites.end())
    return 0;
  for (auto [Start, Ii] : It->second) {
    const Function *G = Fns.get(Start);
    if (!G || Ii >= G->Instrs.size())
      continue;
    const BasicBlock *B = G->blockContaining(G->Instrs[Ii].Addr);
    if (!B)
      continue;
    size_t End = std::min(B->InstrsEnd, G->Instrs.size());
    bool Stop = false;
    for (size_t J = std::min<size_t>(Ii + 1, End); J < End && !Stop; ++J) {
      const Instr &Nx = G->Instrs[J];
      InsnInfo II = insnInfo(Nx.Inst, T);
      for (const RegUse &U : II.Regs) {
        if (grp(U.Reg) == Reg && U.Read) {
          Bits = std::max(Bits, regBits(U.Reg));
          Stop = true;
        }
        if (grp(U.Reg) == Reg && U.Write)
          Stop = true;
      }
      if (Nx.Flow == FlowKind::Call)
        break;
    }
  }
  return Bits;
}

/// Parameters seen written (rcx..r9 prefix) before a call site.
static unsigned callsiteArgs(const Function &G, const McTables &T,
                             Va CallIp) {
  const BasicBlock *B = G.blockContaining(CallIp);
  if (!B)
    return 0;
  bool Written[4] = {false, false, false, false};
  size_t End = std::min(B->InstrsEnd, G.Instrs.size());
  for (size_t i = B->InstrsBegin; i < End; ++i) {
    const Instr &Ins = G.Instrs[i];
    if (Ins.Addr == CallIp)
      break;
    if (Ins.Flow == FlowKind::Call) {
      Written[0] = Written[1] = Written[2] = Written[3] = false;
      continue;
    }
    InsnInfo II = insnInfo(Ins.Inst, T);
    for (const RegUse &U : II.Regs) {
      unsigned R = grp(U.Reg);
      for (int K = 0; K < 4; ++K)
        if (GRP[K] == R && U.Write)
          Written[K] = true;
    }
  }
  unsigned N = 0;
  while (N < 4 && Written[N])
    ++N;
  return N;
}

} // end anonymous namespace

//==--------------------------------------------------------------------==//
// analyze — all functions + externs
//==--------------------------------------------------------------------==//

Signatures analyzeSignatures(const Program &P, const Disasm &Dis) {
  const McTables &T = McTables::get();
  FnCache Fns{P, Dis, {}};
  CallSites Sites = callSites(P, Fns);

  Signatures Out;
  static const unsigned Watched[8] = {X86::RCX,  X86::RDX,  X86::R8,
                                      X86::R9,   X86::XMM0, X86::XMM1,
                                      X86::XMM2, X86::XMM3};

  for (const auto &[Start, Rec] : P.Functions) {
    const Function *Fp = Fns.get(Start);
    if (!Fp)
      continue;
    const Function &F = *Fp;
    std::optional<FrameModel> Frame = frameModel(P, F, T);

    std::vector<RegSet> Gen, Kill;
    blockGenKill(F, T, Watched, Gen, Kill);
    std::vector<RegSet> Live = liveIn(F, Gen, Kill);
    RegSet EntryLive;
    if (!Live.empty())
      EntryLive = Live[entryBlock(F)];

    VarArgsInfo VarArgs =
        detectVarArgs(F, T, Frame ? &*Frame : nullptr);
    Signature Sig;
    Sig.VarArgs = VarArgs.Detected;
    for (int K = 0; K < 4; ++K) {
      unsigned R = GRP[K], Xr = XRP[K];
      bool IntLive = EntryLive.count(R);
      bool XmmLive = EntryLive.count(Xr);
      unsigned ReadBits = 0;
      bool HasSemanticRead = false;
      if (IntLive)
        ReadBits = widestRead(F, T, R,
                              VarArgs.Detected ? &VarArgs.Spills : nullptr,
                              &HasSemanticRead);
      if (VarArgs.Detected && !HasSemanticRead)
        IntLive = false;
      if (XmmLive && !IntLive) {
        Param P_;
        P_.Loc = ParamLoc::reg(Xr);
        P_.Bits = 128;
        P_.Float = true;
        Sig.Params.push_back(P_);
      } else if (IntLive) {
        Param P_;
        P_.Loc = ParamLoc::reg(R);
        P_.Bits = ReadBits;
        Sig.Params.push_back(P_);
      }
    }
    for (auto &[Off, Bits] : stackArgs(F, T, Frame ? &*Frame : nullptr)) {
      Param P_;
      P_.Loc = ParamLoc::stack(Off);
      P_.Bits = Bits;
      Sig.Params.push_back(P_);
    }

    bool RaxRet = mustDefine(F, T, X86::RAX) &&
                  callerReads(Sites, Fns, T, F.Start, X86::RAX);
    bool XmmRet = mustDefine(F, T, X86::XMM0) &&
                  callerReads(Sites, Fns, T, F.Start, X86::XMM0);
    // Return width: what callers actually read post-call when narrower
    // than the widest write; otherwise the widest write feeding rets.
    unsigned RaxRetBits = 0;
    {
      unsigned W = widestWrite(F, T, X86::RAX);
      unsigned C = callerReadWidth(Sites, Fns, T, F.Start, X86::RAX);
      RaxRetBits = (C > 0 && C < W) ? C : W;
    }
    if (XmmRet) {
      Sig.HasRet = true;
      Sig.Ret.Float = true;
      Sig.Ret.Bits = std::clamp(widestWrite(F, T, X86::XMM0), 32u, 128u);
    } else if (RaxRet) {
      Sig.HasRet = true;
      Sig.Ret.Bits = std::clamp(RaxRetBits, 8u, 64u);
    }
    // A function that never returns has no ret.
    if (F.IsNoreturn)
      Sig.HasRet = false;
    Sig.Noreturn = F.IsNoreturn;
    Sig.SigSource = Signature::Source::Inferred;
    Out.Funcs[Start] = std::move(Sig);
  }

  // Externs + indirect call-site arg prefixes: observe every call site,
  // then fold in function order (later sites win, as before).
  struct ExternObs {
    Va Slot;
    const Extern *Ext;
    unsigned Nargs;
  };
  struct LocalObs {
    Va Ip;
    unsigned N;
    bool Indirect;
  };
  for (const auto &[Start, Rec] : P.Functions) {
    const Function *G = Fns.get(Start);
    if (!G)
      continue;
    for (const Instr &Ins : G->Instrs) {
      if (Ins.Flow != FlowKind::Call)
        continue;
      InsnInfo II = insnInfo(Ins.Inst, T);
      std::optional<Va> Slot;
      for (const MemOp &M : II.Mems) {
        if (M.Index != 0)
          continue;
        // RIP-relative needs the resolved address; absolute/stack
        // slots use the raw displacement.
        if (M.Base == X86::RIP) {
          if (Ins.HasMemTarget)
            Slot = Ins.MemTarget;
        } else {
          Slot = Va((uint64_t)M.Disp);
        }
        break;
      }
      const Extern *Ext = nullptr;
      Va ExtSlot;
      if (Slot && P.Externs) {
        auto It = P.Externs->find(*Slot);
        if (It != P.Externs->end()) {
          Ext = &It->second;
          ExtSlot = It->first;
        }
      }
      if (!Ext && Ins.HasTarget) {
        // Call to a local thunk `jmp [iat]`? Mirrors the Rust lookup:
        // it uses the thunk's raw displacement (not the RIP-resolved
        // address), so this effectively never fires for RIP-relative
        // thunks; kept faithful.
        if (const Function *T2 = Fns.get(Ins.Target))
          if (T2->Instrs.size() == 1 &&
              T.MII->getName(T2->Instrs[0].Inst.getOpcode()) == "JMP64m") {
            InsnInfo TI = insnInfo(T2->Instrs[0].Inst, T);
            if (!TI.Mems.empty() && P.Externs) {
              Va S2((uint64_t)TI.Mems[0].Disp);
              auto It = P.Externs->find(S2);
              if (It != P.Externs->end()) {
                Ext = &It->second;
                ExtSlot = It->first;
              }
            }
          }
      }
      unsigned N = callsiteArgs(*G, T, Ins.Addr);
      if (Ext) {
        Signature &Entry = Out.Externs[ExtSlot];
        if (Entry.SigSource != Signature::Source::Inferred ||
            (Entry.Params.empty() && Entry.Ret.Bits == 0 &&
             !Entry.HasRet && !Entry.Noreturn)) {
          // First observation initializes (Rust or_insert_with).
          if (Entry.Params.empty() && !Entry.HasRet &&
              Entry.Ret.Bits == 0 && !Entry.Noreturn) {
            Entry.Noreturn = Ext->Noreturn;
            if (!Ext->Noreturn) {
              Entry.HasRet = true;
              Entry.Ret.Float = false;
              Entry.Ret.Bits = 64;
            }
            Entry.SigSource = Signature::Source::Inferred;
          }
        }
        if (Entry.Params.size() < N) {
          Entry.Params.clear();
          for (unsigned K = 0; K < N; ++K) {
            Param P_;
            P_.Loc = ParamLoc::reg(GRP[K]);
            P_.Bits = 64;
            Entry.Params.push_back(P_);
          }
        }
      } else {
        Out.CallsiteArgs[Ins.Addr] = N;
        bool Indirect = !Ins.HasTarget;
        if (Indirect)
          Out.IndirectArgs[Ins.Addr] = N;
      }
    }
  }
  return Out;
}

} // end namespace revenant
