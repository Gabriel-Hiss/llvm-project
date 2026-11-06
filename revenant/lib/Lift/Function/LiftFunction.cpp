//===- LiftFunction.cpp - function-level asm -> LLVM IR lifter ------------===//
//
// One recovered function -> one LLVM function in "decompiler form" (see
// docs/ir-contract.md): blocks named bb_<VA>, register file and stack frame
// as entry-block allocas that SROA/mem2reg later dissolve, calls typed from
// recovered signatures, imports as named declarations, data/string globals
// per the contract, and opaque revenant.asm.<mnemonic> calls wherever the
// core table has no rule yet. Mirrors crates/lift lib.rs/block.rs.
//
// Per-instruction semantics go through LiftState::liftInstr (hand-lifted
// core in LiftCore.cpp, calls in LiftCalls.cpp); LunaLiftGen2's generated
// liftX86Inst(X86LiftContext&, ...) plugs into liftInstr once it lands.
//
// NOTE: revenant::BasicBlock (analysis CFG) collides with llvm::BasicBlock,
// so every LLVM type here is fully qualified.
//
//===----------------------------------------------------------------------===//

#include "LiftState.h"
#include "LiftEH.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"

#include "X86BaseInfo.h"
#include "X86MCTargetDesc.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Metadata.h"

#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <cctype>

