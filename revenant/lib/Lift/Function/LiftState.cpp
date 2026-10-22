//===- LiftState.cpp - per-function x86-64 lift state ---------------------===//
//
// Register file, flags, frame model, operand decoding, globals, opaque
// fallback — the state half of crates/lift (regs.rs, mem.rs, frame.rs,
// flags.rs, opaque.rs) against MCInst operands. Terminators and calls live
// in LiftFunction.cpp.
//
//===----------------------------------------------------------------------===//

#include "LiftState.h"
#include "LiftX86.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"

#include "X86MCTargetDesc.h"
#include "X86BaseInfo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace revenant {
namespace lift {

//===--------------------------------------------------------------------===//
// X86MCTools
//===--------------------------------------------------------------------===//

const X86MCTools &X86MCTools::get() {
  static const X86MCTools Tools = [] {
    X86MCTools T2;
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    Triple TT("x86_64-pc-windows-msvc");
    std::string Err;
    T2.T = TargetRegistry::lookupTarget(TT, Err);
    if (T2.T) {
      T2.MRI.reset(T2.T->createMCRegInfo(TT));
      T2.MII.reset(T2.T->createMCInstrInfo());
    }
    return T2;
  }();
  return Tools;
}

//===--------------------------------------------------------------------===//
// Register tables
//===--------------------------------------------------------------------===//

namespace {

/// The 15 tracked GPR roots (all but RSP/RIP), in the Rust order.
const MCRegister GprRoots[] = {
    X86::RAX, X86::RCX, X86::RDX, X86::RBX, X86::RSI, X86::RDI, X86::RBP,
    X86::R8,  X86::R9,  X86::R10, X86::R11, X86::R12, X86::R13, X86::R14,
    X86::R15};

struct RootInfo {
  bool Tracked = false;
  unsigned Off = 0;
  unsigned Bits = 0;
};
/// Width of R (8/16/32/64) from register-class containment.
/// MC-level MCRegisterInfo has no getMinimalPhysRegClass (that lives on
/// CodeGen's TRI), so probe the X86 GR classes directly.
unsigned regWidthOf(MCRegister R) {
  const MCRegisterInfo &MRI = *X86MCTools::get().MRI;
  unsigned NumClasses = MRI.getNumRegClasses();
  auto widthOf = [&](int ClassID) -> unsigned {
    if (ClassID < 0 || (unsigned)ClassID >= NumClasses)
      return 0;
    const MCRegisterClass &RC = MRI.getRegClass((unsigned)ClassID);
    return RC.contains(R) ? RC.getSizeInBits() : 0;
  };
  // X86 enum values (X86::GR8RegClassID etc.) come from the generated
  // register-info header; probe by containment instead of hardcoding ids.
  for (unsigned C = 0; C < NumClasses; ++C) {
    const MCRegisterClass &RC = MRI.getRegClass(C);
    if (RC.contains(R) && (RC.getSizeInBits() == 8 ||
                            RC.getSizeInBits() == 16 ||
                            RC.getSizeInBits() == 32 ||
                            RC.getSizeInBits() == 64))
      return RC.getSizeInBits();
  }
  (void)widthOf;
  if (X86II::isXMMReg(R) || X86II::isYMMReg(R) || X86II::isZMMReg(R))
    return 128;
  return 64;
}

/// (root, offset, bits) for every subregister of the tracked GPRs, computed
/// from the MC sub/super iterators plus class widths. MC-level
/// MCRegisterInfo has no getSubRegIdxOffset/Size (those live on CodeGen's
/// TRI); offsets here come from the nesting structure: 32/16/8-low at 0,
/// AH/CH/DH/BH at 8.
const DenseMap<MCRegister, RootInfo> &gprInfo() {
  static const auto *T = [] {
    auto *M = new DenseMap<MCRegister, RootInfo>();
    const MCRegisterInfo &MRI = *X86MCTools::get().MRI;
    for (MCRegister Root : GprRoots) {
      (*M)[Root] = {true, 0, 64};
      for (MCSubRegIterator S(Root, &MRI); S.isValid(); ++S) {
        MCRegister Sub = *S;
        unsigned Bits = regWidthOf(Sub);
        unsigned Off = 0;
        // High-byte regs (AH/CH/DH/BH) sit at bit 8; everything else
        // reachable from a 64-bit root sits at 0.
        if (Bits == 8) {
          StringRef N = MRI.getName(Sub);
          if (N == "AH" || N == "CH" || N == "DH" || N == "BH")
            Off = 8;
        }
        (*M)[Sub] = {true, Off, Bits};
      }
    }
    return M;
  }();
  return *T;
}

/// The 64-bit GPR root a register belongs to, or NoRegister.
MCRegister gprRootOf(MCRegister R) {
  const auto &M = gprInfo();
  auto It = M.find(R);
  if (It == M.end() || !It->second.Tracked)
    return MCRegister();
  // Walk supers to the tracked root.
  const MCRegisterInfo &MRI = *X86MCTools::get().MRI;
  MCRegister Root = R;
  for (MCSuperRegIterator SI(R, &MRI); SI.isValid(); ++SI)
    Root = *SI;
  return Root;
}

bool isVectorReg(MCRegister R) {
  return X86II::isXMMReg(R) || X86II::isYMMReg(R) || X86II::isZMMReg(R);
}

/// xmm index 0-15 for a vector register; none for xmm16+/non-vector.
std::optional<unsigned> xmmSlotOf(MCRegister R) {
  unsigned N;
  if (X86II::isXMMReg(R))
    N = (unsigned)R - (unsigned)X86::XMM0;
  else if (X86II::isYMMReg(R))
    N = (unsigned)R - (unsigned)X86::YMM0;
  else if (X86II::isZMMReg(R))
    N = (unsigned)R - (unsigned)X86::ZMM0;
  else
    return std::nullopt;
  if (N < 16)
    return N;
  return std::nullopt;
}

} // namespace

std::optional<unsigned> LiftState::xmmSlot(MCRegister R) {
  return xmmSlotOf(R);
}

bool lift::isTrackedGpr(MCRegister R) {
  auto It = gprInfo().find(R);
  return It != gprInfo().end() && It->second.Tracked;
}

MCRegister lift::rootRegOf(MCRegister R, const MCRegisterInfo &) {
  return gprRootOf(R);
}

std::pair<unsigned, unsigned>
lift::regOffsetBits(MCRegister R, const MCRegisterInfo &) {
  auto It = gprInfo().find(R);
  if (It == gprInfo().end() || !It->second.Tracked)
    return {0, 64};
  return {It->second.Off, It->second.Bits};
}

bool lift::isHigh8(MCRegister R) {
  auto It = gprInfo().find(R);
  return It != gprInfo().end() && It->second.Off == 8 && It->second.Bits == 8;
}

std::string lift::sanitizeName(StringRef Name) {
  std::string S;
  S.reserve(Name.size());
  for (char C : Name) {
    if (isAlnum(C) || C == '_' || C == '.') {
      S.push_back(C);
    } else {
      // Encode special chars as _XX hex to avoid collisions
      // (e.g. operator== vs operator!= both become operator__).
      char Buf[4];
      snprintf(Buf, sizeof(Buf), "_%02x", (unsigned char)C);
      S += Buf;
    }
  }
  if (S.empty())
    S = "f";
  return S;
}

//===--------------------------------------------------------------------===//
// Logical operands
//===--------------------------------------------------------------------===//

std::vector<LogOp> lift::logicalOps(const MCInst &MI, const MCInstrDesc &Desc) {
  std::vector<LogOp> Out;
  unsigned NumDefs = Desc.getNumDefs();
  unsigned MIPos = 0;

  // lea64mem and friends carry no OPERAND_MEMORY type; the address group is
  // the second descriptor operand.
  unsigned Opc = MI.getOpcode();
  bool IsLea = Opc == X86::LEA16r || Opc == X86::LEA32r || Opc == X86::LEA64r ||
               Opc == X86::LEA64_32r;
  auto kindAt = [&](unsigned DescOp) -> LogOpKind {
    if (DescOp >= Desc.NumOperands)
      return LogOpKind::Invalid;
    if (IsLea && DescOp == 1)
      return LogOpKind::Mem;
    uint8_t OT = Desc.operands()[DescOp].OperandType;
    if (OT == MCOI::OPERAND_REGISTER)
      return LogOpKind::Reg;
    if (OT == MCOI::OPERAND_MEMORY)
      return LogOpKind::Mem;
    return LogOpKind::Imm;
  };
  auto logicalKindAt = [&](unsigned DescOp) {
    LogOpKind K = kindAt(DescOp);
    if (K == LogOpKind::Mem &&
        MIPos + X86::AddrNumOperands > MI.getNumOperands()) {
      const MCOperand &O = MI.getOperand(MIPos);
      return O.isReg()    ? LogOpKind::Reg
             : O.isExpr() ? LogOpKind::Invalid
                          : LogOpKind::Imm;
    }
    return K;
  };

  // outs: first NumDefs desc operands == first NumDefs MI operands (a def
  // memory operand still occupies the 5-operand group).
  for (unsigned D = 0; D < NumDefs && MIPos < MI.getNumOperands(); ++D) {
    LogOpKind K = logicalKindAt(D);
    Out.push_back({K, MIPos});
    MIPos += (K == LogOpKind::Mem) ? X86::AddrNumOperands : 1;
    if (K == LogOpKind::Mem)
      D += X86::AddrNumOperands - 1;
  }
  // Decoded MCInsts carry tied inputs ($src1 = $dst) as real operands; they
  // name the same register as their def, so they merge into it and the
  // logical view matches the iced one (`add rax, rcx` is two operands).
  for (unsigned D = NumDefs; D < Desc.NumOperands; ++D) {
    if (MIPos >= MI.getNumOperands())
      break;
    if (Desc.getOperandConstraint(D, MCOI::TIED_TO) != -1) {
      ++MIPos;
      continue;
    }
    LogOpKind K = logicalKindAt(D);
    Out.push_back({K, MIPos});
    MIPos += (K == LogOpKind::Mem) ? X86::AddrNumOperands : 1;
    if (K == LogOpKind::Mem)
      D += X86::AddrNumOperands - 1;
  }
  // Variable operands: whatever is left.
  while (MIPos < MI.getNumOperands()) {
    const MCOperand &O = MI.getOperand(MIPos);
    Out.push_back({O.isReg()    ? LogOpKind::Reg
                   : O.isExpr() ? LogOpKind::Invalid
                                : LogOpKind::Imm,
                   MIPos});
    ++MIPos;
  }
  return Out;
}

//===--------------------------------------------------------------------===//
// analyzeStack (private port of crates/analysis/stackptr.rs)
//===--------------------------------------------------------------------===//

namespace {

const char *opNameOf(const Instr &I, const X86MCTools &MC) {
  return MC.MII->getName(I.Inst.getOpcode()).data();
}
StringRef opName(const Instr &I, const X86MCTools &MC) {
  return MC.MII->getName(I.Inst.getOpcode());
}

bool opIsReg(const Instr &I, unsigned L, MCRegister R,
             const std::vector<LogOp> &Ops) {
  return L < Ops.size() && Ops[L].Kind == LogOpKind::Reg &&
         I.Inst.getOperand(Ops[L].MI).getReg() == R;
}
bool opIsRegAny(const Instr &I, unsigned L, const std::vector<LogOp> &Ops) {
  return L < Ops.size() && Ops[L].Kind == LogOpKind::Reg;
}
MCRegister regAt(const Instr &I, unsigned L, const std::vector<LogOp> &Ops) {
  return I.Inst.getOperand(Ops[L].MI).getReg();
}
bool opIsImm(const Instr &I, unsigned L, const std::vector<LogOp> &Ops) {
  return L < Ops.size() && Ops[L].Kind == LogOpKind::Imm;
}
int64_t immAt(const Instr &I, unsigned L, const std::vector<LogOp> &Ops) {
  return I.Inst.getOperand(Ops[L].MI).getImm();
}
bool opIsMem(const Instr &I, unsigned L, const std::vector<LogOp> &Ops) {
  return L < Ops.size() && Ops[L].Kind == LogOpKind::Mem;
}
/// Decompose the 5-operand memory group at logical operand L.
void memParts(const Instr &I, unsigned L, const std::vector<LogOp> &Ops,
              MCRegister &Base, int64_t &Scale, MCRegister &Index,
              int64_t &Disp, MCRegister &Seg) {
  unsigned P = Ops[L].MI;
  Base = I.Inst.getOperand(P + X86::AddrBaseReg).getReg();
  Scale = I.Inst.getOperand(P + X86::AddrScaleAmt).getImm();
  Index = I.Inst.getOperand(P + X86::AddrIndexReg).getReg();
  Disp = I.Inst.getOperand(P + X86::AddrDisp).getImm();
  Seg = I.Inst.getOperand(P + X86::AddrSegmentReg).getReg();
}
bool isRsp(MCRegister R) { return R == X86::RSP || R == X86::ESP; }
bool isRbp(MCRegister R) { return R == X86::RBP || R == X86::EBP; }

/// Is `va` an import thunk for `__chkstk`?
bool isChkstk(const Program &P, Va Addr) {
  if (P.Externs)
    for (auto &[Slot, E] : *P.Externs)
      if (E.Name.find("chkstk") != std::string::npos && E.Slot == Addr)
        return true;
  if (P.Names)
    if (auto It = P.Names->find(Addr); It != P.Names->end())
      return It->second.find("chkstk") != std::string::npos;
  return false;
}

} // namespace

llvm::Expected<StackInfo> lift::analyzeStack(const Program &P,
                                             const FuncRecord &F,
                                             const Disasm &D,
                                             ArrayRef<Instr> Instrs) {
  const X86MCTools &MC = X86MCTools::get();
  StackInfo Out;
  std::map<Va, int64_t> BlockIn;
  std::vector<std::pair<Va, int64_t>> Work = {{F.Start, 0}};

  auto err = [&](Va At, int64_t Have, int64_t Want) {
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        "inconsistent rsp delta " + Twine(Have) + " vs " + Twine(Want) +
            " at block " + Twine::utohexstr(At.Value));
  };

