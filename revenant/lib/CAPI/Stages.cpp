//==- lib/CAPI/Stages.cpp - the decompile stage pipeline ------------------==//
//
// Per run: lift -> ir-raw -> cleanup+idioms -> ir-clean -> unopt+structure+
// types+ssaout -> ir-final -> source. Each stage writes its artifact next
// to the output and reports into StageReport; stages whose real
// implementation has not landed run a visible fallback (status "stub").
//
// Wiring points for landing slices:
//  - lift:       fallbackLift() below is replaced by the real lifter once
//                revenant/Lift/Lift.h exists (Swe2LiftFunction).
//  - passes:     registerRevenantPasses prefers lib/Transforms/Pipeline's
//                Register.cpp (MuseTxPipeline); each known pass header is
//                also registered individually so the tree works when only
//                some passes merged.
//  - source:     Clang frontend once RevenantClangCore is linkable;
//                rustc fork for rust once LunaRustcCore's build is green.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Session.h"

#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"
#include "revenant/Object/PeFile.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <numeric>
#include <set>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

// Optional slice headers: register what exists, strip what does not.
#if __has_include("revenant/Transforms/Pipeline/Pipeline.h")
#include "revenant/Transforms/Pipeline/Pipeline.h"
#define RV_HAVE_TX_PIPELINE 1
#endif
#if __has_include("revenant/Transforms/Idioms/Idioms.h")
#include "revenant/Transforms/Idioms/Idioms.h"
#define RV_HAVE_IDIOMS 1
#endif
#if __has_include("revenant/Transforms/Devectorize/Devectorize.h")
#include "revenant/Transforms/Devectorize/Devectorize.h"
#define RV_HAVE_DEVEC 1
#endif
#if __has_include("revenant/Transforms/Reroll/Reroll.h")
#include "revenant/Transforms/Reroll/Reroll.h"
#define RV_HAVE_REROLL 1
#endif
#if __has_include("revenant/Transforms/Unrotate/Unrotate.h")
#include "revenant/Transforms/Unrotate/Unrotate.h"
#define RV_HAVE_UNROTATE 1
#endif
#if __has_include("revenant/Transforms/Patterns/Patterns.h")
#include "revenant/Transforms/Patterns/Patterns.h"
#define RV_HAVE_PATTERNS 1
#endif
#if __has_include("revenant/Transforms/TypeRecovery/TypeRecovery.h")
#include "revenant/Transforms/TypeRecovery/TypeRecovery.h"
#define RV_HAVE_TYPES 1
#endif
#if __has_include("revenant/Structure/RevenantStructure.h")
#include "revenant/Structure/RevenantStructure.h"
#define RV_HAVE_STRUCTURE 1
#endif
#if __has_include("revenant/Lift/LiftFunction.h")
#include "revenant/Lift/LiftFunction.h"
#define RV_HAVE_LIFT 1
#endif
#if __has_include("../Transforms/SsaOut/SsaOut.h")
#include "../Transforms/SsaOut/SsaOut.h"
#define RV_HAVE_SSAOUT 1
#endif

#if defined(REVENANT_HAVE_CLANG_FRONTEND)
#include "revenant/Clang/ClangFrontend.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"
#endif
#include "llvm/Support/Program.h"

using namespace llvm;

namespace revenant {
namespace {

//==--------------------------------------------------------------------==//
// Small helpers
//==--------------------------------------------------------------------==//

/// Any string -> C identifier (revenant_asm_*, imp_*, bb_*).
std::string cIdent(StringRef S) {
  std::string Out;
  for (char C : S)
    Out.push_back(isAlnum(C) || C == '_' ? C : '_');
  if (Out.empty() || isDigit(Out[0]))
    Out.insert(Out.begin(), '_');
  return Out;
}

/// First mnemonic token of the Intel-format text, lowercased; prefixes
/// (lock/rep*) fold into the real mnemonic.
std::string mnemonicOf(const revenant::Instr &I, Disasm &D) {
  std::string Formatted = D.format(I);
  StringRef Text(Formatted);
  StringRef Tok = Text.take_until([](char C) { return isSpace(C); });
  std::string Out = Tok.lower();
  for (StringRef P : {"lock", "rep", "repe", "repne", "repz", "repnz",
                      "addr16", "addr32", "bnd"})
    if (Tok == P) {
      StringRef Rest = Text.drop_front(Tok.size()).ltrim();
      StringRef Next = Rest.take_until([](char C) { return isSpace(C); });
      if (!Next.empty())
        Out += "_" + Next.lower();
      break;
    }
  std::string Clean;
  for (char C : Out)
    Clean.push_back(isAlnum(C) ? C : '_');
  return Clean.empty() ? "op" : Clean;
}

std::string moduleText(const Module &M) {
  std::string S;
  raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}

Error writeText(StringRef Path, StringRef Text) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_None);
  if (EC)
    return createStringError(EC, Path);
  OS << Text;
  return Error::success();
}

