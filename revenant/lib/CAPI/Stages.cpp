//==- lib/CAPI/Stages.cpp - the decompile stage pipeline ------------------==//
//
// Per run: asm -> sigs -> lift (ir-raw) -> cleanup + idioms (ir-clean) ->
// unopt + structure + types + ssaout (ir-final) -> source. Every stage is the
// real implementation; a stage that cannot run fails the whole run. Each
// stage reports its status and elapsed time into StageReport, and writes its
// artifact next to the output when `DecompileOpts::ArtifactBase` is set.
//
//  - sigs:   revenant::analyzeSignatures (RevenantAnalysis), converted to the
//            lifter's mirror type.
//  - lift:   revenant::lift::liftFunction (RevenantLift).
//  - passes: revenant::registerRevenantPasses (RevenantTransformPipeline).
//  - source: C/C++ through RevenantClangCore + Core's printTranslationUnit;
//            Rust through the rustc fork's `-Z revenant-decompile`.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Session.h"

#include "revenant/Analysis/EHTables.h"
#include "revenant/Analysis/Signatures.h"
#include "revenant/Clang/ClangFrontend.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"
#include "revenant/Lift/LiftFunction.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Transforms/Pipeline/Pipeline.h"

#include <chrono>
#include <numeric>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "X86BaseInfo.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace revenant {
namespace {

// Store references only; format diagnostics when a crash actually occurs.
class FunctionStageTrace final : public PrettyStackTraceEntry {
  StringRef Stage;
  StringRef Pass;
  StringRef Name;
  uint64_t Address;

public:
  FunctionStageTrace(StringRef Stage, StringRef Pass, StringRef Name,
                     uint64_t Address = 0)
      : Stage(Stage), Pass(Pass), Name(Name), Address(Address) {}