  while (!Work.empty()) {
    auto [Start, InDelta] = Work.back();
    Work.pop_back();
    const BlockRec *B = nullptr;
    for (const BlockRec &BB : F.Blocks)
      if (BB.startVa(*P.Img) == Start) {
        B = &BB;
        break;
      }
    if (!B)
      continue;
    if (auto It = BlockIn.find(Start); It != BlockIn.end()) {
      // Paths that reach a block at different rsp depths (shrink-wrapped
      // epilogues, Go stack-growth stubs) keep the first depth seen; the
      // function still lifts, with frame offsets right for that path.
      if (It->second != InDelta)
        Out.JoinMismatches++;
      continue;
    }
    BlockIn[Start] = InDelta;
    int64_t Delta = InDelta;
    std::optional<int64_t> ChkstkSize;

    auto bump = [&](int64_t D2, const Instr &I) {
      auto Ops = logicalOps(I.Inst, MC.MII->get(I.Inst.getOpcode()));
      for (unsigned L = 0; L < Ops.size(); ++L) {
        if (Ops[L].Kind != LogOpKind::Mem)
          continue;
        MCRegister Base, Index, Seg;
        int64_t Scale, Disp;
        memParts(I, L, Ops, Base, Scale, Index, Disp, Seg);
        std::optional<int64_t> Off;
        if (isRsp(Base))
          Off = Disp - D2;
        else if (isRbp(Base) && Out.RbpDelta)
          Off = Disp - *Out.RbpDelta;
        if (!Off)
          continue;
        if (*Off < -Out.FrameSize)
          Out.FrameSize = -*Off;
        if (*Off > Out.ArgsSize)
          Out.ArgsSize = *Off;
      }
    };

    for (size_t I = B->InstrsBegin; I < B->InstrsEnd; ++I) {
      const Instr &Ins = Instrs[I];
      auto Ops = logicalOps(Ins.Inst, MC.MII->get(Ins.Inst.getOpcode()));
      Va Ip = Ins.Addr;
      Out.Deltas[Ip] = Delta;
      bump(Delta, Ins);
      StringRef N = opName(Ins, MC);

      // Frame-pointer detection: `mov rbp, rsp` / `lea rbp, [rsp+X]`.
      if (!Out.RbpDelta) {
        if (N.starts_with("MOV") && opIsReg(Ins, 0, X86::RBP, Ops) &&
            opIsReg(Ins, 1, X86::RSP, Ops))
          Out.RbpDelta = Delta;
        else if (N.starts_with("LEA") && opIsReg(Ins, 0, X86::RBP, Ops) &&
                 Ops.size() > 1 && Ops[1].Kind == LogOpKind::Mem) {
          MCRegister Base, Index, Seg;
          int64_t Scale, Disp;
          memParts(Ins, 1, Ops, Base, Scale, Index, Disp, Seg);
          if (isRsp(Base) && Index == X86::NoRegister)
            Out.RbpDelta = Delta - Disp;
        }
      }
      // `mov eax, imm` feeding a possible `call __chkstk`.
      if (N.starts_with("MOV") && opIsReg(Ins, 0, X86::EAX, Ops) &&
          opIsImm(Ins, 1, Ops))
        ChkstkSize = immAt(Ins, 1, Ops);

      // `call __chkstk` adjusts rsp by its rax argument.
      if (Ins.Flow == FlowKind::Call && Ins.HasTarget &&
          isChkstk(P, Ins.Target)) {
        if (ChkstkSize)
          Delta += *ChkstkSize;
        else if (!Out.DynVA)
          Out.DynVA = Ip;
        continue;
      }
      // `leave` / `mov rsp, rbp` restore the fp's delta.
      if ((N.starts_with("LEAVE") ||
           (N.starts_with("MOV") &&
            (opIsReg(Ins, 0, X86::RSP, Ops) || opIsReg(Ins, 0, X86::ESP, Ops)) &&
            opIsReg(Ins, 1, X86::RBP, Ops))) &&
          Out.RbpDelta) {
        Delta = *Out.RbpDelta;
        continue;
      }

      bool WritesRsp = opIsReg(Ins, 0, X86::RSP, Ops) ||
                       opIsReg(Ins, 0, X86::ESP, Ops);
      if (N.starts_with("PUSH") || N.starts_with("PUSHF")) {
        Delta += 8;
      } else if ((N.starts_with("POP") && !N.starts_with("POPCNT")) ||
                 N.starts_with("POPF")) {
        Delta -= 8;
      } else if (N.starts_with("SUB") && WritesRsp && opIsImm(Ins, 1, Ops)) {
        Delta += immAt(Ins, 1, Ops);
      } else if (N.starts_with("ADD") && WritesRsp && opIsImm(Ins, 1, Ops)) {
        Delta -= immAt(Ins, 1, Ops);
      } else if (N.starts_with("LEA") && WritesRsp && Ops.size() > 1 &&
                 Ops[1].Kind == LogOpKind::Mem) {
        MCRegister Base, Index, Seg;
        int64_t Scale, Disp;
        memParts(Ins, 1, Ops, Base, Scale, Index, Disp, Seg);
        if (isRsp(Base) && Index == X86::NoRegister)
          Delta -= Disp;
        else if (!Out.DynVA)
          Out.DynVA = Ip;
      } else if (WritesRsp) {
        // Non-constant rsp write: dynamic point.
        if (!Out.DynVA)
          Out.DynVA = Ip;
      }
    }

    if (B->EndKind != BlockEnd::Return && B->EndKind != BlockEnd::TailCall &&
        B->EndKind != BlockEnd::Noreturn) {
      for (const EdgeRec &E : F.edgesOf(*B)) {
        size_t EdgeIdx = &E - F.Edges.data();
        if (E.Kind != EdgeTag::TailCall)
          Work.push_back({F.edgeTo(*P.Img, EdgeIdx), Delta});
      }
    }
  }
  return Out;
}