void record(std::vector<StageReport> &Stages, StringRef Name, StringRef Status,
            StringRef Note, StringRef Artifact = "") {
  Stages.push_back({Name.str(), Status.str(), Note.str(), Artifact.str()});
}

/// Emit `call <ret> @revenant.asm.<mn>(args)` with !revenant.opaque and
/// optional !revenant.va provenance.
CallInst *opaqueCall(IRBuilder<> &B, Module &M, StringRef Mn, Type *RetTy,
                     ArrayRef<Value *> Args, std::optional<uint64_t> VA) {
  SmallVector<Type *, 4> ArgTys;
  for (Value *A : Args)
    ArgTys.push_back(A->getType());
  std::string Signature;
  raw_string_ostream SignatureOS(Signature);
  RetTy->print(SignatureOS);
  for (Type *ArgTy : ArgTys) {
    SignatureOS << '.';
    ArgTy->print(SignatureOS);
  }
  SignatureOS.flush();
  llvm::Function *Fn =
      ir::getOrCreateOpaqueAsmFn(M, (Mn + "." + Signature).str(), RetTy,
                                 ArgTys);
  CallInst *CI = B.CreateCall(Fn, Args);
  CI->setMetadata(ir::MDKindOpaque, MDNode::get(M.getContext(), {}));
  if (VA)
    ir::setInstructionVA(*CI, *VA);
  return CI;
}

//==--------------------------------------------------------------------==//
// Module skeleton: triple, datalayout, lang flag, extern + function
// declarations. Signatures are void() until the sigs stage lands.
//==--------------------------------------------------------------------==//

std::unique_ptr<Module> moduleFor(LLVMContext &Ctx, const Session &S, Lang Out,
                                  ArrayRef<Va> Targets) {
  auto M = std::make_unique<Module>("revenant", Ctx);
  M->setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  M->setDataLayout(
      "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:"
      "32:64-S128");
  ir::setModuleLang(*M, Out == Lang::Cpp   ? ir::Lang::CPP
                        : Out == Lang::Rust ? ir::Lang::Rust
                                            : ir::Lang::C);
  FunctionType *Sig = FunctionType::get(Type::getVoidTy(Ctx), false);
  if (S.program().Externs)
    for (const auto &[Slot, X] : *S.program().Externs)
      if (!M->getFunction(X.Name))
        llvm::Function::Create(Sig, llvm::Function::ExternalLinkage, X.Name,
                               *M);
  for (Va T : Targets) {
    std::string N = S.nameFor(T);
    llvm::Function *F = M->getFunction(N);
    if (!F)
      F = llvm::Function::Create(Sig, llvm::Function::ExternalLinkage, N,
                                 *M);
    ir::setFunctionVA(*F, T.Value);
  }
  return M;
}
llvm::Function *fallbackLift(Module &M, const Session &S, Disasm &D,
                             const revenant::Function &F);

/// Lift one function into M: the real lifter when its header is on the
/// tree, else the opaque-call fallback below. Returns the stage note.
Expected<std::string> liftOne(Module &M, const Session &S, Disasm &D,
                              const FuncRecord &Rec,
                              const revenant::Function &F) {
#if defined(RV_HAVE_LIFT)
  auto L = revenant::lift::liftFunction(M, S.program(), Rec, D,
                                        /*Sigs=*/nullptr,
                                        /*TM=*/nullptr,
                                        /*RunPasses=*/false);
  if (!L)
    return L.takeError();
  return formatv("revenant::lift::liftFunction ({0} opaque sites)",
                 L->AsmFallbacks.size());
#else
  fallbackLift(M, S, D, F);
  return std::string(
      "opaque revenant.asm.* calls + CFG terminators (fallback lifter)");
#endif
}


//==--------------------------------------------------------------------==//
// Fallback lifter: every instruction becomes an opaque revenant.asm.*
// call; block ends become real terminators (br/condbr/switch/ret/
// unreachable). Shape follows docs/ir-contract.md so frontends and the
// transforms see well-formed modules end to end.
//==--------------------------------------------------------------------==//