namespace revenant::lift {

Signature defaultCalleeSignature() {
  Signature S;
  S.Params = {{ParamLoc::reg(llvm::X86::RCX), 64, false, std::nullopt},
              {ParamLoc::reg(llvm::X86::RDX), 64, false, std::nullopt},
              {ParamLoc::reg(llvm::X86::R8), 64, false, std::nullopt},
              {ParamLoc::reg(llvm::X86::R9), 64, false, std::nullopt}};
  S.Ret = RetLoc{false, 64};
  return S;
}

llvm::StringRef liftCleanupPasses() {
  // Mirrors crates/lift CLEANUP_PASSES; the in-process driver runs exactly
  // this text. MuseTxPipeline's defaultPipeline wraps it with idioms/unopt/
  // structure for the full decompile.
  return "sroa,mem2reg,early-cse,instcombine<no-verify-fixpoint>,"
         "simplifycfg<no-switch-to-lookup;no-hoist-common-insts;no-sink-common-"
         "insts;no-speculate-blocks>,gvn,dse,dce,sroa,instcombine<no-verify-"
         "fixpoint>";
}

static llvm::FunctionType *functionType(llvm::LLVMContext &C,
                                        const Signature *S) {
  llvm::SmallVector<llvm::Type *, 8> Params;
  if (S)
    for (const Param &P : S->Params)
      Params.push_back(P.Float ? (P.Bits == 32 ? llvm::Type::getFloatTy(C)
                                               : llvm::Type::getDoubleTy(C))
                               : llvm::Type::getIntNTy(C,
                                                       std::max(1u, P.Bits)));
  llvm::Type *Ret = llvm::Type::getVoidTy(C);
  if (S && S->Ret)
    Ret = S->Ret->Float ? (S->Ret->Bits == 32 ? llvm::Type::getFloatTy(C)
                                              : llvm::Type::getDoubleTy(C))
                        : llvm::Type::getIntNTy(C, std::max(1u, S->Ret->Bits));
  return llvm::FunctionType::get(Ret, Params, false);
}

static const Signature *signatureFor(const Signatures *Sigs, Va Addr) {
  return Sigs ? Sigs->of(Addr) : nullptr;
}

std::string lift::definitionName(const FuncRecord &F) {
  return F.Name.empty() ? llvm::formatv("sub_{0:x}", F.Start.Value).str()
                        : sanitizeName(F.Name);
}

llvm::Function *lift::claimDefinition(llvm::Module &M, llvm::StringRef Name,
                                      const Signature *Sig) {
  llvm::FunctionType *FT = functionType(M.getContext(), Sig);
  if (llvm::Function *Old = M.getFunction(Name)) {
    if (!Old->empty())
      return nullptr;
    // A previous callee decl with the same name: keep it only when the type
    // already matches; else rename the old decl out of the way.
    if (Old->getFunctionType() != FT)
      Old->setName(Name + ".decl");
  }
  if (llvm::Function *Old = M.getFunction(Name))
    return Old;
  return llvm::cast<llvm::Function>(
      M.getOrInsertFunction(Name, FT).getCallee());
}

// Count of contiguous repetitions of one straight-line opcode window.
// Several asm copies sharing the function's single Location record is
// evidence they implement one source statement (an unrolled loop body).
// Returns 0 when no repeated window is worth reporting. Windows may not
// cross branches, calls, returns or traps; opcode equality ignores
// registers/immediates, so this is confirmation-grade evidence only and
// never a rewrite proof on its own.
static unsigned repeatedWindowFactor(const std::vector<Instr> &Instrs) {
  constexpr size_t MinWindow = 4;
  constexpr size_t MinCovered = 8;
  constexpr size_t MaxWindow = 64;
  constexpr unsigned MaxFactor = 64;
  size_t N = Instrs.size();
  if (N < MinCovered)
    return 0;
  std::vector<int64_t> Op(N, -1);
  for (size_t I = 0; I < N; ++I)
    if (Instrs[I].Flow == FlowKind::None)
      Op[I] = (int64_t)Instrs[I].Inst.getOpcode();
  unsigned Best = 0;
  for (size_t M = MinWindow, End = std::min(N / 2, MaxWindow); M <= End; ++M) {
    for (size_t S = 0; S + 2 * M <= N; ++S) {
      bool Straight = true;
      for (size_t K = 0; K < M; ++K)
        if (Op[S + K] < 0) {
          Straight = false;
          break;
        }
      if (!Straight)
        continue;
      unsigned K = 1;
      while (K < MaxFactor && S + (size_t)(K + 1) * M <= N) {
        bool Same = true;
        for (size_t J = 0; J < M; ++J)
          if (Op[S + (size_t)K * M + J] != Op[S + J]) {
            Same = false;
            break;
          }
        if (!Same)
          break;
        ++K;
      }
      if (K >= 2 && (size_t)K * M >= MinCovered && K > Best)
        Best = K;
    }
  }
  return Best;
}

static void attachVa(llvm::Instruction *I, uint64_t VA) {
  if (I)
    ir::setInstructionVA(*I, VA);
}

static llvm::BasicBlock *edgeBlock(LiftState &LS, const FuncRecord &F,
                                   const Image &Img, size_t EdgeIdx) {
  Va To = F.edgeTo(Img, EdgeIdx);
  auto It = LS.Blocks.find(To);
  return It == LS.Blocks.end() ? nullptr : It->second;
}

/// Traced index location for a switch dispatch block: the guard jcc's fused
/// producer operand when provenance matches, else the recorded CmpOperand.
/// The value holds at dispatch-block entry, before this block's own stores.
static std::pair<CmpOperand, int64_t> switchLoc(const FuncRecord &F,
                                                const BlockRec &BR) {
  CmpOperand Loc;
  int64_t Off = 0;
  if (auto Meta = F.blockSwitchIndex(BR)) {
    Loc = Meta->first;
    Off = Meta->second;
  } else {
    Loc.Tag = CmpOperand::Kind::Reg;
    Loc.Reg = (unsigned)llvm::X86::RAX;
  }
  return {Loc, Off};
}

/// The last instruction of a dispatch block (before its indirect jump) that
/// loads through a scaled index, `mov[sxd] reg, [base + 4*idx]`, with the
/// index register. That load reads the jump table.
static std::optional<std::pair<size_t, unsigned>>
tableLoadIndex(llvm::ArrayRef<Instr> Instrs, const BlockRec &BR,
               const llvm::MCInstrInfo &MII) {
  for (size_t N = BR.InstrsEnd - 1; N-- > BR.InstrsBegin;) {
    const llvm::MCInst &Inst = Instrs[N].Inst;
    const llvm::MCInstrDesc &Desc = MII.get(Inst.getOpcode());
    if (!Desc.mayLoad())
      continue;
    int Mem = llvm::X86II::getMemoryOperandNo(Desc.TSFlags);
    if (Mem < 0)
      continue;
    Mem += llvm::X86II::getOperandBias(Desc);
    const llvm::MCOperand &Scale = Inst.getOperand(Mem + llvm::X86::AddrScaleAmt);
    const llvm::MCOperand &Index = Inst.getOperand(Mem + llvm::X86::AddrIndexReg);
    if (Scale.isImm() && (Scale.getImm() == 4 || Scale.getImm() == 8) &&
        Index.isReg() && Index.getReg())
      return std::make_pair(N, unsigned(Index.getReg()));
    return std::nullopt;
  }
  return std::nullopt;
}

/// True when an instruction in [Begin, End) writes Reg or an alias of it.
static bool clobbersBefore(llvm::ArrayRef<Instr> Instrs, size_t Begin,
                           size_t End, unsigned Reg,
                           const llvm::MCInstrInfo &MII,
                           const llvm::MCRegisterInfo &MRI) {
  for (size_t N = Begin; N < End; ++N)
    if (MII.get(Instrs[N].Inst.getOpcode())
            .hasDefOfPhysReg(Instrs[N].Inst, Reg, MRI))
      return true;
  return false;
}

/// True for the trap instructions (ud0/ud1/ud2, the `TRAP` opcode): they
/// never return, so the block they sit in ends with `unreachable`.
static bool isTrapInsn(const Instr &I) {
  return I.Inst.getOpcode() == llvm::X86::TRAP;
}

/// The source-level signature in the grammar the Unabi pass reads:
/// "<param> ... => <ret>", with "p" for a pointer, "f32"/"f64" for a
/// float, "rN" for an integer of N bits and "v" for a void return. The
/// recovered signature is the only place the source shape of a function
/// is known, so the lifter publishes it: without the attribute the Unabi
/// pass has no input and never fires.
///
/// An empty result means the recovered signature says nothing the flat
/// ABI does not already say (the widths and the float kinds are the ones
/// the lifter built the function type from). Such a signature would give
/// the Unabi pass nothing to rewrite, and carrying it on every function
/// costs the whole pipeline an attribute list per function, so it is left
/// out.
static std::string sigString(const Signature &S) {
  auto IntBits = [](unsigned Bits) {
    return (Bits == 8 || Bits == 16 || Bits == 32) ? Bits : 64;
  };
  bool Pointer = false;
  for (const Param &P : S.Params)
    if (P.Type.find('*') != std::string::npos)
      Pointer = true;
  if (!Pointer && !S.VarArgs)
    return "";
  std::string Out;
  for (const Param &P : S.Params) {
    if (P.Type.find('*') != std::string::npos)
      Out += "p";
    else if (P.Float)
      Out += (P.Bits == 32 ? "f32" : "f64");
    else
      Out += "r" + std::to_string(IntBits(P.Bits));
    Out += " ";
  }
  if (S.VarArgs)
    Out += "... ";
  Out += "=> ";
  if (!S.Ret)
    Out += "v";
  else if (S.Ret->Float)
    Out += (S.Ret->Bits == 32 ? "f32" : "f64");
  else
    Out += "r" + std::to_string(IntBits(S.Ret->Bits));
  return Out;
}

static llvm::Error liftTerminator(LiftState &LS, const FuncRecord &F,
                                  const BlockRec &BR, const Instr &I,
                                  const Image &Img) {
  llvm::BasicBlock *From = LS.Blocks.find(BR.startVa(Img))->second;
  LS.B.SetInsertPoint(From);
  auto EmitUnreachable = [&] {
    auto *UI = LS.B.CreateUnreachable();
    attachVa(UI, I.Addr.Value);
  };
  auto EmitBranch = [&](size_t EdgeIdx) {
    if (llvm::BasicBlock *To = edgeBlock(LS, F, Img, EdgeIdx)) {
      llvm::BranchInst *BI = LS.B.CreateBr(To);
      attachVa(BI, I.Addr.Value);
    } else {
      EmitUnreachable();
    }
  };

  if (I.Flow == FlowKind::Return) {
    const Signature *Sig = signatureFor(LS.Sigs, F.Start);
    if (!Sig || !Sig->Ret) {
      llvm::ReturnInst *RI = LS.B.CreateRetVoid();
      attachVa(RI, I.Addr.Value);
    } else {
      llvm::Value *Ret = nullptr;
      unsigned Bits = std::max(1u, Sig->Ret->Bits);
      if (Sig->Ret->Float) {
        Ret = LS.regRead(llvm::X86::XMM0);
        // xmm slots are i128: truncate to the scalar width first (a
        // direct i128->double bitcast is an invalid cast); widths
        // above 64 still return one double in the low lane.
        unsigned ValBits = Bits == 32 ? 32 : 64;
        llvm::Type *Ty = ValBits == 32
                             ? llvm::Type::getFloatTy(LS.Mod.getContext())
                             : llvm::Type::getDoubleTy(LS.Mod.getContext());
        Ret = LS.B.CreateBitCast(LS.B.CreateTrunc(Ret, LS.iN(ValBits)),
                                 Ty, "retf");
      } else {
        Ret = LS.regRead(llvm::X86::RAX);
        Ret = LS.extTo(Ret, Bits);
      }
      llvm::ReturnInst *RI = LS.B.CreateRet(Ret);
      attachVa(RI, I.Addr.Value);
    }
    return llvm::Error::success();
  }

  if (I.Flow == FlowKind::Trap) {
    EmitUnreachable();
    return llvm::Error::success();
  }

  if (I.Flow == FlowKind::CondBranch) {
    llvm::Value *Cond = LS.B.getFalse();
    if (auto CC = LS.condCodeOf(I))
      Cond = LS.condFor(*CC, I.Addr);
    // The taken edge is whichever edge targets the jcc operand (the
    // SwitchDefault edge is the taken side for `ja` guards).
    std::optional<size_t> Taken, Other;
    for (const EdgeRec &E : F.edgesOf(BR)) {
      size_t Idx = &E - F.Edges.data();
      Va To = F.edgeTo(Img, Idx);
      if (I.HasTarget && To == I.Target)
        Taken = Idx;
      else if (!Other)
        Other = Idx;
    }
    auto ArmFor = [&](std::optional<size_t> Idx) -> llvm::BasicBlock * {
      if (!Idx)
        return nullptr;
      if (llvm::BasicBlock *To = edgeBlock(LS, F, Img, *Idx))
        return To;
      if (F.Edges[*Idx].Kind != EdgeTag::TailCall)
        return nullptr;
      // jcc targets a tail call: emit a one-off block that tail-calls
      // the target and returns in the caller's own shape.
      llvm::BasicBlock *Arm =
          llvm::BasicBlock::Create(LS.Mod.getContext(), "", &LS.Fn);
      llvm::IRBuilder<>::InsertPointGuard Guard(LS.B);
      LS.B.SetInsertPoint(Arm);
      LS.emitTailCall(F.edgeTo(Img, *Idx));
      return Arm;
    };
    llvm::BasicBlock *T = ArmFor(Taken);
    llvm::BasicBlock *O = ArmFor(Other);
    if (T && O) {
      auto *BI = LS.B.CreateCondBr(Cond, T, O);
      attachVa(BI, I.Addr.Value);
    } else if (T) {
      EmitBranch(*Taken);
    } else if (O) {
      EmitBranch(*Other);
    } else {
      EmitUnreachable();
    }
    return llvm::Error::success();
  }

  if (I.Flow == FlowKind::Call) {
    // Terminating call: noreturn callee or dead end.
    if (llvm::Error E = LS.liftInstr(I)) {
      EmitUnreachable();
      return std::move(E);
    }
    EmitUnreachable();
    return llvm::Error::success();
  }

  // Fallthrough/unconditional: first Jump/Fallthrough/CondNotTaken edge.
  for (const EdgeRec &E : F.edgesOf(BR)) {
    size_t Idx = &E - F.Edges.data();
    if (E.Kind == EdgeTag::TailCall) {
      Va Target = F.edgeTo(Img, Idx);
      LS.emitTailCall(Target);
      return llvm::Error::success();
    }
    if (E.Kind == EdgeTag::Jump || E.Kind == EdgeTag::Fallthrough ||
        E.Kind == EdgeTag::CondNotTaken) {
      EmitBranch(Idx);
      return llvm::Error::success();
    }
  }
  EmitUnreachable();
  return llvm::Error::success();
}

/// LLVM `switch` for a resolved jump-table dispatch. Mirrors
/// crates/lift block.rs emit_switch: the guard jcc's fused producer operand
/// is the table index when provenance matches, else the CmpOperand read at
/// the guard block end.
static llvm::Error emitSwitch(LiftState &LS, const FuncRecord &F,
                              const BlockRec &BR, const Instr &Last,
                              const Image &Img,
                              llvm::Value *EntryBase = nullptr) {
  Va Start = BR.startVa(Img);
  llvm::BasicBlock *Cur = LS.Blocks.find(Start)->second;
  auto [Loc, Off] = switchLoc(F, BR);
  // Prefer the index read at dispatch-block entry (stashed before the body
  // lifted): later stores in this block, like Win64 arg shuffles, must not
  // leak into the dispatch operand. Fall back to a read at the end.
  llvm::Value *Base =
      EntryBase ? EntryBase : LS.evalCmpOperand(Loc, Start);
  int64_t MinCase = 0;
  bool HaveMin = false;
  for (size_t E = BR.EdgesBegin; E < BR.EdgesEnd; ++E) {
    if (F.Edges[E].Kind != EdgeTag::Switch)
      continue;
    for (const SwitchEdge &SE : F.SwitchEdges)
      if (SE.Edge == E)
        for (int64_t V : SE.Values)
          if (!HaveMin || V < MinCase) {
            MinCase = V;
            HaveMin = true;
          }
  }
  int64_t Total = Off + MinCase;
  LS.B.SetInsertPoint(Cur);
  llvm::Value *Operand = LS.extTo(Base, 64);
  if (Total != 0)
    Operand = LS.B.CreateAdd(Operand, LS.ci((uint64_t)Total), "sw");
  llvm::Value *Operand32 =
      LS.B.CreateTruncOrBitCast(Operand, LS.iN(32), "sw32");
  // Default: the SwitchDefault edge target when present, else the last edge.
  llvm::BasicBlock *Default = nullptr;
  for (size_t E = BR.EdgesBegin; E < BR.EdgesEnd; ++E)
    if (F.Edges[E].Kind == EdgeTag::SwitchDefault)
      Default = edgeBlock(LS, F, Img, E);
  if (!Default && BR.EdgesBegin < BR.EdgesEnd)
    Default = edgeBlock(LS, F, Img, BR.EdgesEnd - 1);
  if (!Default) {
    Default = llvm::BasicBlock::Create(LS.Mod.getContext(), "", &LS.Fn);
    ir::nameBlock(*Default);
    LS.B.SetInsertPoint(Default);
    auto *UI = LS.B.CreateUnreachable();
    attachVa(UI, Last.Addr.Value);
    LS.B.SetInsertPoint(Cur);
  }
  llvm::SwitchInst *SI =
      LS.B.CreateSwitch(Operand32, Default, BR.EdgesEnd - BR.EdgesBegin);
  for (size_t E = BR.EdgesBegin; E < BR.EdgesEnd; ++E) {
    if (F.Edges[E].Kind != EdgeTag::Switch)
      continue;
    llvm::BasicBlock *To = edgeBlock(LS, F, Img, E);
    if (!To)
      continue;
    for (const SwitchEdge &SE : F.SwitchEdges) {
      if (SE.Edge != E)
        continue;
      for (int64_t V : SE.Values)
        SI->addCase(LS.ci((uint32_t)V, 32), To);
    }
  }
  attachVa(SI, Last.Addr.Value);
  return llvm::Error::success();
}

llvm::Expected<LiftedFunction>
liftFunction(llvm::Module &M, const Program &P, const FuncRecord &F,
             const Disasm &D, const Signatures *Sigs, llvm::TargetMachine *TM,
             bool RunPasses, const FunctionEH *EH) {
  if (TM) {
    if (M.getTargetTriple().empty())
      M.setTargetTriple(TM->getTargetTriple());
    if (M.getDataLayoutStr().empty())
      M.setDataLayout(TM->createDataLayout());
  }
  if (M.getTargetTriple().empty()) {
    M.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
    M.setDataLayout("e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-"
                    "f80:128-n8:16:32:64-S128");
  }
  if (!M.getModuleFlag("revenant.lang"))
    M.addModuleFlag(llvm::Module::Override, "revenant.lang",
                    llvm::MDString::get(M.getContext(), "c"));

  std::vector<Instr> Instrs;
  Instrs.reserve(F.InstrRvas.size());
  for (Rva R : F.InstrRvas) {
    auto I = D.decodeOne(P.Img->vaOfRva(R));
    if (!I)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "failed to decode instruction at 0x%llx",
                                     static_cast<unsigned long long>(
                                         P.Img->vaOfRva(R).Value));
    Instrs.push_back(std::move(*I));
  }
  auto StackOrErr = analyzeStack(P, F, D, Instrs);
  if (!StackOrErr)
    return StackOrErr.takeError();