//===--------------------------------------------------------------------===//
// LiftState
//===--------------------------------------------------------------------===//

LiftState::LiftState(llvm::Module &M, llvm::Function &Fn, const Program &P,
                     const FuncRecord &F, const Disasm &D,
                     const Signatures *Sigs, StackInfo S,
                     llvm::ArrayRef<Instr> Is)
    : Mod(M), Fn(Fn), B(M.getContext()), P(P), F(F), D(D), Sigs(Sigs),
      Stack(std::move(S)), Instrs(Is), MC(X86MCTools::get()) {}

llvm::AllocaInst *LiftState::entryAlloca(llvm::Type *Ty,
                                         llvm::StringRef Name) {
  llvm::IRBuilder<> EB(Fn.getEntryBlock().getContext());
  EB.SetInsertPoint(&Fn.getEntryBlock(), Fn.getEntryBlock().begin());
  return EB.CreateAlloca(Ty, nullptr, Name);
}

void LiftState::setVA(Va Addr) {
  CurVa = Addr;
  // IRBuilderBase::AddOrRemoveMetadataToCopy is private, so tagProvenance
  // plus explicit setInstructionVA at creation sites carry provenance.
  // setVA only records the current VA for rsp-relative addressing.
  (void)Addr;
}

void LiftState::tagProvenance() {
  for (llvm::BasicBlock &BB : Fn)
    for (llvm::Instruction &I : BB)
      if (!I.isDebugOrPseudoInst() && !ir::getInstructionVA(I))
        ir::setInstructionVA(I, CurVa.Value ? CurVa.Value : F.Start.Value);
}