llvm::Function *fallbackLift(Module &M, const Session &S, Disasm &D,
                             const revenant::Function &F) {
  LLVMContext &C = M.getContext();
  llvm::Function *LF = M.getFunction(F.Name);
  if (!LF)
    LF = llvm::Function::Create(
        FunctionType::get(Type::getVoidTy(C), false),
        llvm::Function::ExternalLinkage, F.Name, M);
  ir::setFunctionVA(*LF, F.Start.Value);

  Type *Void = Type::getVoidTy(C);
  Type *I1 = Type::getInt1Ty(C);
  Type *I64 = Type::getInt64Ty(C);
  IRBuilder<> B(C);

  DenseMap<uint64_t, llvm::BasicBlock *> Bbs;
  for (const revenant::BasicBlock &BB : F.Blocks) {
    llvm::BasicBlock *LBB = llvm::BasicBlock::Create(C, "", LF);
    ir::nameBlock(*LBB, BB.Start.Value);
    Bbs[BB.Start.Value] = LBB;
  }

  auto edgeBBs = [&](const revenant::BasicBlock &BB, EdgeKind K) {
    SmallVector<llvm::BasicBlock *, 4> Out;
    for (const Edge &E : BB.Edges)
      if (E.Kind == K && Bbs.count(E.To.Value))
        Out.push_back(Bbs[E.To.Value]);
    return Out;
  };
  auto calleeFor = [&](llvm::StringRef N) {
    llvm::Function *Callee = M.getFunction(N);
    if (!Callee)
      Callee = llvm::Function::Create(
          FunctionType::get(Void, false), llvm::Function::ExternalLinkage, N,
          M);
    return Callee;
  };

  for (const revenant::BasicBlock &BB : F.Blocks) {
    llvm::BasicBlock *LBB = Bbs[BB.Start.Value];
    B.SetInsertPoint(LBB);
    const revenant::Instr *Last =
        BB.InstrsEnd > BB.InstrsBegin && BB.InstrsEnd <= F.Instrs.size()
            ? &F.Instrs[BB.InstrsEnd - 1]
            : nullptr;
    // The last instruction of a terminated block is consumed by the
    // terminator handling below (jcc predicate, jmp, ret, ud2).
    bool LastIsTerm = Last && Last->Flow != FlowKind::None &&
                      Last->Flow != FlowKind::Call;

    for (size_t I = BB.InstrsBegin; I < BB.InstrsEnd && I < F.Instrs.size();
         ++I) {
      const revenant::Instr &In = F.Instrs[I];
      bool IsLast = (I + 1 == BB.InstrsEnd);
      if (In.Flow == FlowKind::Call) {
        llvm::Function *Callee = nullptr;
        if (In.HasMemTarget && S.program().Externs &&
            S.program().Externs->count(In.MemTarget))
          Callee = calleeFor(S.program().Externs->at(In.MemTarget).Name);
        else if (In.HasTarget && S.program().Functions.count(In.Target))
          Callee = calleeFor(S.program().Functions.at(In.Target).Name);
        if (Callee) {
          CallInst *CI = B.CreateCall(Callee);
          ir::setInstructionVA(*CI, In.Addr.Value);
        } else {
          opaqueCall(B, M, mnemonicOf(In, D), Void, {}, In.Addr.Value);
        }
        continue;
      }
      if (IsLast && LastIsTerm)
        continue;
      opaqueCall(B, M, mnemonicOf(In, D), Void, {}, In.Addr.Value);
    }

    uint64_t LastVA = Last ? Last->Addr.Value : BB.Start.Value;
    switch (BB.EndKind) {
    case BlockEnd::Fallthrough: {
      auto T = edgeBBs(BB, EdgeKind::Fallthrough);
      if (T.empty())
        B.CreateUnreachable();
      else
        B.CreateBr(T.front());
      break;
    }
    case BlockEnd::Branch: {
      auto Taken = edgeBBs(BB, EdgeKind::CondTaken);
      auto Not = edgeBBs(BB, EdgeKind::CondNotTaken);
      auto Jumps = edgeBBs(BB, EdgeKind::Jump);
      if (!Taken.empty() && !Not.empty()) {
        // The jcc becomes the predicate: revenant.asm.<jcc>(i64 target).
        SmallVector<Value *, 1> Args = {ConstantInt::get(I64, 0)};
        for (const Edge &E : BB.Edges)
          if (E.Kind == EdgeKind::CondTaken)
            Args[0] = ConstantInt::get(I64, E.To.Value);
        CallInst *Pred = opaqueCall(
            B, M, Last ? mnemonicOf(*Last, D) : "jcc", I1, Args, LastVA);
        B.CreateCondBr(Pred, Taken.front(), Not.front());
      } else if (!Jumps.empty()) {
        if (Last)
          opaqueCall(B, M, mnemonicOf(*Last, D), Void, {}, LastVA);
        B.CreateBr(Jumps.front());
      } else {
        B.CreateUnreachable();
      }
      break;
    }
    case BlockEnd::TailCall: {
      if (Last)
        opaqueCall(B, M, mnemonicOf(*Last, D), Void, {}, LastVA);
      for (const Edge &E : BB.Edges)
        if (E.Kind == EdgeKind::TailCall &&
            S.program().Functions.count(E.To)) {
          B.CreateCall(calleeFor(S.program().Functions.at(E.To).Name));
          break;
        }
      B.CreateRetVoid();
      break;
    }
    case BlockEnd::Return:
      if (Last)
        opaqueCall(B, M, mnemonicOf(*Last, D), Void, {}, LastVA);
      B.CreateRetVoid();
      break;
    case BlockEnd::Noreturn:
      if (Last)
        opaqueCall(B, M, mnemonicOf(*Last, D), Void, {}, LastVA);
      B.CreateUnreachable();
      break;
    case BlockEnd::UnresolvedIndirect: {
      if (Last)
        opaqueCall(B, M, mnemonicOf(*Last, D), Void, {}, LastVA);
      auto Cases = edgeBBs(BB, EdgeKind::Switch);
      auto Def = edgeBBs(BB, EdgeKind::SwitchDefault);
      if (Cases.empty()) {
        B.CreateUnreachable();
        break;
      }
      CallInst *Sel = opaqueCall(B, M, "indirect", I64, {}, LastVA);
      SwitchInst *Sw =
          B.CreateSwitch(Sel, Def.empty() ? Cases.front() : Def.front(),
                         Cases.size());
      for (size_t I = 0; I < Cases.size(); ++I)
        Sw->addCase(ConstantInt::get(cast<IntegerType>(I64), I), Cases[I]);
      break;
    }
    case BlockEnd::EndOfFunction:
      B.CreateUnreachable();
      break;
    }
  }
  return LF;
}