  void print(raw_ostream &OS) const override {
    OS << "Revenant stage=" << Stage << " pass=" << Pass
       << " function=" << Name;
    if (Address)
      OS << " va=" << format_hex(Address, 0);
    OS << "\n";
  }
};

//==--------------------------------------------------------------------==//
// Small helpers
//==--------------------------------------------------------------------==//

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

using StageClock = std::chrono::steady_clock;
thread_local StageClock::time_point LastStageTime;
thread_local bool StageClockStarted = false;

void resetStageClock() {
  LastStageTime = StageClock::now();
  StageClockStarted = true;
}

/// Append a finished stage; its elapsed time runs from the previous record.
void record(std::vector<StageReport> &Stages, StringRef Name, StringRef Status,
            StringRef Note) {
  StageClock::time_point Now = StageClock::now();
  StageReport R{Name.str(), Status.str(), Note.str(), ""};
  if (StageClockStarted)
    R.ElapsedMicroseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(Now -
                                                              LastStageTime)
            .count();
  LastStageTime = Now;
  StageClockStarted = true;
  Stages.push_back(std::move(R));
}

//==--------------------------------------------------------------------==//
// Module skeleton and signatures
//==--------------------------------------------------------------------==//

/// Triple, datalayout and language flag. The lifter declares imports and
/// callees itself, typed from the recovered signatures.
std::unique_ptr<Module> moduleFor(LLVMContext &Ctx, Lang Out) {
  auto M = std::make_unique<Module>("revenant", Ctx);
  M->setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  M->setDataLayout(
      "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:"
      "32:64-S128");
  ir::setModuleLang(*M, Out == Lang::Cpp    ? ir::Lang::CPP
                        : Out == Lang::Rust ? ir::Lang::Rust
                                            : ir::Lang::C);
  return M;
}

/// analysis::Signature -> the lifter's mirror type.
lift::Signature convertSig(const revenant::Signature &S) {
  lift::Signature Out;
  for (const revenant::Param &P : S.Params) {
    lift::Param LP;
    LP.Loc = P.Loc.isStack() ? lift::ParamLoc::stack(P.Loc.Off)
                             : lift::ParamLoc::reg(llvm::MCRegister(P.Loc.Reg));
    LP.Bits = P.Bits;
    LP.Float = P.Float;
    Out.Params.push_back(std::move(LP));
  }
  if (S.HasRet)
    Out.Ret = lift::RetLoc{S.Ret.Float, S.Ret.Bits};
  Out.NoReturn = S.Noreturn;
  switch (S.SigSource) {
  case revenant::Signature::Source::Pdb:
    Out.Source = lift::SigSource::Pdb;
    break;
  case revenant::Signature::Source::Inferred:
    Out.Source = lift::SigSource::Inferred;
    break;
  case revenant::Signature::Source::Default:
    Out.Source = lift::SigSource::Default;
    break;
  }
  return Out;
}

std::shared_ptr<const lift::Signatures> recoverSignatures(const Program &Prog,
                                                          const Disasm &Dis) {
  revenant::Signatures A = revenant::analyzeSignatures(Prog, Dis);
  auto Out = std::make_shared<lift::Signatures>();
  for (const auto &[At, S] : A.Funcs)
    Out->Funcs.emplace(At, convertSig(S));
  for (const auto &[At, S] : A.Externs)
    Out->Externs.emplace(At, convertSig(S));
  for (const auto &[At, N] : A.IndirectArgs)
    Out->IndirectArgs.emplace(At, N);
  for (const auto &[At, N] : A.CallsiteArgs)
    Out->CallsiteArgs.emplace(At, N);
  return Out;
}

//==--------------------------------------------------------------------==//
// Pass pipeline
//==--------------------------------------------------------------------==//

/// Fail with the verifier's message when `Stage` left M malformed: a pass bug
/// then names its stage here instead of crashing a frontend later.
Error verifyStage(StringRef Stage, const Module &M) {
  std::string Msg;
  raw_string_ostream OS(Msg);
  if (verifyModule(M, &OS))
    return createStringError("IR verifier failed after " + Stage.str() + ": " +
                             Msg);
  return Error::success();
}

SmallVector<StringRef, 16> pipelineElements(StringRef Pipeline);

/// Parse and run `Pipeline` over M, then verify it as `Stage`. A pass name
/// this binary does not know is an error, not a skipped stage.
Error runFunctionPipeline(StringRef Stage, Module &M, StringRef Pipeline,
                          std::vector<StageReport> &Reports) {
  PassBuilder PB;
  registerRevenantPasses(PB);
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  // Verify each function pass independently and retain its last valid body.
  if (!Pipeline.consume_front("function(") || !Pipeline.consume_back(")"))
    return createStringError("expected a function pipeline");
  auto Passes = pipelineElements(Pipeline);
  for (StringRef Pass : Passes) {
    FunctionPassManager FPM;
    if (Error E = PB.parsePassPipeline(FPM, Pass))
      return createStringError("pipeline '" + Pass.str() +
                               "': " + toString(std::move(E)));
    SmallVector<llvm::Function *, 32> Functions;
    for (llvm::Function &F : M)
      if (!F.isDeclaration())
        Functions.push_back(&F);
    for (llvm::Function *F : Functions) {
      FunctionStageTrace Trace(Stage, Pass, F->getName());
      ValueToValueMapTy SavedMap;
      llvm::Function *Saved = CloneFunction(F, SavedMap);
      FPM.run(*F, FAM);
      std::string Message;
      raw_string_ostream OS(Message);
      if (verifyFunction(*F, &OS)) {
        std::string Name = F->getName().str();
        FAM.clear(*F, Name);
        F->deleteBody();
        F->copyAttributesFrom(Saved);
        ValueToValueMapTy RestoreMap;
        RestoreMap[Saved] = F;
        auto Dest = F->arg_begin();
        for (Argument &A : Saved->args())
          RestoreMap[&A] = &*Dest++;
        SmallVector<ReturnInst *, 8> Returns;
        CloneFunctionInto(F, Saved, RestoreMap,
                          CloneFunctionChangeType::LocalChangesOnly, Returns);
        record(Reports, Stage, "failed",
               Name + ": " + Pass.str() + ": " + Message);
        errs() << "[failed] " << Stage << ": " << Name << ": " << Pass
               << ": " << Message;
      }
      Saved->eraseFromParent();
    }
  }
  return verifyStage(Stage, M);
}

// Split only between pipeline elements, never inside adaptors or options.
SmallVector<StringRef, 16> pipelineElements(StringRef Pipeline) {
  SmallVector<StringRef, 16> Elements;
  unsigned Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I != Pipeline.size(); ++I) {
    char C = Pipeline[I];
    if (C == '(' || C == '<')
      ++Depth;
    else if ((C == ')' || C == '>') && Depth)
      --Depth;
    else if (C == ',' && !Depth) {
      Elements.push_back(Pipeline.slice(Start, I));
      Start = I + 1;
    }
  }
  Elements.push_back(Pipeline.drop_front(Start));
  return Elements;
}