llvm::Value *LiftState::extTo(llvm::Value *V, unsigned Bits) {
  llvm::Type *T = V->getType();
  unsigned W = T->isIntegerTy() ? T->getIntegerBitWidth()
                              : T->getPrimitiveSizeInBits();
  if (!T->isIntegerTy()) {
    if (T->isPointerTy())
      V = B.CreatePtrToInt(V, iN(W), "p2i");
    else
      V = B.CreateBitCast(V, iN(W), "b2i");
  }
  return B.CreateZExtOrTrunc(V, iN(Bits), "e");
}

Value *LiftState::sextTo(Value *V, unsigned Bits) {
  unsigned W = V->getType()->getIntegerBitWidth();
  if (W == Bits)
    return V;
  return B.CreateSExtOrTrunc(V, iN(Bits), "e");
}

//===--------------------------------------------------------------------===//
// Operand view
//===--------------------------------------------------------------------===//

std::vector<LogOp> LiftState::opsOf(const Instr &I) const {
  return logicalOps(I.Inst, MC.MII->get(I.Inst.getOpcode()));
}

LogOpKind LiftState::opKind(const Instr &I, unsigned L) const {
  auto Ops = opsOf(I);
  return L < Ops.size() ? Ops[L].Kind : LogOpKind::Invalid;
}

MCRegister LiftState::opReg(const Instr &I, unsigned L) const {
  auto Ops = opsOf(I);
  if (L >= Ops.size() || Ops[L].Kind != LogOpKind::Reg)
    return MCRegister();
  return I.Inst.getOperand(Ops[L].MI).getReg();
}

int64_t LiftState::opImm(const Instr &I, unsigned L) const {
  auto Ops = opsOf(I);
  if (L >= Ops.size() || Ops[L].Kind != LogOpKind::Imm)
    return 0;
  const MCOperand &O = I.Inst.getOperand(Ops[L].MI);
  return O.isImm() ? O.getImm() : 0;
}

LiftState::MemOp LiftState::opMem(const Instr &I, unsigned L) const {
  auto Ops = opsOf(I);
  MemOp M{};
  if (L >= Ops.size() || Ops[L].Kind != LogOpKind::Mem)
    return M;
  unsigned P2 = Ops[L].MI;
  M.Base = I.Inst.getOperand(P2 + X86::AddrBaseReg).getReg();
  M.Scale = I.Inst.getOperand(P2 + X86::AddrScaleAmt).getImm();
  M.Index = I.Inst.getOperand(P2 + X86::AddrIndexReg).getReg();
  M.Disp = I.Inst.getOperand(P2 + X86::AddrDisp).getImm();
  M.Seg = I.Inst.getOperand(P2 + X86::AddrSegmentReg).getReg();
  return M;
}

namespace {

/// Width in bits of the access through the memory operand. Sibling register
/// operands win; else the smallest digit group in the TableGen name
/// (MOVZX64rm8 -> 8, ADD64mi32 -> 64); else 64.
unsigned memAccessBits(const Instr &I, const MCInstrDesc &Desc,
                       const std::vector<LogOp> &Ops, unsigned MemL,
                       const MCRegisterInfo &MRI, const MCInstrInfo &MII) {
  // Smallest digit group in the opcode name.
  StringRef N = MII.getName(I.Inst.getOpcode());
  unsigned Best = 0;
  for (unsigned Ix = 0; Ix + 1 < N.size(); ++Ix) {
    if (!isDigit(N[Ix]))
      continue;
    unsigned V = N[Ix] - '0', J = Ix;
    while (J + 1 < N.size() && isDigit(N[J + 1]))
      V = V * 10 + (N[++J] - '0');
    Ix = J;
    if (V == 8 || V == 16 || V == 32 || V == 64 || V == 80 || V == 128 ||
        V == 256 || V == 512)
      if (!Best || V < Best)
        Best = V;
  }
  if (Best)
    return Best;
  // Sibling register operand width.
  for (unsigned L = 0; L < Ops.size(); ++L) {
    if (L == MemL || Ops[L].Kind != LogOpKind::Reg)
      continue;
    auto R = I.Inst.getOperand(Ops[L].MI).getReg();
    return regWidthOf(R);
  }
  return 64;
}

} // namespace

unsigned LiftState::opBits(const Instr &I, unsigned L) const {
  auto Ops = opsOf(I);
  if (L >= Ops.size())
    return 64;
  switch (Ops[L].Kind) {
  case LogOpKind::Reg:
    return regOffsetBits(I.Inst.getOperand(Ops[L].MI).getReg(), *MC.MRI).second;
  case LogOpKind::Mem:
    return memAccessBits(I, MC.MII->get(I.Inst.getOpcode()), Ops, L, *MC.MRI,
                         *MC.MII);
  case LogOpKind::Imm:
    return 64;
  default:
    return 64;
  }
}

//===--------------------------------------------------------------------===//
// Registers and flags
//===--------------------------------------------------------------------===//

Value *LiftState::regRead(MCRegister R) {
  if (R == X86::RIP || R == X86::EIP)
    return ci(0); // RIP is never read directly; mem RIP uses MemTarget.
  if (isRsp(R)) {
    int64_t Off = -Stack.deltaAt(CurVa);
    Value *P;
    if (Stack.isDynamicAt(CurVa) && DynSp) {
      Value *D = B.CreateLoad(ptrTy(), DynSp, "ds");
      // rsp = dyn_sp - (delta - delta_at_dyn).
      int64_t Resid = DynDelta - Stack.deltaAt(CurVa);
      P = B.CreateInBoundsGEP(iN(8), D, {ci((uint64_t)Resid)}, "sp");
    } else {
      P = framePtrOff(Off);
    }
    return B.CreatePtrToInt(P, i64(), "rsp");
  }
  if (auto Slot = xmmSlotOf(R)) {
    auto It = Xmm.find(*Slot);
    if (It == Xmm.end())
      return UndefValue::get(iN(128));
    return B.CreateLoad(iN(128), It->second, "x");
  }
  MCRegister Root = gprRootOf(R);
  if (!Root.isValid()) {
    if (isVectorReg(R))
      return UndefValue::get(iN(128));
    return UndefValue::get(i64());
  }
  AllocaInst *Al = Gpr[Root];
  Value *V = B.CreateLoad(i64(), Al, "r");
  auto [Off, Bits] = regOffsetBits(R, *MC.MRI);
  if (Off)
    V = B.CreateLShr(V, ci(Off), "s");
  if (Bits < 64)
    V = B.CreateTrunc(V, iN(Bits), "t");
  return V;
}