//==--------------------------------------------------------------------==//
// Pass pipeline: register landed revenant-* passes, parse, strip the ones
// this binary does not know (recorded as stubbed), run.
//==--------------------------------------------------------------------==//

void registerRevenantPasses(PassBuilder &PB) {
#if defined(RV_HAVE_TX_PIPELINE)
  // Pipeline lib is the single-registration authority when present.
  revenant::registerRevenantPasses(PB);
#endif
  auto Reg = [&](StringRef Name, auto Pass) {
    PB.registerPipelineParsingCallback(
        [Name, Pass](StringRef N, FunctionPassManager &PM,
                     ArrayRef<PassBuilder::PipelineElement>) {
          if (N == Name) {
            auto P = Pass;
            PM.addPass(std::move(P));
            return true;
          }
          return false;
        });
  };
#if defined(RV_HAVE_IDIOMS)
  Reg("revenant-idioms", IdiomsPass());
#endif
#if defined(RV_HAVE_REROLL)
  Reg("revenant-reroll", RerollPass());
#endif
#if defined(RV_HAVE_UNROTATE)
  Reg("revenant-unrotate", UnrotatePass());
  Reg("revenant-untailrec", UntailRecPass());
#endif
#if defined(RV_HAVE_PATTERNS)
  Reg("revenant-patterns", PatternsPass());
#endif
#if defined(RV_HAVE_TYPES)
  Reg("revenant-types", TypeRecoveryPass());
#endif
#if defined(RV_HAVE_STRUCTURE)
  Reg("revenant-structure", RevenantStructurePass());
#endif
}

/// Remove `name` (and `name<opts>`) elements plus one adjacent comma from
/// a `function(e1,e2,...)` pipeline text.
std::string erasePass(StringRef Text, StringRef Name) {
  std::string S = Text.str();
  size_t Pos = 0;
  while ((Pos = S.find(Name.str(), Pos)) != std::string::npos) {
    size_t End = Pos + Name.size();
    bool BoundL = Pos == 0 || S[Pos - 1] == ',' || S[Pos - 1] == '(';
    char After = End < S.size() ? S[End] : '\0';
    bool BoundR = After == ',' || After == ')' || After == '<';
    if (!(BoundL && BoundR)) {
      Pos = End;
      continue;
    }
    size_t RemoveEnd = End;
    if (After == '<') {
      int Depth = 0;
      for (; RemoveEnd < S.size(); ++RemoveEnd) {
        if (S[RemoveEnd] == '<')
          ++Depth;
        else if (S[RemoveEnd] == '>' && --Depth == 0) {
          ++RemoveEnd;
          break;
        }
      }
    }
    size_t RemoveStart = Pos;
    if (RemoveEnd < S.size() && S[RemoveEnd] == ',')
      ++RemoveEnd;
    else if (RemoveStart > 0 && S[RemoveStart - 1] == ',')
      --RemoveStart;
    S.erase(RemoveStart, RemoveEnd - RemoveStart);
    Pos = RemoveStart;
  }
  return S;
}