Error runModulePass(Module &M, StringRef Pass) {
  PassBuilder PB;
  registerRevenantPasses(PB);
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerLoopAnalyses(LAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM;
  if (Error E = PB.parsePassPipeline(MPM, Pass))
    return E;
  MPM.run(M, MAM);
  return verifyStage(Pass, M);
}

Error runPipelineText(StringRef Stage, std::unique_ptr<Module> &M,
                      StringRef Pipeline, std::vector<StageReport> &Reports) {
  // Validate the whole pipeline first: unknown pass names are configuration
  // errors, not recoverable failures of a particular function or module.
  PassBuilder PB;
  registerRevenantPasses(PB);
  ModulePassManager Parsed;
  if (Error E = PB.parsePassPipeline(Parsed, Pipeline))
    return E;
  for (StringRef Element : pipelineElements(Pipeline)) {
    if (Element.starts_with("function(")) {
      if (Error E = runFunctionPipeline(Stage, *M, Element, Reports))
        return E;
      continue;
    }
    FunctionStageTrace Trace(Stage, Element, "module");
    auto Saved = CloneModule(*M);
    // Analysis managers die inside runModulePass before the module is swapped.
    if (Error E = runModulePass(*M, Element)) {
      std::string Message = toString(std::move(E));
      M.swap(Saved);
      record(Reports, Stage, "failed",
             "module: " + Element.str() + ": " + Message);
      errs() << "[failed] " << Stage << ": module: " << Element << ": "
             << Message << "\n";
    }
  }
  return verifyStage(Stage, *M);
}

/// ir-clean: normalize lifted stack/address/width shapes, then cleanup + idioms.
std::string cleanPipeline() {
  return formatv("function({0},{1})", CleanupPipeline, IdiomsPipeline).str();
}

/// ir-final: deoptimization, structuring, type recovery, SSA-out.
std::string finalPipeline(bool Patterns) {
  return formatv("function({0}{1},revenant-structure,revenant-types,"
                 "revenant-ssaout)",
                 UnoptPipeline, Patterns ? ",revenant-patterns" : "")
      .str();
}

//==--------------------------------------------------------------------==//
// Source emit
//==--------------------------------------------------------------------==//

struct EmitResult {
  std::string Text;
  std::vector<std::pair<uint32_t, std::vector<Va>>> LineVas;
};

/// Contract IR -> C/C++ through the Clang frontend: Core builds the
/// TranslationUnit and prints it with `printTranslationUnit`. The frontend
/// does not attribute output lines to VAs, so `LineVas` stays empty.
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

/// ir-final .ll -> Rust through the rustc fork driver flag
/// (`-Z revenant-decompile=<file.ll>`). REVENANT_RUSTC overrides the binary
/// path.
Expected<EmitResult> emitRustcSource(StringRef LlText) {
  const char *Env = getenv("REVENANT_RUSTC");
  std::string Rustc =
      Env && *Env ? Env
                  : "C:/Users/User/RustroverProjects/Revenant/rust/build/"
                    "x86_64-pc-windows-msvc/stage1/bin/rustc.exe";
  if (!sys::fs::exists(Rustc))
    return createStringError(
        "rustc fork not built (set REVENANT_RUSTC to a stage-1 rustc)");
  SmallString<64> LlPath;
  if (auto EC = sys::fs::createTemporaryFile("revenant", "ll", LlPath))
    return createStringError(EC, "create temp .ll");
  if (Error E = writeText(LlPath, LlText))
    return E;
  SmallString<64> OutPath, ErrPath;
  if (auto EC = sys::fs::createTemporaryFile("revenant", "rs", OutPath))
    return createStringError(EC, "create temp .rs");
  if (auto EC = sys::fs::createTemporaryFile("revenant", "err", ErrPath))
    return createStringError(EC, "create temp .err");
  // The fork prints the generated crate on stdout and never reads its input
  // file, but rustc insists on one: `-` names stdin, which is the null device.
  std::string Flag = ("-Zrevenant-decompile=" + LlPath).str();
  SmallVector<StringRef, 4> Argv = {Rustc, Flag, "-"};
  std::optional<StringRef> Redirects[] = {StringRef(""), StringRef(OutPath),
                                          StringRef(ErrPath)};
  int Rc = sys::ExecuteAndWait(Rustc, Argv, /*Env=*/std::nullopt, Redirects,
                               /*SecondsToWait=*/120);
  std::string Src, Stderr;
  if (auto Buf = MemoryBuffer::getFile(OutPath); Buf && *Buf)
    Src = (*Buf)->getBuffer().str();
  if (auto Buf = MemoryBuffer::getFile(ErrPath); Buf && *Buf)
    Stderr = (*Buf)->getBuffer().str();
  sys::fs::remove(LlPath);
  sys::fs::remove(OutPath);
  sys::fs::remove(ErrPath);
  if (Rc != 0 || Src.empty())
    return createStringError("rustc fork failed (exit " + Twine(Rc) +
                             "): " + Stderr);
  EmitResult Er;
  Er.Text = std::move(Src);
  return Er;
}

} // namespace