void LiftState::regWrite(MCRegister R, Value *V) {
  if (isRsp(R) || R == X86::RIP || R == X86::EIP)
    return; // rsp is virtual: the frame delta tracks it.
  if (auto Slot = xmmSlotOf(R)) {
    auto It = Xmm.find(*Slot);
    if (It != Xmm.end())
      B.CreateStore(extTo(V, 128), It->second);
    return;
  }
  MCRegister Root = gprRootOf(R);
  if (!Root.isValid())
    return;
  AllocaInst *Al = Gpr[Root];
  auto [Off, Bits] = regOffsetBits(R, *MC.MRI);
  V = extTo(V, Bits);
  if (Bits == 64) {
    B.CreateStore(V, Al);
    return;
  }
  if (Bits == 32 && Off == 0) {
    // 32-bit writes zero-extend into the root.
    B.CreateStore(B.CreateZExt(V, i64(), "z"), Al);
    return;
  }
  Value *Old = B.CreateLoad(i64(), Al, "old");
  uint64_t Mask = ~(((uint64_t)-1 >> (64 - Bits)) << Off);
  Value *Cleared = B.CreateAnd(Old, ci(Mask), "clr");
  Value *Ins = B.CreateZExt(V, i64(), "z");
  if (Off)
    Ins = B.CreateShl(Ins, ci(Off), "sh");
  B.CreateStore(B.CreateOr(Cleared, Ins, "m"), Al);
}

Value *LiftState::flagRead(StringRef N) {
  return B.CreateLoad(iN(1), Flags[N], N);
}

void LiftState::flagWrite(StringRef N, Value *I1) {
  Value *V = I1;
  if (V->getType()->getIntegerBitWidth() != 1)
    V = B.CreateICmpNE(V, ConstantInt::get(V->getType(), 0), "f");
  B.CreateStore(V, Flags[N]);
}

//===--------------------------------------------------------------------===//
// Operand read/write
//===--------------------------------------------------------------------===//

Value *LiftState::opReadInt(const Instr &I, unsigned L, unsigned Bits) {
  auto Ops = opsOf(I);
  if (L >= Ops.size())
    return ci(0);
  switch (Ops[L].Kind) {
  case LogOpKind::Reg:
    return extTo(regRead(I.Inst.getOperand(Ops[L].MI).getReg()), Bits);
  case LogOpKind::Mem:
    return memRead(I, L, Bits);
  case LogOpKind::Imm:
    return ci((uint64_t)opImm(I, L), std::max(Bits, 8u));
  default:
    return ci(0);
  }
}

void LiftState::opWriteInt(const Instr &I, unsigned L, Value *V) {
  auto Ops = opsOf(I);
  if (L >= Ops.size())
    return;
  switch (Ops[L].Kind) {
  case LogOpKind::Reg: {
    MCRegister R = I.Inst.getOperand(Ops[L].MI).getReg();
    auto [Off, Bits] = regOffsetBits(R, *MC.MRI);
    if (xmmSlotOf(R))
      regWrite(R, extTo(V, 128));
    else
      regWrite(R, extTo(V, Bits));
    return;
  }
  case LogOpKind::Mem:
    memWrite(I, L, V);
    return;
  default:
    return;
  }
}

//===--------------------------------------------------------------------===//
// Frame and memory
//===--------------------------------------------------------------------===//

Value *LiftState::framePtrOff(int64_t Off) {
  return B.CreateInBoundsGEP(iN(8), Sp0, {ci((uint64_t)Off)}, "fp");
}

Value *LiftState::memPtr(const Instr &I, unsigned L) {
  MemOp M = opMem(I, L);
  // moffs absolute form decodes as an immediate operand instead.
  if (M.Base == X86::RIP) {
    Va Target = I.HasMemTarget ? I.MemTarget : I.nextVa().wrappingAdd(M.Disp);
    return globalPtr(Target);
  }
  Value *Idx = nullptr;
  if (M.Index != X86::NoRegister)
    Idx = extTo(regRead(M.Index), 64);
  if (isRsp(M.Base)) {
    if (Stack.isDynamicAt(CurVa) && DynSp) {
      Value *D = B.CreateLoad(ptrTy(), DynSp, "ds");
      int64_t Resid = M.Disp + DynDelta - Stack.deltaAt(CurVa);
      return B.CreateInBoundsGEP(iN(8), D, {ci((uint64_t)Resid)}, "fp");
    }
    int64_t Off = Stack.rspOff(CurVa, M.Disp);
    if (!Idx)
      return framePtrOff(Off);
    Value *Scaled = B.CreateMul(Idx, ci((uint64_t)M.Scale), "sc");
    Value *Sum = B.CreateAdd(sextTo(Scaled, 64), ci((uint64_t)Off), "a");
    return B.CreateInBoundsGEP(iN(8), Sp0, {Sum}, "fp");
  }
  if (isRbp(M.Base) && Stack.rbpOff(M.Disp)) {
    int64_t Off = *Stack.rbpOff(M.Disp);
    if (Fn.hasFnAttribute("revenant.msvc.funclet")) {
      Value *Base = B.CreateIntToPtr(regRead(X86::RDX), ptrTy(), "eh.frame");
      return B.CreateInBoundsGEP(iN(8), Base, {ci((uint64_t)Off)}, "eh.fp");
    }
    return framePtrOff(Off);
  }
  // General address: base + idx*scale + disp.
  Value *Addr = M.Base == X86::NoRegister ? ci(0) : extTo(regRead(M.Base), 64);
  if (Idx) {
    Value *Scaled = B.CreateMul(sextTo(Idx, 64), ci((uint64_t)M.Scale), "sc");
    Addr = B.CreateAdd(Addr, Scaled, "a");
  }
  if (M.Disp)
    Addr = B.CreateAdd(Addr, ci((uint64_t)M.Disp), "d");
  return B.CreateIntToPtr(Addr, ptrTy(), "p");
}

std::string lift::globalNameFor(const Program &P, Va Addr) {
  if (P.Strings) {
    if (auto It = P.Strings->find(Addr); It != P.Strings->end()) {
      const std::string &S = It->second;
      std::string San = sanitizeName(S);
      bool Ascii = !S.empty() && San != "f";
      for (char C : S)
        if ((unsigned char)C > 0x7e || (unsigned char)C < 0x20)
          Ascii = false;
      return Ascii ? "a" + San : "str_" + utohexstr(Addr.Value, true);
    }
  }
  if (P.Functions.count(Addr)) {
    if (P.Names)
      if (auto It = P.Names->find(Addr); It != P.Names->end())
        return "f_" + sanitizeName(It->second);
    return "f_" + utohexstr(Addr.Value, true);
  }
  if (P.Names)
    if (auto It = P.Names->find(Addr); It != P.Names->end())
      return sanitizeName(It->second);
  return "g_" + utohexstr(Addr.Value, true);
}