/// Run `pipeline` text on M. Unknown passes (not yet landed) are stripped
/// one at a time from the text; each stripped name lands in `stripped`.
Error runPipelineText(Module &M, StringRef Pipeline,
                      std::vector<std::string> &Stripped) {
  PassBuilder PB;
  registerRevenantPasses(PB);
  std::string Text = Pipeline.str();
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  for (unsigned Tries = 0; Tries < 64; ++Tries) {
    ModulePassManager MPM;
    if (Error E = PB.parsePassPipeline(MPM, Text)) {
      std::string Msg = toString(std::move(E));
      size_t Q1 = Msg.find('\'');
      size_t Q2 =
          Q1 == std::string::npos ? std::string::npos : Msg.find('\'', Q1 + 1);
      if (Q1 == std::string::npos || Q2 == std::string::npos)
        return createStringError("pipeline parse failed: " + Msg);
      std::string Name = Msg.substr(Q1 + 1, Q2 - Q1 - 1);
      std::string Next = erasePass(Text, Name);
      if (Next == Text)
        return createStringError("pipeline parse failed: " + Msg);
      Stripped.push_back(std::move(Name));
      Text = std::move(Next);
      continue;
    }
    MPM.run(M, MAM);
    return Error::success();
  }
  return createStringError("pipeline parse failed after stripping passes");
}

//==--------------------------------------------------------------------==//
// Fallback source emit: valid C/Rust skeleton from contract IR.
//==--------------------------------------------------------------------==//

struct EmitResult {
  std::string Text;
  std::vector<std::pair<uint32_t, std::vector<Va>>> LineVas;
};

/// C name for a callee: opaque stubs -> revenant_asm_<mn>, everything else
/// -> imp_<name>.
std::string calleeIdent(const llvm::Function *Callee) {
  if (!Callee)
    return "revenant_asm_call";
  StringRef N = Callee->getName();
  if (N.starts_with(ir::OpaqueAsmPrefix))
    return cIdent(("revenant_asm_" +
                   N.drop_front(ir::OpaqueAsmPrefix.size()))
                      .str());
  return cIdent(("imp_" + N).str());
}

/// Contract IR -> C-shaped skeleton text. Everything that is not a
/// call/terminator the emitter understands is left as a comment; the
/// output always compiles.
EmitResult emitCFallback(Module &M) {
  EmitResult R;
  std::string Out;
  raw_string_ostream OS(Out);
  uint32_t Line = 1;
  auto emit = [&](const Twine &S) {
    OS << S << '\n';
    ++Line;
  };
  auto noteVa = [&](const llvm::Instruction &I) {
    if (auto VA = ir::getInstructionVA(I))
      R.LineVas.push_back({Line, {Va(*VA)}});
  };

  emit("#include <stdint.h>");
  emit("");
  std::set<std::string> Seen;
  for (llvm::Function &F : M) {
    if (!F.isDeclaration())
      continue;
    std::string Id = calleeIdent(&F);
    if (!Seen.insert(Id).second)
      continue;
    FunctionType *FT = F.getFunctionType();
    std::string Ret = FT->getReturnType()->isIntegerTy(1)  ? "int"
                      : FT->getReturnType()->isIntegerTy() ? "long long"
                                                           : "void";
    std::string Args;
    for (unsigned I = 0; I < FT->getNumParams(); ++I) {
      if (I)
        Args += ", ";
      Args += "unsigned long long";
    }
    emit(Ret + " " + Id + "(" + (Args.empty() ? "void" : Args) + ");");
  }
  emit("");
  emit("void revenant_asm_unreachable(void);");

  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    std::string Name = cIdent(F.getName());
    if (Name.empty())
      Name = "sub_unknown";
    emit("void " + Name + "(void) {");
    for (llvm::BasicBlock &BB : F) {
      emit("  " + cIdent(BB.getName()) + ":");
      for (llvm::Instruction &I : BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          // Predicate calls consumed by a br/switch are printed there.
          bool Consumed = llvm::any_of(I.users(), [](llvm::User *U) {
            return isa<BranchInst>(U) || isa<SwitchInst>(U);
          });
          if (Consumed)
            continue;
          std::string Call = "  " + calleeIdent(CI->getCalledFunction()) + "(";
          if (CI->arg_size() == 1)
            if (auto *K = dyn_cast<ConstantInt>(CI->getArgOperand(0)))
              Call += formatv("0x{0:x-16}ULL", K->getZExtValue()).str();
          Call += ");";
          noteVa(I);
          emit(Call);
        } else if (auto *BI = dyn_cast<BranchInst>(&I)) {
          if (BI->isUnconditional()) {
            noteVa(I);
            emit("  goto " + cIdent(BI->getSuccessor(0)->getName()) + ";");
          } else {
            std::string Pred = "cond";
            if (auto *CI = dyn_cast<CallInst>(BI->getCondition())) {
              Pred = calleeIdent(CI->getCalledFunction());
              Pred += "(";
              if (CI->arg_size() == 1)
                if (auto *K = dyn_cast<ConstantInt>(CI->getArgOperand(0)))
                  Pred += formatv("0x{0:x-16}ULL", K->getZExtValue()).str();
              Pred += ")";
            }
            noteVa(I);
            emit("  if (" + Pred + ") goto " +
                 cIdent(BI->getSuccessor(0)->getName()) + "; else goto " +
                 cIdent(BI->getSuccessor(1)->getName()) + ";");
          }
        } else if (auto *SI = dyn_cast<SwitchInst>(&I)) {
          std::string Sel = "0";
          if (auto *CI = dyn_cast<CallInst>(SI->getCondition()))
            Sel = calleeIdent(CI->getCalledFunction()) + "()";
          noteVa(I);
          emit("  switch (" + Sel + ") {");
          for (auto &C : SI->cases())
            emit("    case " +
                 std::to_string(C.getCaseValue()->getZExtValue()) +
                 ": goto " + cIdent(C.getCaseSuccessor()->getName()) +
                 ";");
          emit("    default: goto " +
               cIdent(SI->getDefaultDest()->getName()) + ";");
          emit("  }");
        } else if (isa<ReturnInst>(&I)) {
          noteVa(I);
          emit("  return;");
        } else if (isa<UnreachableInst>(&I)) {
          noteVa(I);
          emit("  revenant_asm_unreachable();");
        } else {
          std::string S;
          raw_string_ostream IS(S);
          I.print(IS);
          emit("  /* " + S + " */");
        }
      }
    }
    emit("}");
    emit("");
  }
  R.Text = Out;
  return R;
}