Expected<Decompiled> Session::decompile(Va Start, Lang Out,
                                        const DecompileOpts &Opts) {
  return decompileAll({Start}, Out, Opts);
}

Expected<Decompiled> Session::decompileAll(ArrayRef<Va> Targets, Lang Out,
                                           const DecompileOpts &Opts) {
  Decompiled R;
  R.Stages = SetupStages;
  resetStageClock();
  std::vector<Va> Todo(Targets.begin(), Targets.end());
  if (Todo.empty())
    for (const auto &KV : Prog.Functions)
      Todo.push_back(KV.first);

  // ---- asm -----------------------------------------------------------
  std::string AsmText;
  size_t InstrCount = 0;
  std::vector<Va> Ready;
  for (Va T : Todo) {
    auto FOrErr = materialize(T);
    if (!FOrErr) {
      std::string Note =
          formatv("0x{0:x}: {1}", T.Value, toString(FOrErr.takeError())).str();
      record(R.Stages, "asm", "failed", Note);
      errs() << "[failed] asm: " << Note << "\n";
      continue;
    }
    auto A = disasmFunction(T);
    if (!A) {
      std::string Note =
          formatv("0x{0:x}: {1}", T.Value, toString(A.takeError())).str();
      record(R.Stages, "asm", "failed", Note);
      errs() << "[failed] asm: " << Note << "\n";
      continue;
    }
    Ready.push_back(T);
    AsmText += *A;
    InstrCount += FOrErr->Instrs.size();
    if (Opts.Emit == EmitKind::Asm)
      for (const revenant::Instr &I : FOrErr->Instrs)
        R.LineVas.push_back(
            {static_cast<uint32_t>(R.LineVas.size() + 1), {I.Addr}});
  }
  Todo = std::move(Ready);
  record(
      R.Stages, "asm", "ok",
      formatv("{0} instructions across {1} functions", InstrCount, Todo.size())
          .str());
  if (Opts.Emit == EmitKind::Asm)
    R.Text = AsmText;

  // Each stage below runs only when a later artifact was asked for: `break`
  // leaves the block once the requested emit kind is produced.
  std::string RawText, CleanText, FinalText;
  do {
    if (Opts.Emit == EmitKind::Asm)
      break;
    // ---- signatures ------------------------------------------------------
    // Interprocedural over the whole program, so it runs once per session.
    if (!Sigs)
      Sigs = recoverSignatures(Prog, *Dis);
    record(R.Stages, "sigs", "ok",
           formatv("{0} functions, {1} imports", Sigs->Funcs.size(),
                   Sigs->Externs.size())
               .str());

    // ---- lift -> ir-raw ----------------------------------------------------
    LLVMContext Ctx;
    auto M = moduleFor(Ctx, Out);
    // Decode the image's EH metadata once, then associate each lifted function
    // with its table entry. The EH vector owns these entries through the loop.
    std::vector<FunctionEH> EHFunctions = decodeEHTables(pe());
    DenseSet<uint64_t> FuncletAddresses;
    DenseMap<uint64_t, uint64_t> FuncletAliases;
    // EH map targets can point at the entry instruction inside a discovered
    // funclet, rather than at its containing Program function's start.
    auto collectFunclet = [&](Va Handler, Va Owner) {
      auto Exact = Prog.Functions.find(Handler);
      if (Exact != Prog.Functions.end() && Handler != Owner) {
        FuncletAddresses.insert(Handler.Value);
        FuncletAliases[Handler.Value] = Handler.Value;
        return;
      }
      for (const auto &[Start, F] : Prog.Functions) {
        if (Start == Owner)
          continue;
        bool Contains = false;
        for (const auto &[Lo, Hi] : F.Chunks)
          Contains |= Lo <= Handler && Handler < Hi;
        if (F.Chunks.empty())
          Contains = Start <= Handler && Handler < F.End;
        if (!Contains)
          continue;
        FuncletAddresses.insert(Start.Value);
        FuncletAliases[Handler.Value] = Start.Value;
        return;
      }
    };
    for (const FunctionEH &EH : EHFunctions) {
      if (!llvm::is_contained(Todo, EH.Function))
        continue;
      for (const EHHandler &H : EH.Handlers)
        collectFunclet(H.Address, EH.Function);
      for (const EHCleanup &C : EH.Cleanups)
        collectFunclet(C.Address, EH.Function);
    }
    DenseMap<uint64_t, llvm::Function *> Funclets;
    for (uint64_t Address : FuncletAddresses) {
      Va At(Address);
      auto SignatureMap = *Sigs;
      lift::Signature FuncletSig;
      FuncletSig.Params.push_back(
          {lift::ParamLoc::reg(llvm::X86::RDX), 64, false, std::string("frame")});
      FuncletSig.Ret = lift::RetLoc{false, 64};
      SignatureMap.Funcs[At] = std::move(FuncletSig);
      auto L = lift::liftFunction(*M, Prog, Prog.Functions.at(At), *Dis,
                                  &SignatureMap, /*TM=*/nullptr,
                                  /*RunPasses=*/false, /*EH=*/nullptr);
      if (!L) {
        std::string Message = toString(L.takeError());
        return createStringError(inconvertibleErrorCode(),
                                 "failed to lift EH funclet 0x%llx: %s",
                                 static_cast<unsigned long long>(Address),
                                 Message.c_str());
      }
      Funclets[Address] = L->Fn;
    }
    size_t OpaqueSites = 0;
    for (Va T : Todo) {
      if (FuncletAddresses.contains(T.Value))
        continue;
      FunctionStageTrace Trace("lift", "liftFunction", Prog.Functions.at(T).Name,
                               T.Value);
      const FunctionEH *EH = nullptr;
      auto EHIt = llvm::find_if(EHFunctions, [T](const FunctionEH &Entry) {
        return Entry.Function == T;
      });
      if (EHIt != EHFunctions.end()) {
        if (Compiler.Producer == revenant::Compiler::Rustc)
          EHIt->Model = EHModel::Rust;
        EH = &*EHIt;
      }
      auto L = lift::liftFunction(*M, Prog, Prog.Functions.at(T), *Dis,
                                  Sigs.get(), /*TM=*/nullptr,
                                  /*RunPasses=*/false, EH);
      if (!L) {
        std::string Message = toString(L.takeError());
        std::string Note = formatv("0x{0:x}: {1}", T.Value, Message).str();
        record(R.Stages, "lift", "failed", Note);
        errs() << "[failed] lift: " << Note << "\n";
        continue;
      }
      OpaqueSites += L->AsmFallbacks.size();
    }
    // MSVC catch funclets return their continuation VA in rax.
    auto continuationVA = [](const llvm::Function &F)
        -> std::optional<uint64_t> {
      const llvm::AllocaInst *Rax = nullptr;
      for (const llvm::Instruction &I : F.getEntryBlock())
        if (auto *AI = dyn_cast<llvm::AllocaInst>(&I);
            AI && AI->getName() == "rax") {
          Rax = AI;
          break;
        }
      if (!Rax)
        return std::nullopt;
      std::optional<uint64_t> Result;
      for (const llvm::BasicBlock &BB : F)
        for (const llvm::Instruction &I : BB)
          if (auto *Store = dyn_cast<llvm::StoreInst>(&I);
              Store && Store->getPointerOperand()->stripPointerCasts() == Rax)
            if (auto *V = dyn_cast<llvm::ConstantInt>(Store->getValueOperand())) {
              uint64_t VA = V->getZExtValue();
              if (Result && *Result != VA)
                return std::nullopt;
              Result = VA;
            }
      return Result;
    };
    if (!Funclets.empty()) {
    SmallVector<llvm::Function *, 32> Parents;
    for (llvm::Function &F : *M)
      Parents.push_back(&F);
    for (llvm::Function *ParentPtr : Parents) {
      llvm::Function &Parent = *ParentPtr;
      if (Parent.isDeclaration())
        continue;
      auto ParentVA = ir::getFunctionVA(Parent);
      if (ParentVA && FuncletAddresses.contains(*ParentVA))
        continue;
      llvm::AllocaInst *Frame = nullptr;
      for (llvm::Instruction &I : Parent.getEntryBlock())
        if (auto *AI = dyn_cast<llvm::AllocaInst>(&I);
            AI && AI->getName() == "frame") {
          Frame = AI;
          break;
        }
      if (!Frame)
        continue;
      SmallVector<llvm::BasicBlock *, 8> HandlerBlocks;
      for (llvm::BasicBlock &BB : Parent)
        for (llvm::Instruction &I : BB)
          if (isa<llvm::FuncletPadInst>(&I)) {
            HandlerBlocks.push_back(&BB);
            break;
          }
      for (llvm::BasicBlock *BBPtr : HandlerBlocks) {
        llvm::BasicBlock &BB = *BBPtr;
        llvm::FuncletPadInst *Pad = nullptr;
        for (llvm::Instruction &I : BB)
          if (auto *P = dyn_cast<llvm::FuncletPadInst>(&I)) {
            Pad = P;
            break;
          }
        if (!Pad)
          continue;
        llvm::MDNode *Handler = Pad->getMetadata("revenant.handler");
        if (!Handler || Handler->getNumOperands() != 1)
          continue;
        auto *AddressMD =
            dyn_cast<llvm::ConstantAsMetadata>(Handler->getOperand(0));
        auto *Address = AddressMD
                            ? dyn_cast<llvm::ConstantInt>(AddressMD->getValue())
                            : nullptr;
        if (!Address)
          continue;
        auto Alias = FuncletAliases.find(Address->getZExtValue());
        if (Alias == FuncletAliases.end())
          continue;
        llvm::Function *Funclet = Funclets.lookup(Alias->second);
        if (!Funclet || Funclet->arg_size() != 1 || BB.empty())
          continue;
        std::optional<uint64_t> ContinueVA = continuationVA(*Funclet);
        llvm::Instruction *Ret = BB.getTerminator();
        if (!Ret)
          continue;
        llvm::IRBuilder<> Builder(Ret);
        llvm::Value *FrameArg =
            Builder.CreatePtrToInt(Frame, Funclet->getArg(0)->getType());
        SmallVector<llvm::Value *, 1> BundleInputs{Pad};
        SmallVector<OperandBundleDef, 1> Bundles{
            OperandBundleDef("funclet", BundleInputs)};
        SmallVector<llvm::Value *, 1> Args{FrameArg};
        llvm::CallInst *Call = Builder.CreateCall(
            Funclet->getFunctionType(), Funclet, Args, Bundles, "eh.funclet");
        llvm::InlineFunctionInfo IFI;
        if (!llvm::InlineFunction(*Call, IFI).isSuccess())
          return createStringError(inconvertibleErrorCode(),
                                   "failed to inline EH funclet into %s",
                                   Parent.getName().str().c_str());
        if (ContinueVA)
          if (auto *CatchRet =
                  dyn_cast<llvm::CatchReturnInst>(BB.getTerminator())) {
            std::string Name = formatv("bb_{0:x}", *ContinueVA).str();
            for (llvm::BasicBlock &Next : Parent)
              if (Next.getName() == Name) {
                CatchRet->setSuccessor(&Next);
                break;
              }
          }
      }
    }
    }
    for (auto &Entry : Funclets)
      if (Entry.second->use_empty())
        Entry.second->eraseFromParent();
    if (Error E = verifyStage("lift", *M))
      return E;
    record(
        R.Stages, "lift", "ok",
        formatv("revenant::lift::liftFunction, {0} opaque sites", OpaqueSites)
            .str());
    RawText = moduleText(*M);
    if (Opts.Emit == EmitKind::IrRaw) {
      R.Text = RawText;
      break;
    }

    // ---- ir-clean ----------------------------------------------------------
    if (auto E = runPipelineText("ir-clean", M, cleanPipeline(), R.Stages))
      return std::move(E);
    record(R.Stages, "ir-clean", "ok", "cleanup + idioms");
    CleanText = moduleText(*M);
    if (Opts.Emit == EmitKind::IrClean) {
      R.Text = CleanText;
      break;
    }

    // ---- ir-final ----------------------------------------------------------
    if (auto E =
            runPipelineText("ir-final", M, finalPipeline(Opts.Patterns),
                            R.Stages))
      return std::move(E);
    record(R.Stages, "ir-final", "ok", "unopt + structure + types + ssaout");
    FinalText = moduleText(*M);
    if (Opts.Emit == EmitKind::IrFinal) {
      R.Text = FinalText;
      break;
    }

    // ---- source ------------------------------------------------------------
    if (Opts.Emit == EmitKind::Source) {
      FunctionStageTrace Trace("source", Out == Lang::Rust ? "rustc" : "clang",
                               "module");
      Expected<EmitResult> Em = Out == Lang::Rust
                                    ? emitRustcSource(FinalText)
                                    : emitClangSource(*M, Out == Lang::Cpp);
      if (!Em)
        return Em.takeError();
      record(R.Stages, "source", "ok",
             Out == Lang::Rust  ? "rustc fork"
             : Out == Lang::Cpp ? "Clang frontend (C++)"
                                : "Clang frontend (C)");
      R.Text = std::move(Em->Text);
      R.LineVas = std::move(Em->LineVas);
    }
  } while (false);

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