Value *LiftState::globalPtr(Va Addr) {
  if (auto It = Globals.find(Addr); It != Globals.end())
    return It->second;
  std::string Name = globalNameFor(P, Addr);
  std::optional<std::vector<uint8_t>> Init;
  if (P.Strings) {
    if (auto It = P.Strings->find(Addr); It != P.Strings->end())
      Init = std::vector<uint8_t>(It->second.begin(), It->second.end());
  }
  // True when Init came straight from the image, i.e. the bytes are the
  // program's data, not an analyzer-recognized string.
  bool FromImage = false;
  if (!Init && !P.Functions.count(Addr)) {
    Init = P.Img->readAtMost(Addr, 4096);
    FromImage = true;
  }
  unsigned Sz = Init ? (unsigned)Init->size() : 16;
  if (Sz == 0)
    Sz = 16;
  // Writable image data (the security cookie, mutable tables) carries the
  // file's bytes as its initializer but must not be `constant`: the program
  // stores to it, and a constant global lets the optimizer fold its loads to
  // the file's bytes. The IR contract keeps analyzer strings constant.
  bool Writable = FromImage && P.Img->isWritable(Addr);
  auto *G = new GlobalVariable(
      Mod, ArrayType::get(iN(8), Sz), /*isConstant=*/Init && !Writable,
      Init ? GlobalValue::InternalLinkage : GlobalValue::ExternalLinkage,
      nullptr, Name);
  if (Init) {
    SmallVector<Constant *, 16> Vals;
    Vals.reserve(Init->size());
    for (uint8_t V2 : *Init)
      Vals.push_back(ci(V2, 8));
    G->setInitializer(ConstantArray::get(ArrayType::get(iN(8), Sz), Vals));
  }
  ir::setGlobalVA(*G, Addr.Value);
  Globals[Addr] = G;
  return G;
}

Value *LiftState::memRead(const Instr &I, unsigned L, unsigned Bits,
                          bool Float) {
  auto Ops = opsOf(I);
  if (L >= Ops.size() || Ops[L].Kind != LogOpKind::Mem)
    return UndefValue::get(iN(Bits));
  // Absolute (moffs) form: decoded as a single immediate.
  Type *Ty;
  if (Float)
    Ty = Bits == 32 ? Type::getFloatTy(Mod.getContext())
                    : Type::getDoubleTy(Mod.getContext());
  else
    Ty = iN(Bits);
  MemOp M = opMem(I, L);
  if (M.Seg != X86::NoRegister)
    return UndefValue::get(Ty);
  Value *P = memPtr(I, L);
  return B.CreateAlignedLoad(Ty, P, Align(1), "m");
}

void LiftState::memWrite(const Instr &I, unsigned L, Value *V) {
  MemOp M = opMem(I, L);
  if (M.Seg != X86::NoRegister)
    return;
  Value *P = memPtr(I, L);
  B.CreateAlignedStore(V, P, Align(1));
}

void LiftState::pushVal(Value *V, const Instr &I) {
  // New rsp = old - 8 -> frame offset at delta+8.
  int64_t Off = -(Stack.deltaAt(I.Addr) + 8);
  B.CreateAlignedStore(extTo(V, 64), framePtrOff(Off), Align(1));
}

Value *LiftState::popVal(const Instr &I) {
  int64_t Off = -Stack.deltaAt(I.Addr);
  return B.CreateAlignedLoad(i64(), framePtrOff(Off), Align(1), "pv");
}

void LiftState::emitDynAlloca(const Instr &I) {
  DynDelta = Stack.deltaAt(I.Addr);
  StringRef N = MC.MII->getName(I.Inst.getOpcode());
  Value *Sz;
  unsigned Align = 16;
  if (I.Flow == FlowKind::Call) {
    // `call __chkstk`: size arrives in rax.
    Sz = regRead(X86::RAX);
  } else if (N.starts_with("AND") && opIsImm(I, 1, opsOf(I))) {
    unsigned A = std::max<int64_t>(-opImm(I, 1), 1);
    Align = A;
    Sz = ci((uint64_t)(DynDelta + (int64_t)A));
  } else {
    Sz = opReadInt(I, 1, 64);
  }
  AllocaInst *P = B.CreateAlloca(iN(8), extTo(Sz, 64), "dyn");
  P->setAlignment(llvm::Align(Align));
  B.CreateStore(P, DynSp);
}

//===--------------------------------------------------------------------===//
// Flags
//===--------------------------------------------------------------------===//

void LiftState::produce(FlagProd P2) {
  materializeProd(P2);
  Prod = P2;
}

void LiftState::materializeFlags(Value *Res, Value *A, Value *B2, char Mn) {
  unsigned Bits = Res->getType()->getIntegerBitWidth();
  Value *Z = ci(0, Bits);
  Value *SfMask = ci(1ull << (Bits - 1), Bits);
  flagWrite("zf", B.CreateICmpEQ(Res, Z, "zf"));
  Value *Sfv = B.CreateAnd(Res, SfMask, "sf");
  flagWrite("sf", B.CreateICmpNE(Sfv, Z, "sf"));
  {
    Value *Lo = B.CreateTrunc(Res, iN(8), "l");
    Value *Ct = B.CreateIntrinsic(Intrinsic::ctpop, {iN(8)}, {Lo});
    Value *Parity = B.CreateTrunc(Ct, iN(1), "pf");
    flagWrite("pf", B.CreateNot(Parity, "pf"));
  }
  bool IsSub = Mn == 's' || Mn == 'd' || Mn == 'n';
  bool IsAdd = Mn == 'a' || Mn == 'i';
  if (IsSub && A) {
    Value *Bv = B2 ? B2 : Z;
    flagWrite("cf", B.CreateICmpULT(A, Bv, "cf"));
    Value *Axb = B.CreateXor(A, Bv, "o");
    Value *Axr = B.CreateXor(A, Res, "o2");
    Value *Both = B.CreateAnd(B.CreateAnd(Axb, Axr, "o3"), SfMask, "o4");
    flagWrite("of", B.CreateICmpNE(Both, Z, "of"));
  } else if (IsAdd && A) {
    Value *Bv = B2 ? B2 : Z;
    flagWrite("cf", B.CreateICmpULT(Res, A, "cf"));
    Value *Axb = B.CreateNot(B.CreateXor(A, Bv, "o"), "n");
    Value *Axr = B.CreateXor(A, Res, "o2");
    Value *Both = B.CreateAnd(B.CreateAnd(Axb, Axr, "o3"), SfMask, "o4");
    flagWrite("of", B.CreateICmpNE(Both, Z, "of"));
  } else {
    flagWrite("cf", B.getFalse());
    flagWrite("of", B.getFalse());
  }
}

void LiftState::materializeProd(const FlagProd &P2) {
  switch (P2.K) {
  case FlagProd::Cmp: {
    Value *Res = B.CreateSub(P2.A, P2.B, "cs");
    materializeFlags(Res, P2.A, P2.B, 's');
    return;
  }
  case FlagProd::Test:
    materializeFlags(P2.Res, nullptr, nullptr, 'l');
    return;
  case FlagProd::Arith:
    materializeFlags(P2.Res, P2.A, P2.B, P2.Mn);
    return;
  }
}