/// Rust-shaped skeleton: extern "C" stubs and one unsafe fn per function.
EmitResult emitRustFallback(Module &M) {
  EmitResult R;
  std::string Out;
  raw_string_ostream OS(Out);
  uint32_t Line = 1;
  auto emit = [&](const Twine &S) {
    OS << S << '\n';
    ++Line;
  };
  emit("#![allow(non_snake_case, non_camel_case_types, dead_code)]");
  emit("");
  emit("extern \"C\" {");
  std::set<std::string> Seen;
  for (llvm::Function &F : M) {
    if (!F.isDeclaration())
      continue;
    if (Seen.insert(calleeIdent(&F)).second)
      emit("    fn " + calleeIdent(&F) + "();");
  }
  emit("}");
  emit("");
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    emit("pub unsafe extern \"C\" fn " + cIdent(F.getName()) + "() {");
    for (llvm::BasicBlock &BB : F)
      for (llvm::Instruction &I : BB)
        if (auto *CI = dyn_cast<CallInst>(&I))
          if (llvm::Function *Callee = CI->getCalledFunction()) {
            if (auto VA = ir::getInstructionVA(I)) {
              R.LineVas.push_back({Line, {Va(*VA)}});
              emit(formatv("    {0}(); // 0x{1:x-16}",
                           calleeIdent(Callee), *VA));
            } else {
              emit("    " + calleeIdent(Callee) + "();");
            }
          }
    emit("}");
    emit("");
  }
  R.Text = Out;
  return R;
}

#if defined(REVENANT_HAVE_CLANG_FRONTEND)
/// Contract IR -> C/C++ through the real Clang frontend (LunaClangCore +
/// stmt/expr/type builders). No line map yet: the frontend does not
/// attribute output lines to VAs.
Expected<EmitResult> emitClangSource(Module &M, bool Cpp) {
  clangx::ClangFrontendOptions Opts;
  Opts.Lang = Cpp ? "cpp" : "c";
  auto TOrErr = clangx::createASTContext(M, Opts);
  if (!TOrErr)
    return TOrErr.takeError();
  std::unique_ptr<clangx::TranslationUnit> T = std::move(*TOrErr);
  auto Types = clangx::createTypeBuilder(T->frontend());
  auto Stmts = clangx::createStmtBuilder(T->frontend(), *Types);
  auto Exprs = clangx::createExprBuilder(T->frontend(), *Types);
  clangx::fillTranslationUnit(*T, M, *Types, *Stmts, *Exprs);
  EmitResult R;
  raw_string_ostream OS(R.Text);
  SmallVector<StringRef, 4> Inc = {"stdint.h", "stddef.h", "stdbool.h"};
  clangx::printTranslationUnit(T->tu(), OS, Inc);
  return R;
}
#endif