  const Signature *Sig = signatureFor(Sigs, F.Start);
  std::string Name = definitionName(F);
  llvm::Function *Fn = claimDefinition(M, Name, Sig);
  if (!Fn)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "function already defined: %s",
                                   Name.c_str());
  ir::setFunctionVA(*Fn, F.Start.Value);
  // Publish the recovered signature for the Unabi pass, the only consumer
  // of the source-level shape of a function.
  if (Sig) {
    std::string SigText = sigString(*Sig);
    if (!SigText.empty())
      Fn->addFnAttr("revenant.sig", SigText);
  }
  if (auto It = P.Embedded.find(F.Start); It != P.Embedded.end()) {
    const EmbeddedFunctionMetadata &Meta = It->second;
    if (!Meta.SourceFile.empty()) {
      llvm::Metadata *Location[] = {
          llvm::MDString::get(M.getContext(), Meta.SourceFile),
          llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
              llvm::Type::getInt32Ty(M.getContext()), Meta.Line)),
          llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
              llvm::Type::getInt32Ty(M.getContext()), Meta.Column))};
      Fn->setMetadata("rev.source", llvm::MDNode::get(M.getContext(), Location));
    }
    if (!Meta.PanicMessages.empty()) {
      std::vector<llvm::Metadata *> Messages;
      Messages.reserve(Meta.PanicMessages.size());
      for (const std::string &Message : Meta.PanicMessages)
        Messages.push_back(llvm::MDString::get(M.getContext(), Message));
      Fn->setMetadata("rev.panic", llvm::MDNode::get(M.getContext(), Messages));
    }
    if (!Meta.ExceptionTypes.empty()) {
      std::vector<llvm::Metadata *> Types;
      Types.reserve(Meta.ExceptionTypes.size());
      for (const std::string &Type : Meta.ExceptionTypes)
        Types.push_back(llvm::MDString::get(M.getContext(), Type));
      Fn->setMetadata("rev.eh.types", llvm::MDNode::get(M.getContext(), Types));
    }
    if (!Meta.ClassNames.empty()) {
      std::vector<llvm::Metadata *> Classes;
      Classes.reserve(Meta.ClassNames.size());
      for (const std::string &Class : Meta.ClassNames)
        Classes.push_back(llvm::MDString::get(M.getContext(), Class));
      Fn->setMetadata("rev.class", llvm::MDNode::get(M.getContext(), Classes));
    }
    if (!Meta.MetadataStrings.empty()) {
      std::vector<llvm::Metadata *> Strings;
      Strings.reserve(Meta.MetadataStrings.size());
      for (const std::string &String : Meta.MetadataStrings)
        Strings.push_back(llvm::MDString::get(M.getContext(), String));
      Fn->setMetadata("rev.strings", llvm::MDNode::get(M.getContext(), Strings));
    }
    // Unroll evidence: repeated straight-line asm windows sharing this
    // function's single Location record. Gated on crash metadata so
    // --meta=none (empty Embedded) emits nothing. Confirmation only.
    if (!Meta.SourceFile.empty() || !Meta.PanicMessages.empty()) {
      if (unsigned D = repeatedWindowFactor(Instrs))
        Fn->setMetadata("rev.unroll",
                        llvm::MDNode::get(M.getContext(),
                                          llvm::ConstantAsMetadata::get(
                                              llvm::ConstantInt::get(
                                                  llvm::Type::getInt32Ty(
                                                      M.getContext()),
                                                  D))));
    }
  }
  // Cold-part boundaries: the function's disjoint code ranges as
  // (start, end) VA pairs. Plain address facts, attached regardless of
  // meta mode.
  if (F.Chunks.size() > 1) {
    std::vector<llvm::Metadata *> Ranges;
    for (const auto &[Lo, Hi] : F.Chunks) {
      llvm::Metadata *Ends[] = {
          llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
              llvm::Type::getInt64Ty(M.getContext()), Lo.Value)),
          llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
              llvm::Type::getInt64Ty(M.getContext()), Hi.Value))};
      Ranges.push_back(llvm::MDNode::get(M.getContext(), Ends));
    }
    Fn->setMetadata("rev.chunks", llvm::MDNode::get(M.getContext(), Ranges));
  }
  if (!P.EmbeddedTypes.empty()) {
    llvm::NamedMDNode *Types = M.getOrInsertNamedMetadata("rev.types");
    if (Types->getNumOperands() == 0) {
      for (const EmbeddedTypeMetadata &Type : P.EmbeddedTypes) {
        // Row layout: name, decorated name, vftable (null when the class has
        // none), base class names, then virtual function addresses.
        std::vector<llvm::Metadata *> Fields{
            llvm::MDString::get(M.getContext(), Type.Name),
            llvm::MDString::get(M.getContext(), Type.DecoratedName)};
        if (Type.VTable.Value)
          Fields.push_back(llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
              llvm::Type::getInt64Ty(M.getContext()), Type.VTable.Value)));
        else
          Fields.push_back(llvm::MDNode::get(M.getContext(), {}));
        for (const std::string &Base : Type.BaseClasses)
          Fields.push_back(llvm::MDString::get(M.getContext(), Base));
        for (Va Target : Type.VirtualFunctions)
          Fields.push_back(llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
              llvm::Type::getInt64Ty(M.getContext()), Target.Value)));
        Types->addOperand(llvm::MDNode::get(M.getContext(), Fields));
      }
    }
  }
  if (Sig && Sig->NoReturn)
    Fn->addFnAttr(llvm::Attribute::NoReturn);
  if (Sig)
    for (const Param &P : Sig->Params)
      if (P.Name && *P.Name == "frame") {
        Fn->addFnAttr("revenant.msvc.funclet");
        break;
      }

  while (!Fn->empty())
    Fn->begin()->eraseFromParent();
  // LLVM forbids predecessors of the entry block. When a loop or jump targets
  // the function's first instruction, the setup block is a separate entry
  // that branches to the block holding that code.
  bool EntryIsTarget = false;
  for (const BlockRec &BR : F.Blocks)
    for (const EdgeRec &E : F.edgesOf(BR)) {
      size_t Idx = &E - F.Edges.data();
      if (E.Kind != EdgeTag::TailCall && F.edgeTo(*P.Img, Idx) == F.Start)
        EntryIsTarget = true;
    }
  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(M.getContext(), "", Fn);
  if (EntryIsTarget)
    ir::nameBlock(*Entry);
  else
    ir::nameBlock(*Entry, F.Start.Value);
  LiftState LS(M, *Fn, P, F, D, Sigs, std::move(*StackOrErr), Instrs);
  if (!EntryIsTarget)
    LS.Blocks.emplace(F.Start, Entry);
  LS.B.SetInsertPoint(Entry);

  // Register file: 15 GPR roots as i64, xmm0-15 as i128, 5 flags as i1.
  static const llvm::MCRegister Gprs[] = {
      llvm::X86::RAX, llvm::X86::RCX, llvm::X86::RDX, llvm::X86::RBX,
      llvm::X86::RSI, llvm::X86::RDI, llvm::X86::RBP, llvm::X86::R8,
      llvm::X86::R9, llvm::X86::R10, llvm::X86::R11, llvm::X86::R12,
      llvm::X86::R13, llvm::X86::R14, llvm::X86::R15};
  for (llvm::MCRegister R : Gprs) {
    std::string N = std::string(LS.MC.MRI->getName(R));
    llvm::transform(N, N.begin(), ::tolower);
    LS.Gpr[R] = LS.entryAlloca(LS.i64(), N);
  }
  for (unsigned K = 0; K < 16; ++K)
    LS.Xmm[K] = LS.entryAlloca(LS.iN(128), "xmm" + std::to_string(K));
  for (llvm::StringRef Fl : {"zf", "sf", "cf", "of", "pf"})
    LS.Flags[Fl] = LS.entryAlloca(LS.iN(1), Fl);

  // Frame: [max rsp depth low .. sp0 .. args high].
  int64_t MaxDelta = LS.Stack.FrameSize;
  for (auto &[VA, D2] : LS.Stack.Deltas)
    MaxDelta = std::max(MaxDelta, D2);
  uint64_t Low = (uint64_t)std::max<int64_t>(MaxDelta, 0) + 8;
  uint64_t High = (uint64_t)std::max<int64_t>(LS.Stack.ArgsSize, 0x28) + 0x100;
  LS.Frame = LS.entryAlloca(
      llvm::ArrayType::get(LS.iN(8), (unsigned)(Low + High)), "frame");
  LS.Sp0 =
      LS.B.CreateInBoundsGEP(LS.iN(8), LS.Frame, {LS.ci(Low)}, "sp0");
  if (LS.Stack.DynVA) {
    LS.DynSp = LS.entryAlloca(LS.ptrTy(), "dynslot");
    LS.B.CreateStore(
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(
            LS.ptrTy())),
        LS.DynSp);
  }
  // Incoming parameters into their locations.
  if (Sig) {
    unsigned K = 0;
    for (const Param &Pm : Sig->Params) {
      if (K >= Fn->arg_size())
        break;
      llvm::Argument &A = *(Fn->arg_begin() + K++);
      if (Pm.Loc.IsStack) {
        LS.B.CreateAlignedStore(&A, LS.framePtrOff(Pm.Loc.StackOff),
                                llvm::Align(1));
      } else if (Pm.Float) {
        auto It = LS.Xmm.find(Pm.Loc.Reg == llvm::X86::XMM0   ? 0u
                              : Pm.Loc.Reg == llvm::X86::XMM1 ? 1u
                              : Pm.Loc.Reg == llvm::X86::XMM2 ? 2u
                              : Pm.Loc.Reg == llvm::X86::XMM3 ? 3u
                                                              : 0u);
        if (It != LS.Xmm.end()) {
          llvm::Type *AT = A.getType();
          llvm::Value *Vec;
          if (AT->isFloatTy()) {
            auto *VT = llvm::FixedVectorType::get(AT, 4);
            Vec = LS.B.CreateInsertElement(llvm::Constant::getNullValue(VT),
                                           &A, LS.ci(0, 32), "i");
          } else {
            auto *VT = llvm::FixedVectorType::get(AT, 2);
            Vec = LS.B.CreateInsertElement(llvm::Constant::getNullValue(VT),
                                           &A, LS.ci(0, 32), "i");
          }
          LS.B.CreateStore(LS.B.CreateBitCast(Vec, LS.iN(128), "bc"),
                           It->second);
        }
      } else {
        auto It = LS.Gpr.find(rootRegOf(Pm.Loc.Reg, *LS.MC.MRI));
        if (It != LS.Gpr.end())
          LS.B.CreateStore(LS.extTo(&A, 64), It->second);
      }
    }
  }
  // Non-parameter registers start undefined.
  {
    bool IsParam[15] = {};
    if (Sig)
      for (const Param &Pm : Sig->Params) {
        if (Pm.Loc.IsStack || Pm.Float)
          continue;
        llvm::MCRegister Root = rootRegOf(Pm.Loc.Reg, *LS.MC.MRI);
        for (unsigned K = 0; K < 15; ++K)
          if (Gprs[K] == Root)
            IsParam[K] = true;
      }
    for (unsigned K = 0; K < 15; ++K)
      if (!IsParam[K])
        LS.B.CreateStore(llvm::UndefValue::get(LS.i64()), LS.Gpr[Gprs[K]]);
    for (auto &[N, Al] : LS.Flags)
      LS.B.CreateStore(llvm::UndefValue::get(LS.iN(1)), Al);
  }

  // Blocks (first record wins on duplicate starts).
  for (const BlockRec &BR : F.Blocks) {
    Va Start = BR.startVa(*P.Img);
    if (LS.Blocks.count(Start))
      continue;
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(M.getContext(), "", Fn);
    ir::nameBlock(*BB, Start.Value);
    LS.Blocks.emplace(Start, BB);
  }
  if (EntryIsTarget) {
    auto It = LS.Blocks.find(F.Start);
    if (It != LS.Blocks.end())
      attachVa(LS.B.CreateBr(It->second), F.Start.Value);
    else
      attachVa(LS.B.CreateUnreachable(), F.Start.Value);
  }

  for (const BlockRec &BR : F.Blocks) {
    Va Start = BR.startVa(*P.Img);
    auto It = LS.Blocks.find(Start);
    if (It == LS.Blocks.end())
      continue;
    // Skip duplicate block records.
    bool First = true;
    for (const BlockRec &Prev : F.Blocks) {
      if (&Prev == &BR)
        break;
      if (Prev.startVa(*P.Img) == Start)
        First = false;
    }
    if (!First)
      continue;
    LS.B.SetInsertPoint(It->second);
    LS.Prod.reset();
    if (BR.InstrsBegin >= BR.InstrsEnd || BR.InstrsEnd > Instrs.size()) {
      auto *UI = LS.B.CreateUnreachable();
      attachVa(UI, Start.Value);
      continue;
    }
    // Switch dispatch blocks: read the traced index now, at block entry.
    // The block body can shuffle the index register afterwards (Win64 arg
    // moves), and a read at the end would dispatch on the wrong value.
    llvm::Value *SwitchEntry = nullptr;
    // When the block computes the index itself (a niche-enum decode such as
    // `xor r8, rax; cmovs rax, r8`), the traced register is written before
    // the table load and its entry value is stale. The index register of
    // the table load, read right before that load, is what the hardware
    // dispatches on.
    size_t TableLoad = SIZE_MAX;
    unsigned TableIndex = 0;
    if (Instrs[BR.InstrsEnd - 1].Flow == FlowKind::IndirectBranch &&
        BR.SwitchIndex != UINT32_MAX) {
      auto [SLoc, SOff] = switchLoc(F, BR);
      SwitchEntry = LS.evalCmpOperand(SLoc, Start);
      if (SOff == 0 && SLoc.Tag == CmpOperand::Kind::Reg)
        if (auto Load = tableLoadIndex(Instrs, BR, *LS.MC.MII);
            Load && clobbersBefore(Instrs, BR.InstrsBegin, Load->first,
                                   SLoc.Reg, *LS.MC.MII, *LS.MC.MRI)) {
          TableLoad = Load->first;
          TableIndex = Load->second;
        }
    }
    // Windows targets lower IR `unreachable` to ud2 (TrapUnreachable), so a
    // ud2 right after a call is the `unreachable` behind a noreturn callee;
    // any other ud2 is the program's own trap (`llvm.trap`).
    auto EmitTrap = [&](const Instr &I, const Instr *Prev) {
      LS.setVA(I.Addr);
      if (!Prev || Prev->Flow != FlowKind::Call) {
        auto *T = LS.B.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{},
                                       llvm::ArrayRef<llvm::Value *>{});
        attachVa(T, I.Addr.Value);
      }
      auto *UI = LS.B.CreateUnreachable();
      attachVa(UI, I.Addr.Value);
    };
    // Set when a trap instruction ended the block before its last
    // instruction; the rest of the block is dead code.
    bool Trapped = false;
    for (size_t N = BR.InstrsBegin; N + 1 < BR.InstrsEnd; ++N) {
      const Instr &I = Instrs[N];
      // ud2 (and its ud0/ud1 siblings) never returns. Lifting it as an
      // opaque `revenant.asm.ud2` call would make every instruction after
      // it depend on a clobbered register, and the cleanup pass then drops
      // the rest of the function. The trap ends the block instead, and the
      // dead code behind it is not lifted at all.
      if (isTrapInsn(I)) {
        EmitTrap(I, N > BR.InstrsBegin ? &Instrs[N - 1] : nullptr);
        Trapped = true;
        break;
      }
      if (N == TableLoad) {
        CmpOperand Index;
        Index.Reg = TableIndex;
        SwitchEntry = LS.evalCmpOperand(Index, Start);
      }
      if (llvm::Error E = LS.liftInstr(I)) {
        Fn->deleteBody();
        return std::move(E);
      }
    }
    if (Trapped)
      continue;
    const Instr &Last = Instrs[BR.InstrsEnd - 1];
    if (isTrapInsn(Last)) {
      EmitTrap(Last, BR.InstrsEnd - 1 > BR.InstrsBegin ? &Instrs[BR.InstrsEnd - 2] : nullptr);
      continue;
    }
    LS.setVA(Last.Addr);
    if (Last.Flow == FlowKind::Call) {
      if (llvm::Error E = LS.liftInstr(Last)) {
        Fn->deleteBody();
        return std::move(E);
      }
      if (BR.EdgesBegin < BR.EdgesEnd) {
        if (llvm::BasicBlock *To = edgeBlock(LS, F, *P.Img, BR.EdgesBegin)) {
          llvm::BranchInst *BI = LS.B.CreateBr(To);
          attachVa(BI, Last.Addr.Value);
        } else {
          auto *UI = LS.B.CreateUnreachable();
          attachVa(UI, Last.Addr.Value);
        }
      } else {
        auto *UI = LS.B.CreateUnreachable();
        attachVa(UI, Last.Addr.Value);
      }
    } else if (Last.Flow == FlowKind::Branch ||
               Last.Flow == FlowKind::IndirectBranch) {
      if (Last.Flow == FlowKind::IndirectBranch &&
          BR.SwitchIndex != UINT32_MAX) {
        if (llvm::Error E = emitSwitch(LS, F, BR, Last, *P.Img, SwitchEntry)) {
          Fn->deleteBody();
          return std::move(E);
        }
      } else {
        if (Last.Flow == FlowKind::IndirectBranch &&
            BR.EndKind != BlockEnd::TailCall)
          LS.liftOpaque(Last);
        if (BR.EndKind == BlockEnd::TailCall) {
          // jmp to another function: direct target -> tail call,
          // computed -> indirect tail call.
          bool Done = false;
          for (const EdgeRec &E : F.edgesOf(BR)) {
            size_t Idx = &E - F.Edges.data();
            if (E.Kind == EdgeTag::TailCall) {
              LS.emitTailCall(F.edgeTo(*P.Img, Idx));
              Done = true;
              break;
            }
          }
          if (!Done)
            LS.emitIndirectTailCall(Last);
        } else if (llvm::Error E = liftTerminator(LS, F, BR, Last, *P.Img)) {
          Fn->deleteBody();
          return std::move(E);
        }
      }
    } else {
      // Fallthrough end. Conditional branches, returns and traps carry no
      // work of their own: liftTerminator lowers them.
      if (Last.Flow == FlowKind::None) {
        if (llvm::Error E = LS.liftInstr(Last)) {
          Fn->deleteBody();
          return std::move(E);
        }
      }
      if (llvm::Error E = liftTerminator(LS, F, BR, Last, *P.Img)) {
        Fn->deleteBody();
        return std::move(E);
      }
    }
  }
  LS.tagProvenance();
  // Exception regions from the EH tables: calls inside try ranges become
  // invokes with native LLVM pads (owner: EH slice). Runs before verify
  // so malformed pads fail the lift instead of escaping downstream.
  if (EH)
    liftEH(*Fn, *EH);
  if (llvm::verifyFunction(*Fn, &llvm::errs())) {
    Fn->deleteBody();
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "LLVM verification failed for %s",
                                   Name.c_str());
  }
  if (RunPasses) {
    llvm::PassBuilder PB(TM);
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager MPM;
    if (llvm::Error E = PB.parsePassPipeline(MPM, liftCleanupPasses()))
      return std::move(E);
    MPM.run(M, MAM);
  }
  LiftedFunction Result;
  Result.Fn = Fn;
  Result.BlockMap = LS.Blocks;
  Result.AsmFallbacks = LS.Fallbacks;
  return Result;
}

} // namespace revenant::lift