Value *LiftState::condFromFlags(unsigned CC) {
  auto Ld = [&](StringRef N) { return flagRead(N); };
  auto Not = [&](Value *V) { return B.CreateNot(V, "n"); };
  auto Or = [&](Value *X, Value *Y) { return B.CreateOr(X, Y, "o"); };
  using CC2 = X86::CondCode;
  switch ((CC2)CC) {
  case CC2::COND_E: return Ld("zf");
  case CC2::COND_NE: return Not(Ld("zf"));
  case CC2::COND_B: return Ld("cf");
  case CC2::COND_AE: return Not(Ld("cf"));
  case CC2::COND_BE: return Or(Ld("cf"), Ld("zf"));
  case CC2::COND_A: return Not(Or(Ld("cf"), Ld("zf")));
  case CC2::COND_S: return Ld("sf");
  case CC2::COND_NS: return Not(Ld("sf"));
  case CC2::COND_P: return Ld("pf");
  case CC2::COND_NP: return Not(Ld("pf"));
  case CC2::COND_O: return Ld("of");
  case CC2::COND_NO: return Not(Ld("of"));
  case CC2::COND_L:
    return B.CreateXor(Ld("sf"), Ld("of"), "x");
  case CC2::COND_GE:
    return Not(B.CreateXor(Ld("sf"), Ld("of"), "x"));
  case CC2::COND_LE: {
    Value *X = B.CreateXor(Ld("sf"), Ld("of"), "x");
    return Or(X, Ld("zf"));
  }
  case CC2::COND_G: {
    Value *X = B.CreateXor(Ld("sf"), Ld("of"), "x");
    return Not(Or(X, Ld("zf")));
  }
  default:
    return B.getFalse();
  }
}

std::optional<Value *> LiftState::fuseCond(unsigned CC, const FlagProd &P2) {
  auto Cmp = [&](CmpInst::Predicate Pr, Value *X, Value *Y) -> Value * {
    return B.CreateICmp(Pr, X, Y, "c");
  };
  using CC2 = X86::CondCode;
  switch (P2.K) {
  case FlagProd::Cmp:
    switch ((CC2)CC) {
    case CC2::COND_A: return Cmp(CmpInst::ICMP_UGT, P2.A, P2.B);
    case CC2::COND_AE: return Cmp(CmpInst::ICMP_UGE, P2.A, P2.B);
    case CC2::COND_B: return Cmp(CmpInst::ICMP_ULT, P2.A, P2.B);
    case CC2::COND_BE: return Cmp(CmpInst::ICMP_ULE, P2.A, P2.B);
    case CC2::COND_E: return Cmp(CmpInst::ICMP_EQ, P2.A, P2.B);
    case CC2::COND_NE: return Cmp(CmpInst::ICMP_NE, P2.A, P2.B);
    case CC2::COND_G: return Cmp(CmpInst::ICMP_SGT, P2.A, P2.B);
    case CC2::COND_GE: return Cmp(CmpInst::ICMP_SGE, P2.A, P2.B);
    case CC2::COND_L: return Cmp(CmpInst::ICMP_SLT, P2.A, P2.B);
    case CC2::COND_LE: return Cmp(CmpInst::ICMP_SLE, P2.A, P2.B);
    default: return std::nullopt;
    }
  case FlagProd::Test: {
    Value *Z = ConstantInt::get(P2.Res->getType(), 0);
    switch ((CC2)CC) {
    case CC2::COND_E: return Cmp(CmpInst::ICMP_EQ, P2.Res, Z);
    case CC2::COND_NE: return Cmp(CmpInst::ICMP_NE, P2.Res, Z);
    case CC2::COND_S: return Cmp(CmpInst::ICMP_SLT, P2.Res, Z);
    case CC2::COND_NS: return Cmp(CmpInst::ICMP_SGE, P2.Res, Z);
    case CC2::COND_B:
    case CC2::COND_P:
    case CC2::COND_O:
      return B.getFalse();
    case CC2::COND_AE:
    case CC2::COND_NP:
    case CC2::COND_NO:
      return B.getTrue();
    case CC2::COND_A:
    case CC2::COND_BE:
      return Cmp(CmpInst::ICMP_NE, P2.Res, Z);
    default: return std::nullopt;
    }
  }
  case FlagProd::Arith: {
    Value *Z = ConstantInt::get(P2.Res->getType(), 0);
    auto SubOps = [&](CmpInst::Predicate Pr) -> Value * {
      if (P2.Mn == 's' && P2.A && P2.B)
        return Cmp(Pr, P2.A, P2.B);
      return nullptr;
    };
    switch ((CC2)CC) {
    case CC2::COND_E: return Cmp(CmpInst::ICMP_EQ, P2.Res, Z);
    case CC2::COND_NE: return Cmp(CmpInst::ICMP_NE, P2.Res, Z);
    case CC2::COND_S: return Cmp(CmpInst::ICMP_SLT, P2.Res, Z);
    case CC2::COND_NS: return Cmp(CmpInst::ICMP_SGE, P2.Res, Z);
    case CC2::COND_B: {
      if (Value *V = SubOps(CmpInst::ICMP_ULT)) return V;
      return std::nullopt;
    }
    case CC2::COND_AE: {
      if (Value *V = SubOps(CmpInst::ICMP_UGE)) return V;
      return std::nullopt;
    }
    case CC2::COND_A: {
      if (Value *V = SubOps(CmpInst::ICMP_UGT)) return V;
      return std::nullopt;
    }
    case CC2::COND_BE: {
      if (Value *V = SubOps(CmpInst::ICMP_ULE)) return V;
      return std::nullopt;
    }
    default: return std::nullopt;
    }
  }
  }
  return std::nullopt;
}

Value *LiftState::condFor(unsigned CC, Va JccVa) {
  if (Prod) {
    FlagProd P2 = *Prod;
    Prod.reset();
    if (auto C = fuseCond(CC, P2))
      return *C;
  }
  return condFromFlags(CC);
}

Value *LiftState::evalCmpOperand(const CmpOperand &Loc, Va At) {
  if (Loc.Tag == CmpOperand::Kind::Reg)
    return extTo(regRead(MCRegister(Loc.Reg)), 64);
  Value *P;
  MCRegister Base(Loc.Base), Index(Loc.Index);
  if (isRsp(Base)) {
    int64_t Foff = (int64_t)Loc.Disp - Stack.deltaAt(At);
    P = framePtrOff(Foff);
  } else if (isRbp(Base)) {
    int64_t Foff = Stack.rbpOff((int64_t)Loc.Disp).value_or(0);
    P = framePtrOff(Foff);
  } else {
    Value *Addr = Base == X86::NoRegister ? ci(0)
                                          : extTo(regRead(Base), 64);
    if (Index != X86::NoRegister) {
      Value *Iv = extTo(regRead(Index), 64);
      Value *Sc = B.CreateMul(sextTo(Iv, 64), ci(Loc.Scale), "s");
      Addr = B.CreateAdd(Addr, Sc, "a");
    }
    Addr = B.CreateAdd(Addr, ci(Loc.Disp), "d");
    P = B.CreateIntToPtr(Addr, ptrTy(), "p");
  }
  return B.CreateAlignedLoad(i64(), P, Align(1), "sv");
}