/// ir-final .ll -> Rust through the rustc fork driver flag
/// (`-Z revenant-decompile=<file.ll>`), when the stage-1 rustc exists.
/// REVENANT_RUSTC overrides the binary path.
Expected<EmitResult> emitRustcSource(const Module &M, StringRef LlText) {
  const char *Env = getenv("REVENANT_RUSTC");
  std::string Rustc =
      Env && *Env ? Env
                  : "C:/Users/User/RustroverProjects/Revenant/rust/build/"
                    "x86_64-pc-windows-msvc/stage1/bin/rustc.exe";
  std::error_code EC;
  if (!sys::fs::exists(Rustc))
    return createStringError(
        "rustc fork not built (set REVENANT_RUSTC to a stage-1 rustc)");
  SmallString<64> LlPath;
  EC = sys::fs::createTemporaryFile("revenant", "ll", LlPath);
  if (EC)
    return createStringError(EC, "create temp .ll");
  if (Error E = writeText(LlPath, LlText))
    return E;
  SmallString<64> OutPath;
  EC = sys::fs::createTemporaryFile("revenant", "rs", OutPath);
  if (EC)
    return createStringError(EC, "create temp .rs");
  // The flag's exact emit spelling is settled when LunaRustcCore lands;
  // run with just the -Z flag and read the source back from the temp.
  std::string Flag = ("revenant-decompile=" + LlPath).str();
  std::string Stderr;
  SmallVector<StringRef, 8> Argv = {Rustc, "-Z", Flag};
  int Rc = sys::ExecuteAndWait(
      Rustc, Argv, /*Env=*/std::nullopt, /*Redirects=*/{},
      /*SecondsToWait=*/120, /*MemoryLimit=*/0, &Stderr);
  std::string Src;
  if (auto Buf = MemoryBuffer::getFile(OutPath); Buf && *Buf &&
      !(*Buf)->getBuffer().empty())
    Src = (*Buf)->getBuffer().str();
  sys::fs::remove(LlPath);
  sys::fs::remove(OutPath);
  if (Rc != 0 && Src.empty())
    return createStringError("rustc fork failed: " + Stderr);
  EmitResult Er;
  Er.Text = std::move(Src);
  return Er;
}


} // namespace

// Pipeline texts, engine order from docs/transforms.md.
static const char CleanPipeline[] =
    "function(sroa,mem2reg,early-cse,"
    "instcombine<no-verify-fixpoint>,"
    "simplifycfg<no-switch-to-lookup;no-hoist-common-insts;"
    "no-sink-common-insts;no-speculate-blocks>,"
    "gvn,dse,dce,sroa,instcombine<no-verify-fixpoint>,"
    "revenant-idioms)";

static std::string finalPipeline(bool Patterns) {
  return std::string(
             "function(revenant-lane0,revenant-devectorize,revenant-reroll,"
             "revenant-unrotate,revenant-untailrec") +
         (Patterns ? ",revenant-patterns" : "") +
         ",revenant-structure,revenant-types,revenant-ssaout)";
}

Expected<Decompiled> Session::decompile(Va Start, Lang Out,
                                        const DecompileOpts &Opts) {
  return decompileAll({Start}, Out, Opts);
}