void LiftState::clobberFlags() {
  for (auto &[N, Al] : Flags)
    B.CreateStore(UndefValue::get(iN(1)), Al);
}

void LiftState::clobberVolatile() {
  for (MCRegister R : {X86::RAX, X86::RCX, X86::RDX, X86::R8, X86::R9,
                       X86::R10, X86::R11})
    B.CreateStore(UndefValue::get(i64()), Gpr[R]);
  for (unsigned K = 0; K < 6; ++K)
    B.CreateStore(UndefValue::get(iN(128)), Xmm[K]);
  clobberFlags();
}

void LiftState::clobberCallee(llvm::Function *Callee, std::optional<Va> Target) {
  // Only a local function we can decode has a bounded clobber set; everything
  // else (imports, indirect calls, the CFG dispatch stubs) keeps the
  // conservative Win64 volatile set.
  const FuncRecord *Rec = nullptr;
  if (Target)
    if (auto It = P.Functions.find(*Target); It != P.Functions.end())
      Rec = &It->second;
  if (!Rec) {
    clobberVolatile();
    return;
  }
  // Walk the callee's decoded instructions collecting every register it
  // defines (explicit outs + implicit defs), mapped to a tracked root.
  bool Writes[16] = {}; // rax rcx rdx rbx rsi rdi rbp rsp r8..r15
  auto Mark = [&](MCRegister R) {
    MCRegister Root = rootRegOf(R, *MC.MRI);
    switch (Root) {
    case X86::RAX: Writes[0] = true; break;
    case X86::RCX: Writes[1] = true; break;
    case X86::RDX: Writes[2] = true; break;
    case X86::RBX: Writes[3] = true; break;
    case X86::RSI: Writes[4] = true; break;
    case X86::RDI: Writes[5] = true; break;
    case X86::RBP: Writes[6] = true; break;
    case X86::RSP: Writes[7] = true; break;
    case X86::R8: Writes[8] = true; break;
    case X86::R9: Writes[9] = true; break;
    case X86::R10: Writes[10] = true; break;
    case X86::R11: Writes[11] = true; break;
    case X86::R12: Writes[12] = true; break;
    case X86::R13: Writes[13] = true; break;
    case X86::R14: Writes[14] = true; break;
    case X86::R15: Writes[15] = true; break;
    default: break;
    }
  };
  bool WritesXmm[6] = {};
  bool Unknown = false;
  for (Rva R : Rec->InstrRvas) {
    auto DI = D.decodeOne(P.Img->vaOfRva(R));
    if (!DI) {
      Unknown = true;
      break;
    }
    const MCInstrDesc &Desc = MC.MII->get(DI->Inst.getOpcode());
    // A direct recursive call adds no writes beyond this same body scan
    // (whatever the recursion disturbs is collected here), so it is
    // skipped instead of bailing to the conservative set. This keeps
    // caller registers that a recursive leaf preserves (c-gcc-o2 fib
    // preserves edx across its self-call).
    if (Desc.isCall() && DI->HasTarget && DI->Target == Rec->Start)
      continue;
    // A nested call, an indirect jump, a trap, or a direct jump whose target
    // is another function (a tail call) can transfer to code outside this
    // record, whose register effects are unknown: stay conservative.
    if (Desc.isCall() || DI->Flow == FlowKind::IndirectBranch ||
        DI->Flow == FlowKind::Trap ||
        (DI->HasTarget && DI->Target != Rec->Start &&
         !Rec->instrIndex(*P.Img, DI->Target))) {
      Unknown = true;
      break;
    }
    auto Ops = logicalOps(DI->Inst, Desc);
    for (unsigned K = 0; K < Desc.getNumDefs() && K < Ops.size(); ++K)
      if (Ops[K].Kind == LogOpKind::Reg)
        Mark(DI->Inst.getOperand(Ops[K].MI).getReg());
    for (MCPhysReg Def : Desc.implicit_defs())
      Mark(Def);
    // Any vector def that lands in xmm0-5 also clobbers the tracked slot.
    for (unsigned K = 0; K < Desc.getNumDefs() && K < Ops.size(); ++K)
      if (Ops[K].Kind == LogOpKind::Reg)
        if (auto S = xmmSlot(DI->Inst.getOperand(Ops[K].MI).getReg()))
          if (*S < 6)
            WritesXmm[*S] = true;
    for (MCPhysReg Def : Desc.implicit_defs())
      if (auto S = xmmSlot(Def))
        if (*S < 6)
          WritesXmm[*S] = true;
  }
  if (Unknown) {
    clobberVolatile();
    return;
  }
  // The callee may also be reached through the CFG dispatch stub or a tail
  // call; those are outside Rec and would be missed, so also treat a call
  // target that is not the record start conservatively.
  (void)Callee;
  static const MCRegister Vol[7] = {X86::RAX, X86::RCX, X86::RDX, X86::R8,
                                    X86::R9, X86::R10, X86::R11};
  static const unsigned VolIdx[7] = {0, 1, 2, 8, 9, 10, 11};
  for (unsigned K = 0; K < 7; ++K)
    if (Writes[VolIdx[K]])
      B.CreateStore(UndefValue::get(i64()), Gpr[Vol[K]]);
  for (unsigned K = 0; K < 6; ++K)
    if (WritesXmm[K])
      B.CreateStore(UndefValue::get(iN(128)), Xmm[K]);
  // Flags are always dead across a call in the modeled semantics.
  clobberFlags();
}

//===--------------------------------------------------------------------===//
// Small helpers
//===--------------------------------------------------------------------===//

llvm::Function *LiftState::llvmIntrinsic(llvm::StringRef Name,
                                         llvm::ArrayRef<llvm::Type *> Tys) {
  llvm::Intrinsic::ID Id = llvm::Intrinsic::lookupIntrinsicID(Name);
  if (Id == llvm::Intrinsic::not_intrinsic)
    return nullptr;
  return llvm::Intrinsic::getOrInsertDeclaration(&Mod, Id, Tys);
}

std::string lift::mnemonicOf(const Instr &I, const Disasm &D) {
  std::string Text = D.format(I, {});
  // First non-prefix token, lowercased.
  StringRef T(Text);
  std::string Mn;
  while (!T.empty()) {
    size_t Split = T.find_first_of(" \t");
    StringRef W = T.take_front(Split).trim();
    if (!W.empty() && W != "rep" && W != "repe" && W != "repz" &&
        W != "repne" && W != "repnz" && W != "lock" && W != "rex64" &&
        W != "data16") {
      Mn = W.lower();
      break;
    }
    if (Split == StringRef::npos)
      break;
    T = T.drop_front(Split + 1).trim();
  }
  if (Mn.empty())
    Mn = X86MCTools::get().MII->getName(I.Inst.getOpcode()).lower();
  return Mn;
}

} // namespace lift
} // namespace revenant