Expected<Decompiled> Session::decompileAll(ArrayRef<Va> Targets, Lang Out,
                                           const DecompileOpts &Opts) {
  Decompiled R;
  R.Stages = SetupStages;

  std::vector<Va> Todo(Targets.begin(), Targets.end());
  if (Todo.empty())
    for (const auto &KV : Prog.Functions)
      Todo.push_back(KV.first);

  // ---- asm -----------------------------------------------------------
  std::string AsmText;
  std::vector<revenant::Function> Funcs;
  for (Va T : Todo) {
    auto FOrErr = materialize(T);
    if (!FOrErr)
      return FOrErr.takeError();
    Funcs.push_back(std::move(*FOrErr));
    auto A = disasmFunction(T);
    if (!A)
      return A.takeError();
    AsmText += *A;
  }
  size_t InstrCount = std::accumulate(
      Funcs.begin(), Funcs.end(), size_t(0),
      [](size_t N, const revenant::Function &F) { return N + F.Instrs.size(); });
  record(R.Stages, "asm", "ok",
         formatv("{0} instructions across {1} functions", InstrCount,
                 Funcs.size()).str());
  if (Opts.Emit == EmitKind::Asm) {
    R.Text = AsmText;
    uint32_t L = 1;
    for (const revenant::Function &F : Funcs)
      for (const revenant::Instr &I : F.Instrs)
        R.LineVas.push_back({L++, {I.Addr}});
  }

  // ---- signatures ------------------------------------------------------
  record(R.Stages, "sigs", "stub",
         "signature recovery not landed; void() signatures");

  // ---- lift -> ir-raw ----------------------------------------------------
  LLVMContext Ctx;
  auto M = moduleFor(Ctx, *this, Out, Todo);
  {
    bool LiftStub = true;
    std::string LiftNote;
    for (size_t I = 0; I < Funcs.size(); ++I) {
      const FuncRecord &Rec = Prog.Functions.at(Todo[I]);
      auto N = liftOne(*M, *this, *Dis, Rec, Funcs[I]);
      if (!N)
        return N.takeError();
      LiftNote = *N;
    }
#if defined(RV_HAVE_LIFT)
    LiftStub = false;
#endif
    record(R.Stages, "lift", LiftStub ? "stub" : "ok", LiftNote);
  }
  std::string RawText = moduleText(*M);
  if (Opts.Emit == EmitKind::IrRaw) {
    R.Text = RawText;
    R.LineVas.clear();
  }

  // ---- ir-clean ----------------------------------------------------------
  {
    std::vector<std::string> Stripped;
    if (auto E = runPipelineText(*M, CleanPipeline, Stripped))
      return std::move(E);
    std::string Note =
        Stripped.empty()
            ? "cleanup + idioms"
            : formatv("cleanup; skipped not-landed passes: {0}",
                      llvm::join(Stripped, ", "))
                  .str();
    record(R.Stages, "ir-clean", Stripped.empty() ? "ok" : "stub", Note);
  }
  std::string CleanText = moduleText(*M);
  if (Opts.Emit == EmitKind::IrClean) {
    R.Text = CleanText;
    R.LineVas.clear();
  }

  // ---- ir-final ----------------------------------------------------------
  {
    std::vector<std::string> Stripped;
    if (auto E = runPipelineText(*M, finalPipeline(Opts.Patterns), Stripped))
      return std::move(E);
    std::string Note =
        Stripped.empty()
            ? "unopt + structure + types + ssaout"
            : formatv("ran landed passes; stubbed: {0}",
                      llvm::join(Stripped, ", "))
                  .str();
    record(R.Stages, "ir-final", Stripped.empty() ? "ok" : "stub", Note);
  }
  std::string FinalText = moduleText(*M);
  if (Opts.Emit == EmitKind::IrFinal) {
    R.Text = FinalText;
    R.LineVas.clear();
  }

  // ---- source ------------------------------------------------------------
  if (Opts.Emit == EmitKind::Source) {
    EmitResult Em;
    switch (Out) {
    case Lang::C:
    case Lang::Cpp:
#if defined(REVENANT_HAVE_CLANG_FRONTEND)
      if (auto Real = emitClangSource(*M, Out == Lang::Cpp)) {
        Em = std::move(*Real);
        record(R.Stages, "source", "ok",
               Out == Lang::Cpp ? "Clang frontend (C++)"
                                : "Clang frontend (C)");
      } else {
        Em = emitCFallback(*M);
        record(R.Stages, "source", "stub",
               "Clang frontend failed, C skeleton emitted: " +
                   toString(Real.takeError()));
      }
#else
      Em = emitCFallback(*M);
      record(R.Stages, "source", "stub",
             Out == Lang::Cpp
                 ? "C++ skeleton emitter (Clang frontend not linked yet)"
                 : "C skeleton emitter (Clang frontend not linked yet)");
#endif
      break;
    case Lang::Rust:
      if (auto Real = emitRustcSource(*M, FinalText)) {
        Em = std::move(*Real);
        if (!Em.Text.empty()) {
          record(R.Stages, "source", "ok", "rustc fork");
          break;
        }
      }
      Em = emitRustFallback(*M);
      record(R.Stages, "source", "stub",
             "Rust skeleton emitter (rustc fork not wired yet)");
      break;
    }
    R.Text = std::move(Em.Text);
    R.LineVas = std::move(Em.LineVas);
  }

  // ---- artifacts -----------------------------------------------------------
  if (!Opts.ArtifactBase.empty()) {
    StringRef SrcExt = Out == Lang::Cpp    ? "cpp"
                       : Out == Lang::Rust ? "rs"
                                           : "c";
    struct Art {
      StringRef Stage;
      StringRef Suffix;
      StringRef Ext;
      StringRef Text;
    };
    SmallVector<Art, 5> Arts = {
        {"asm", "asm", "txt", AsmText},
        {"lift", "ir-raw", "ll", RawText},
        {"ir-clean", "ir-clean", "ll", CleanText},
        {"ir-final", "ir-final", "ll", FinalText},
        {"source", "source", SrcExt,
         Opts.Emit == EmitKind::Source ? StringRef(R.Text) : StringRef()},
    };
    for (const Art &A : Arts) {
      if (A.Text.empty())
        continue;
      std::string Path =
          (Opts.ArtifactBase + "." + A.Suffix + "." + A.Ext).str();
      if (auto E = writeText(Path, A.Text))
        return std::move(E);
      for (StageReport &St : R.Stages)
        if (St.Name == A.Stage)
          St.Artifact = Path;
    }
    std::string Man;
    raw_string_ostream MOS(Man);
    for (const StageReport &St : R.Stages)
      MOS << St.Name << '\t' << St.Status << '\t' << St.Note
          << (St.Artifact.empty() ? "" : "\t" + St.Artifact) << '\n';
    if (auto E = writeText(Opts.ArtifactBase + ".manifest.txt", Man))
      return std::move(E);
  }
  return R;
}

} // namespace revenant
