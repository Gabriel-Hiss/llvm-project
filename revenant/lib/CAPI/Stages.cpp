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

#include "revenant/Analysis/LibMatch.h"
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
#include "revenant/Transforms/Uncxx/Uncxx.h"

#include <chrono>
#include <numeric>
#include <thread>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
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
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace revenant {
namespace {
// LLVM keeps a symbol table's name-uniquing counter (`LastUnique`) private.
// Later passes create names like `bb_X558` from that counter, so the
// parallel clean path must carry it across the bitcode round trip, where a
// parsed function starts back at 0. Explicit instantiation may name private
// members; this is the standard way to reach one without patching LLVM.
template <typename Tag, typename Tag::type Member> struct PrivateMember {
  friend typename Tag::type privateMember(Tag) { return Member; }
};
struct LastUniqueTag {
  using type = uint32_t ValueSymbolTable::*;
  friend type privateMember(LastUniqueTag);
};
template struct PrivateMember<LastUniqueTag, &ValueSymbolTable::LastUnique>;
uint32_t &lastUnique(ValueSymbolTable &VST) {
  return VST.*privateMember(LastUniqueTag{});
}

CxxFlavor uncxxFlavor(Compiler Producer) {
  switch (Producer) {
  case Compiler::Msvc:
  case Compiler::ClangCl:
    return CxxFlavor::Msvc;
  case Compiler::MinGwGcc:
    return CxxFlavor::Gnu;
  default:
    return CxxFlavor::Auto;
  }
}

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
    if (!P.Name.empty())
      LP.Name = P.Name;
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
  case revenant::Signature::Source::Library:
    Out.Source = lift::SigSource::Library;
    break;
  case revenant::Signature::Source::Default:
    Out.Source = lift::SigSource::Default;
    break;
  }
  return Out;
}

Expected<std::shared_ptr<const lift::Signatures>>
recoverSignatures(Program &Prog, const Disasm &Dis, StringRef LibraryDBPath) {
  revenant::Signatures A = revenant::analyzeSignatures(Prog, Dis);
  if (!LibraryDBPath.empty()) {
    auto DB = LibraryDatabase::load(LibraryDBPath);
    if (!DB)
      return DB.takeError();
    std::vector<LibraryMatch> Matches = matchLibraryFunctions(Prog, *DB);
    extendLibraryMatchesByCalleeEvidence(Prog, *DB, Matches);
    applyLibraryMatches(Prog, A, Matches, *DB);
  }
  auto Out = std::make_shared<lift::Signatures>();
  for (const auto &[At, S] : A.Funcs)
    Out->Funcs.emplace(At, convertSig(S));
  for (const auto &[At, S] : A.Externs)
    Out->Externs.emplace(At, convertSig(S));
  for (const auto &[At, N] : A.IndirectArgs)
    Out->IndirectArgs.emplace(At, N);
  for (const auto &[At, N] : A.CallsiteArgs)
    Out->CallsiteArgs.emplace(At, N);
  return std::shared_ptr<const lift::Signatures>(std::move(Out));
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
                          std::vector<StageReport> &Reports,
                          CxxFlavor Flavor,
                          const DenseSet<llvm::Function *> *UserFns,
                          bool Guarded,
                          function_ref<void(size_t, llvm::Function &)>
                              AfterRun = nullptr) {
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
  if (!Pipeline.consume_front("function(") || !Pipeline.consume_back(")"))
    return createStringError("expected a function pipeline");
  auto Passes = pipelineElements(Pipeline);
  // Build one single-pass manager per pass up front. Both paths keep the
  // original pass-major order (every function sees pass N before any
  // function sees pass N+1): some passes read callees and callers, so
  // reordering changes results. Unguarded runs with no clone and no
  // per-function verify; the caller restores the stage input and reruns
  // guarded if the stage verify at the end fails. Guarded is the original
  // algorithm: clone, run one pass, verify, restore on failure. Function
  // passes are not run concurrently over one module: passes mutate use
  // lists of shared callees and globals and create constants in one
  // LLVMContext, none of which is locked; a threaded attempt corrupted
  // Value use lists within seconds.
  std::vector<FunctionPassManager> Managers;
  for (StringRef Pass : Passes) {
    FunctionPassManager FPM;
    if (Pass == "revenant-uncxx")
      FPM.addPass(UncxxPass(Flavor, UserFns));
    else if (Error E = PB.parsePassPipeline(FPM, Pass))
      return createStringError("pipeline '" + Pass.str() +
                               "': " + toString(std::move(E)));
    Managers.push_back(std::move(FPM));
  }
  auto RestoreFrom = [](llvm::Function *F, llvm::Function *Saved,
                        FunctionAnalysisManager &FAM) {
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
  };
  for (size_t I = 0; I < Managers.size(); ++I) {
    SmallVector<llvm::Function *, 32> Functions;
    for (llvm::Function &F : M)
      if (!F.isDeclaration())
        Functions.push_back(&F);
    for (llvm::Function *F : Functions) {
      if (!Guarded) {
        FunctionStageTrace Trace(Stage, Passes[I], F->getName());
        Managers[I].run(*F, FAM);
        if (AfterRun)
          AfterRun(I, *F);
        continue;
      }
      FunctionStageTrace Trace(Stage, Passes[I], F->getName());
      ValueToValueMapTy SavedMap;
      llvm::Function *Saved = CloneFunction(F, SavedMap);
      Managers[I].run(*F, FAM);
      std::string Message;
      raw_string_ostream OS(Message);
      if (verifyFunction(*F, &OS)) {
        std::string Name = F->getName().str();
        RestoreFrom(F, Saved, FAM);
        record(Reports, Stage, "failed",
               Name + ": " + Passes[I].str() + ": " + Message);
        errs() << "[failed] " << Stage << ": " << Name << ": " << Passes[I]
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
                      StringRef Pipeline, std::vector<StageReport> &Reports,
                      CxxFlavor Flavor = CxxFlavor::Auto,
                      const DenseSet<llvm::Function *> *UserFns = nullptr,
                      StringRef InputText = "") {
  // Validate the whole pipeline first: unknown pass names are configuration
  // errors, not recoverable failures of a particular function or module.
  PassBuilder PB;
  registerRevenantPasses(PB);
  ModulePassManager Parsed;
  if (Error E = PB.parsePassPipeline(Parsed, Pipeline))
    return E;
  auto RunAll = [&](bool Guarded) -> Error {
    for (StringRef Element : pipelineElements(Pipeline)) {
      if (Element.starts_with("function(")) {
        if (Error E = runFunctionPipeline(Stage, *M, Element, Reports,
                                          Flavor, UserFns, Guarded))
          return E;
        continue;
      }
      FunctionStageTrace Trace(Stage, Element, "module");
      auto Saved = CloneModule(*M);
      // Analysis managers die inside runModulePass before the module is
      // swapped.
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
  };
  // Insurance is paid only on failure. When the caller already holds the
  // stage input as text (the common path), restore by reparsing it; the
  // old premium was one module clone per stage plus one function clone
  // and one full verify per function, which cost more than the passes.
  // Without input text, fall back to one upfront module clone per stage.
  std::unique_ptr<Module> Saved;
  if (InputText.empty())
    Saved = CloneModule(*M);
  if (Error E = RunAll(/*Guarded=*/false)) {
    errs() << "[failed] " << Stage << ": " << toString(std::move(E)) << "\n";
    if (Saved) {
      M.swap(Saved);
    } else {
      SMDiagnostic ParseErr;
      std::unique_ptr<Module> Restored =
          parseIR(MemoryBufferRef(InputText, Stage), ParseErr,
                  M->getContext());
      if (!Restored)
        return createStringError("failed to restore stage input for " +
                                 Stage.str() + ": " +
                                 ParseErr.getMessage().str());
      M.swap(Restored);
    }
    return RunAll(/*Guarded=*/true);
  }
  return Error::success();
}

/// Enable struct-layout recovery and its associated debug types only when
/// explicitly requested. Set REVENANT_ENABLE_STRUCT_DI=1 to opt in.
bool structDIEnabled() {
  const char *Value = std::getenv("REVENANT_ENABLE_STRUCT_DI");
  return Value && StringRef(Value) == "1";
}

/// ir-clean: normalize lifted stack/address/width shapes, run cleanup and
/// the module-level ABI recovery, then idioms as the LAST function pass.
/// Idioms runs twice on purpose. The early run rewrites verified
/// multiply-high DAGs into udiv/sdiv and the quotient-times-divisor residual
/// into urem/srem while the DAG still has its source shape. The second
/// instcombine round then canonicalizes the surviving results, and
/// instcombine refolds a source-level three-way compare, min/max, abs or
/// saturating add/sub into llvm.scmp/llvm.smin/llvm.abs/llvm.uadd.sat, so the
/// late run converts those optimizer forms back to the compiler's
/// pre-optimization (source) compare/select shape for the emitter.
std::string cleanPipeline(bool EnableStructDI) {
  return formatv("function({0},{1}),revenant-lift-abi,"
                 "function(instcombine<no-verify-fixpoint>,early-cse,"
                 "revenant-lift-normalize,{1}){2}",
                 CleanupPipeline, IdiomsPipeline,
                 EnableStructDI ? ",revenant-lift-structs" : "")
      .str();
}

/// ir-final: deoptimization, type recovery, SSA-out, then control-flow shape.
/// `revenant-unhotcold` is a module pass (the cold-chunk search looks at
/// sibling functions), so it sits between the two function blocks: the pass
/// order is unchanged.
std::string finalPipeline(bool Patterns, bool IncludeUncxx) {
  return formatv("function({0}{1}),revenant-unhotcold,"
                 "function(revenant-types,revenant-ssaout,"
                 "revenant-structure{2})",
                 IncludeUncxx ? "revenant-uncxx," : "", UnoptPipeline,
                 Patterns ? ",revenant-patterns" : "")
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
    if (!Sigs) {
      auto Recovered =
          recoverSignatures(Prog, *Dis, Opts.LibraryDBPath);
      if (!Recovered)
        return Recovered.takeError();
      Sigs = std::move(*Recovered);
    }
    auto ClassificationOrErr =
        classifyFunctions(Out, Opts.LibraryDBPath);
    if (!ClassificationOrErr)
      return ClassificationOrErr.takeError();
    FunctionClassification Classification =
        std::move(*ClassificationOrErr);
    // Name the startup-traced entry so the C emitter's main normalization
    // fires. A discovered real name always wins over the traced spelling.
    if (Classification.UserEntry && Classification.UserEntryName) {
      auto Rec = Prog.Functions.find(*Classification.UserEntry);
      if (Rec != Prog.Functions.end() &&
          StringRef(Rec->second.Name).starts_with("sub_")) {
        Rec->second.Name = *Classification.UserEntryName;
        if (Prog.Names)
          (*Prog.Names)[*Classification.UserEntry] =
              *Classification.UserEntryName;
      }
    }
    // The classifier's UserEntry is the hosted `main` on every path that
    // finds it (narrow CRT handoff, MinGW argc/argv setup, PE-entry tail
    // chain). Spell it `main` while the record still has a generic name so
    // the C/C++ emitter applies the hosted argc/argv signature; Rust output
    // has no hosted entry.
    if (Classification.UserEntry && (Out == Lang::C || Out == Lang::Cpp)) {
      auto It = Prog.Functions.find(*Classification.UserEntry);
      if (It != Prog.Functions.end() &&
          It->second.Name.rfind("sub_", 0) == 0)
        It->second.Name = "main";
    }
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
    // Parents whose EH handlers resolved to a funclet: they get the funclet
    // inlined before cleanup, so the parallel clean path keeps them serial.
    DenseSet<uint64_t> FuncletOwners;
    // EH map targets can point at the entry instruction inside a discovered
    // funclet, rather than at its containing Program function's start.
    auto collectFunclet = [&](Va Handler, Va Owner) {
      auto Exact = Prog.Functions.find(Handler);
      if (Exact != Prog.Functions.end() && Handler != Owner) {
        FuncletAddresses.insert(Handler.Value);
        FuncletAliases[Handler.Value] = Handler.Value;
        FuncletOwners.insert(Owner.Value);
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
        FuncletOwners.insert(Owner.Value);
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
    // Funclet lifting and inlining work on any module: the serial path runs
    // them on M, the parallel path inside the shard that owns EH parents.
    // `Created`, when given, receives the globals each funclet lift made.
    auto liftFunclets =
        [&](Module &Mod, const Disasm &D,
            DenseMap<uint64_t, llvm::Function *> &Out,
            std::vector<llvm::GlobalVariable *> *Created) -> Error {
      for (uint64_t Address : FuncletAddresses) {
        Va At(Address);
        auto SignatureMap = *Sigs;
        lift::Signature FuncletSig;
        FuncletSig.Params.push_back({lift::ParamLoc::reg(llvm::X86::RDX), 64,
                                     false, std::string("frame")});
        FuncletSig.Ret = lift::RetLoc{false, 64};
        SignatureMap.Funcs[At] = std::move(FuncletSig);
        size_t Before = Mod.global_size();
        auto L = lift::liftFunction(Mod, Prog, Prog.Functions.at(At), D,
                                    &SignatureMap, /*TM=*/nullptr,
                                    /*RunPasses=*/false, /*EH=*/nullptr);
        if (!L) {
          std::string Message = toString(L.takeError());
          return createStringError(inconvertibleErrorCode(),
                                   "failed to lift EH funclet 0x%llx: %s",
                                   static_cast<unsigned long long>(Address),
                                   Message.c_str());
        }
        Out[Address] = L->Fn;
        if (Created) {
          size_t K = 0;
          for (llvm::GlobalVariable &G : Mod.globals())
            if (K++ >= Before)
              Created->push_back(&G);
        }
      }
      return Error::success();
    };
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
    auto inlineFunclets =
        [&](Module &Mod, DenseMap<uint64_t, llvm::Function *> &Funclets,
            bool EraseUnused) -> Error {
      if (!Funclets.empty()) {
        SmallVector<llvm::Function *, 32> Parents;
        for (llvm::Function &F : Mod)
          Parents.push_back(&F);
        for (llvm::Function *ParentPtr : Parents) {
          llvm::Function &Parent = *ParentPtr;
          if (Parent.isDeclaration())
            continue;
          auto ParentVA = ir::getFunctionVA(Parent);
          if (ParentVA && FuncletAddresses.contains(*ParentVA))
            continue;
          bool IsSEHParent = false;
          if (Parent.hasPersonalityFn())
            if (auto *Personality = dyn_cast<llvm::GlobalValue>(
                    Parent.getPersonalityFn()->stripPointerCasts()))
              IsSEHParent = Personality->getName() == "__C_specific_handler";
          llvm::Value *ParentStack = nullptr;
          for (llvm::Instruction &I : Parent.getEntryBlock())
            if (I.getName() == "sp0")
              ParentStack = &I;
          if (!ParentStack)
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
            auto *Address =
                AddressMD ? dyn_cast<llvm::ConstantInt>(AddressMD->getValue())
                          : nullptr;
            if (!Address)
              continue;
            auto Alias = FuncletAliases.find(Address->getZExtValue());
            if (Alias == FuncletAliases.end())
              continue;
            llvm::Function *Funclet = Funclets.lookup(Alias->second);
            if (!Funclet || Funclet->arg_size() != 1 || BB.empty())
              continue;
            llvm::Instruction *Ret = BB.getTerminator();
            if (!Ret)
              continue;
            std::optional<uint64_t> ContinueVA;
            if (llvm::MDNode *Continuations =
                    Pad->getMetadata("revenant.continuations"))
              if (Continuations->getNumOperands() == 1)
                if (auto *MD = dyn_cast<llvm::ConstantAsMetadata>(
                        Continuations->getOperand(0)))
                  if (auto *Value =
                          dyn_cast<llvm::ConstantInt>(MD->getValue()))
                    ContinueVA = Value->getZExtValue();
            if (!ContinueVA && !IsSEHParent)
              ContinueVA = continuationVA(*Funclet);
            if (IsSEHParent)
              ContinueVA.reset();
            if (!Pad->getMetadata("revenant.catchtype"))
              ContinueVA.reset();
            if (ContinueVA)
              if (auto *CatchRet = dyn_cast<llvm::CatchReturnInst>(Ret)) {
                std::string Name = "bb_" + utohexstr(*ContinueVA, true);
                for (llvm::BasicBlock &Next : Parent)
                  if (Next.getName() == Name) {
                    llvm::BasicBlock *Old = CatchRet->getSuccessor();
                    for (llvm::PHINode &Phi : Old->phis())
                      Phi.removeIncomingValue(CatchRet->getParent(), false);
                    for (llvm::PHINode &Phi : Next.phis())
                      if (Phi.getBasicBlockIndex(CatchRet->getParent()) < 0)
                        Phi.addIncoming(llvm::UndefValue::get(Phi.getType()),
                                        CatchRet->getParent());
                    CatchRet->setSuccessor(&Next);
                    break;
                  }
              }
            llvm::IRBuilder<> Builder(Ret);
            llvm::Value *FrameArg = Builder.CreatePtrToInt(
                ParentStack, Funclet->getArg(0)->getType());
            SmallVector<llvm::Value *, 1> BundleInputs{Pad};
            SmallVector<OperandBundleDef, 1> Bundles{
                OperandBundleDef("funclet", BundleInputs)};
            SmallVector<llvm::Value *, 1> Args{FrameArg};
            llvm::CallInst *Call =
                Builder.CreateCall(Funclet->getFunctionType(), Funclet, Args,
                                   Bundles, "eh.funclet");
            llvm::InlineFunctionInfo IFI;
            if (!llvm::InlineFunction(*Call, IFI).isSuccess())
              return createStringError(inconvertibleErrorCode(),
                                       "failed to inline EH funclet into %s",
                                       Parent.getName().str().c_str());
          }
        }
      }
      if (EraseUnused)
        for (auto &Entry : Funclets)
          if (Entry.second->use_empty())
            Entry.second->eraseFromParent();
      return Error::success();
    };
    // Parallel lift+clean. Whole-program decisions (signatures,
    // classification) are final above, so per-function work can fan out:
    // each worker lifts its share and runs the first clean block in its own
    // context and module, then hands back bitcode that is linked into M.
    // Merging after the first clean block, not after raw lift, is what pays:
    // cleaned IR is several times smaller than raw IR.
    //
    // Three things keep the result identical to the serial path:
    //  - Callee attributes. In serial, every callee is already a lifted
    //    definition (noreturn, revenant.*) when cleanup starts; a worker
    //    only has bare declarations for other shards. Wave 1 lifts, then
    //    definition attributes are copied onto cross-shard declarations,
    //    then wave 2 cleans.
    //  - Global names. Each lifted function creates its own global per
    //    address, and LLVM's module-wide uniquing counter names them in
    //    creation order (g_X, g_X.58, ...); the C output prints these
    //    names. Globals travel through the link under unique tokens with
    //    external linkage (the linker drops unreferenced internal globals)
    //    and are renamed afterwards in serial creation order.
    //  - Module order. Definitions and globals are put back in serial order.
    // EH funclet owners share one shard that lifts and inlines funclets
    // before cleanup, exactly as the serial path does on M.
    // Any failure resets M and the serial path runs unchanged. Source emit
    // only: the raw-IR artifact has no serial meaning here.
    struct CleanSlot {
      bool Failed = false;
      std::string Note;
      size_t Opaque = 0;
    };
    std::vector<CleanSlot> CleanSlots(Todo.size());
    bool ParallelClean = false;
    std::string CleanRemainder;
    std::string CleanBlock1;
    // Serial global names in creation order, and the module name counter
    // serial leaves after lift (parallel path).
    std::vector<std::string> GlobalOrder;
    uint32_t SerialGlobalCounter = 0;
    {
      const char *PC = std::getenv("REVENANT_PARALLEL_CLEAN");
      unsigned Threads = std::thread::hardware_concurrency();
      if (const char *V = std::getenv("REVENANT_THREADS"))
        Threads = std::max(1u, static_cast<unsigned>(std::atoi(V)));
      bool Wanted = PC && StringRef(PC) == "1" && Threads > 1 &&
                    Todo.size() >= 256 && Opts.Emit == EmitKind::Source &&
                    !structDIEnabled();
      if (Wanted) {
        auto Elements = pipelineElements(cleanPipeline(false));
        CleanBlock1 = Elements.front().str();
        for (size_t I = 1; I < Elements.size(); ++I) {
          if (!CleanRemainder.empty())
            CleanRemainder += ",";
          CleanRemainder += Elements[I].str();
        }
        // Shard Threads holds EH funclet owners (and lifts their funclets).
        // Shards keep Todo order inside, like the serial module.
        std::vector<std::vector<size_t>> Shards(Threads + 1);
        std::vector<size_t> Loads(Threads, 0);
        std::vector<size_t> Order(Todo.size());
        std::iota(Order.begin(), Order.end(), size_t(0));
        llvm::sort(Order, [&](size_t A, size_t B) {
          return Prog.Functions.at(Todo[A]).InstrRvas.size() >
                 Prog.Functions.at(Todo[B]).InstrRvas.size();
        });
        for (size_t Idx : Order) {
          if (FuncletAddresses.contains(Todo[Idx].Value))
            continue;
          if (FuncletOwners.contains(Todo[Idx].Value)) {
            Shards[Threads].push_back(Idx);
            continue;
          }
          size_t Best = 0;
          for (size_t S = 1; S < Threads; ++S)
            if (Loads[S] < Loads[Best])
              Best = S;
          Shards[Best].push_back(Idx);
          Loads[Best] += Prog.Functions.at(Todo[Idx]).InstrRvas.size();
        }
        for (std::vector<size_t> &Shard : Shards)
          llvm::sort(Shard);
        // Funclets exist only to be inlined into owners.
        if (!FuncletAddresses.empty() && Shards[Threads].empty())
          Shards[Threads].push_back(SIZE_MAX);
        std::vector<unsigned> ShardOf(Todo.size(), ~0u);
        for (unsigned S = 0; S < Shards.size(); ++S)
          for (size_t Idx : Shards[S])
            if (Idx != SIZE_MAX)
              ShardOf[Idx] = S;
        lift::Signature FuncletSig;
        FuncletSig.Params.push_back(
            {lift::ParamLoc::reg(llvm::X86::RDX), 64, false,
             std::string("frame")});
        FuncletSig.Ret = lift::RetLoc{false, 64};
        if (Compiler.Producer == revenant::Compiler::Rustc)
          for (FunctionEH &EH : EHFunctions)
            EH.Model = EHModel::Rust;
        struct ShardState {
          std::unique_ptr<LLVMContext> Ctx;
          std::unique_ptr<Module> Mod;
          // (Todo index, globals that lift created) in lift order.
          std::vector<std::pair<size_t, SmallVector<llvm::GlobalVariable *, 4>>>
              Created;
          DenseMap<uint64_t, llvm::Function *> Funclets;
          // Function-local name counters, carried across the bitcode trip.
          std::vector<std::pair<std::string, uint32_t>> Counters;
          // Use-list position of every instruction use of a global value:
          // (function, instruction ordinal, operand, position in use list).
          struct UseRec {
            uint32_t Fn, Inst, Op, Pos;
          };
          std::vector<std::string> UseFns;
          std::vector<UseRec> Uses;
          // Every function this shard created, with where it sits in the
          // serial timeline. Lift phase (0): slot = Todo position of the
          // function being lifted. Cleanup pass P (phase P+1): serial visits
          // functions in module (= creation) order, so the slot is the
          // processed function's creation rank, resolved after the merge
          // from `By`. Serial creation order is the minimum over shards.
          // `F` follows renames (a clashing declaration becomes `.decl`)
          // and deletion; `Name` is resolved just before serialization.
          struct FnKeyRec {
            WeakVH F;
            uint64_t Phase, Slot, Seq;
            std::string By;
            std::string Name;
          };
          std::vector<FnKeyRec> FnKeys;
          size_t FnSeen = 0;
          uint64_t FnSeq = 0;
          // Custom metadata kinds get IDs in first-use order, and IDs fix
          // the order attachments print in. Same timeline as functions.
          struct KindRec {
            std::string Name;
            uint64_t Phase, Slot, Seq;
            std::string By;
          };
          std::vector<KindRec> KindKeys;
          size_t KindSeen = 0;
          std::string Bitcode;
          std::string Err;
        };
        std::vector<ShardState> State(Threads + 1);
        auto runWave = [&](auto &&Body) {
          std::vector<std::thread> Workers;
          for (unsigned S = 0; S < State.size(); ++S)
            if (!Shards[S].empty())
              Workers.emplace_back([&, S] { Body(S); });
          for (std::thread &W : Workers)
            W.join();
        };
        auto kindCount = [](ShardState &St) {
          SmallVector<StringRef, 64> Kinds;
          St.Ctx->getMDKindNames(Kinds);
          return Kinds.size();
        };
        auto noteNewFunctions = [](ShardState &St, uint64_t Phase,
                                   uint64_t Slot, StringRef By) {
          size_t I = 0;
          for (llvm::Function &F : *St.Mod)
            if (I++ >= St.FnSeen)
              St.FnKeys.push_back(
                  {WeakVH(&F), Phase, Slot, St.FnSeq++, By.str(), ""});
          St.FnSeen = St.Mod->size();
          SmallVector<StringRef, 64> Kinds;
          St.Ctx->getMDKindNames(Kinds);
          for (size_t K = St.KindSeen; K < Kinds.size(); ++K)
            St.KindKeys.push_back(
                {Kinds[K].str(), Phase, Slot, St.FnSeq++, By.str()});
          St.KindSeen = Kinds.size();
        };
        DenseMap<uint64_t, size_t> TodoSlot;
        for (size_t I = 0; I < Todo.size(); ++I)
          TodoSlot[Todo[I].Value] = I + 1;
        // Wave 1: lift.
        runWave([&](unsigned S) {
          ShardState &St = State[S];
          auto ThreadDis = Disasm::create(*Pe->Img);
          if (!ThreadDis) {
            St.Err = toString(ThreadDis.takeError());
            return;
          }
          St.Ctx = std::make_unique<LLVMContext>();
          St.Mod = moduleFor(*St.Ctx, Out);
          St.KindSeen = kindCount(St);
          // Replay the serial module state each lift sees: funclets are
          // defined before anything else, and when the function at Todo
          // slot s is lifted every function at a smaller slot is already
          // defined. Claiming those foreign definitions (declaration with
          // the definition's name and type, clashing callee declarations
          // renamed to `.decl`) makes calls bind exactly as in serial.
          size_t Claimed = 0;
          auto claimForeignBefore = [&](size_t Limit) {
            for (; Claimed < Limit; ++Claimed) {
              if (ShardOf[Claimed] == S ||
                  FuncletAddresses.contains(Todo[Claimed].Value))
                continue;
              const FuncRecord &Rec = Prog.Functions.at(Todo[Claimed]);
              lift::claimDefinition(*St.Mod, lift::definitionName(Rec),
                                    Sigs->of(Rec.Start));
            }
            // Mirrors of definitions owned elsewhere: not creations here.
            St.FnSeen = St.Mod->size();
          };
          if (S != Threads) {
            for (uint64_t Address : FuncletAddresses)
              lift::claimDefinition(
                  *St.Mod,
                  lift::definitionName(Prog.Functions.at(Va(Address))),
                  &FuncletSig);
            St.FnSeen = St.Mod->size();
          }
          if (S == Threads) {
            std::vector<llvm::GlobalVariable *> FuncletCreated;
            if (Error E = liftFunclets(*St.Mod, *ThreadDis, St.Funclets,
                                       &FuncletCreated)) {
              St.Err = toString(std::move(E));
              return;
            }
            St.Created.emplace_back(
                SIZE_MAX, SmallVector<llvm::GlobalVariable *, 4>(
                              FuncletCreated.begin(), FuncletCreated.end()));
            noteNewFunctions(St, 0, 0, "");
          }
          for (size_t Idx : Shards[S]) {
            if (Idx == SIZE_MAX)
              continue;
            Va T = Todo[Idx];
            claimForeignBefore(Idx);
            const FunctionEH *EH = nullptr;
            auto EHIt = llvm::find_if(EHFunctions,
                                      [T](const FunctionEH &Entry) {
                                        return Entry.Function == T;
                                      });
            if (EHIt != EHFunctions.end())
              EH = &*EHIt;
            size_t Before = St.Mod->global_size();
            auto L = lift::liftFunction(*St.Mod, Prog, Prog.Functions.at(T),
                                        *ThreadDis, Sigs.get(),
                                        /*TM=*/nullptr,
                                        /*RunPasses=*/false, EH);
            SmallVector<llvm::GlobalVariable *, 4> New;
            size_t K = 0;
            for (llvm::GlobalVariable &G : St.Mod->globals())
              if (K++ >= Before)
                New.push_back(&G);
            St.Created.emplace_back(Idx, std::move(New));
            noteNewFunctions(St, 0, Idx + 1, "");
            if (!L) {
              std::string Message = toString(L.takeError());
              CleanSlots[Idx].Failed = true;
              CleanSlots[Idx].Note =
                  formatv("0x{0:x}: {1}", T.Value, Message).str();
              continue;
            }
            CleanSlots[Idx].Opaque = L->AsmFallbacks.size();
            if (Classification.UserReachable.count(T))
              L->Fn->addFnAttr("revenant.user", "true");
            auto Library = Classification.LibraryNames.find(T);
            if (Library != Classification.LibraryNames.end())
              L->Fn->addFnAttr("revenant.lib", Library->second);
          }
          // Finish the timeline: serial defines every later function too,
          // renaming any clashing callee declaration to `.decl`.
          claimForeignBefore(Todo.size());
          if (S == Threads) {
            // Inlining erases spent funclets, so find new functions by set.
            DenseSet<llvm::Function *> Known;
            for (llvm::Function &F : *St.Mod)
              Known.insert(&F);
            // Funclets are erased only once no shard uses them; decided
            // after the wave.
            if (Error E = inlineFunclets(*St.Mod, St.Funclets,
                                         /*EraseUnused=*/false))
              St.Err = toString(std::move(E));
            for (llvm::Function &F : *St.Mod)
              if (!Known.contains(&F))
                St.FnKeys.push_back({WeakVH(&F), 0, Todo.size() + 1,
                                     St.FnSeq++, "", ""});
            St.FnSeen = St.Mod->size();
          }
        });
        std::string Why;
        for (ShardState &St : State)
          if (!St.Err.empty() && Why.empty())
            Why = "lift shard: " + St.Err;
        // Other shards claimed every definition up front; a lift that fails
        // before claiming its name never defines it in serial either.
        for (size_t Idx = 0; Idx < Todo.size() && Why.empty(); ++Idx) {
          if (!CleanSlots[Idx].Failed)
            continue;
          const FuncRecord &Rec = Prog.Functions.at(Todo[Idx]);
          llvm::Function *F =
              State[ShardOf[Idx]].Mod->getFunction(lift::definitionName(Rec));
          if (!F || ir::getFunctionVA(*F) != Rec.Start.Value)
            Why = "lift failed before defining " + lift::definitionName(Rec);
        }
        // Serial erases a funclet right after inlining when nothing uses it
        // anymore. Uses may sit in any shard, so decide here: erase spent
        // funclets in the EH shard, and drop unused funclet declarations
        // elsewhere so they do not reach the merged module.
        if (Why.empty() && !FuncletAddresses.empty()) {
          for (uint64_t Address : FuncletAddresses) {
            std::string Name =
                lift::definitionName(Prog.Functions.at(Va(Address)));
            bool Used = false;
            for (ShardState &St : State)
              if (St.Mod)
                if (llvm::Function *F = St.Mod->getFunction(Name))
                  Used |= !F->use_empty();
            if (Used)
              continue;
            for (ShardState &St : State)
              if (St.Mod)
                if (llvm::Function *F = St.Mod->getFunction(Name))
                  F->eraseFromParent();
          }
          for (ShardState &St : State)
            if (St.Mod)
              St.FnSeen = St.Mod->size();
        }
        // Copy definition attributes onto cross-shard declarations. Serial:
        // contexts are per shard, attributes are rebuilt in the target one.
        if (Why.empty()) {
          // Attribute source: the shard that owns the function, defined or
          // not (a lift that failed late leaves an attributed declaration).
          StringMap<llvm::Function *> DefByName;
          for (size_t Idx = 0; Idx < Todo.size(); ++Idx) {
            unsigned Owner = FuncletAddresses.contains(Todo[Idx].Value)
                                 ? Threads
                                 : ShardOf[Idx];
            if (Owner == ~0u || !State[Owner].Mod)
              continue;
            std::string Name =
                lift::definitionName(Prog.Functions.at(Todo[Idx]));
            if (llvm::Function *F = State[Owner].Mod->getFunction(Name))
              DefByName[Name] = F;
          }
          auto copyAttrs = [&](const AttributeSet &From, LLVMContext &C,
                               bool &Ok) {
            AttrBuilder B(C);
            for (const Attribute &A : From) {
              if (A.isStringAttribute())
                B.addAttribute(A.getKindAsString(), A.getValueAsString());
              else if (A.isIntAttribute())
                B.addRawIntAttr(A.getKindAsEnum(), A.getValueAsInt());
              else if (A.isEnumAttribute())
                B.addAttribute(A.getKindAsEnum());
              else
                Ok = false;
            }
            return AttributeSet::get(C, B);
          };
          for (unsigned S = 0; S < State.size() && Why.empty(); ++S) {
            if (!State[S].Mod)
              continue;
            LLVMContext &C = *State[S].Ctx;
            for (llvm::Function &D : *State[S].Mod) {
              if (!D.isDeclaration() || D.isIntrinsic())
                continue;
              auto It = DefByName.find(D.getName());
              if (It == DefByName.end() || It->second == &D)
                continue;
              llvm::Function *Def = It->second;
              if (Def->arg_size() != D.arg_size()) {
                Why = "signature mismatch for " + D.getName().str();
                break;
              }
              AttributeList From = Def->getAttributes();
              bool Ok = true;
              SmallVector<AttributeSet, 8> Params;
              for (unsigned I = 0; I < D.arg_size(); ++I)
                Params.push_back(copyAttrs(From.getParamAttrs(I), C, Ok));
              AttributeList To = AttributeList::get(
                  C, copyAttrs(From.getFnAttrs(), C, Ok),
                  copyAttrs(From.getRetAttrs(), C, Ok), Params);
              if (!Ok) {
                Why = "non-portable attribute on " + D.getName().str();
                break;
              }
              D.setAttributes(To);
            }
          }
        }
        // Cleanup is pass-major over the module's function list, and the
        // first function in that order to need an intrinsic creates it.
        // Put each shard's functions in serial order (lift-phase creation
        // keys across all shards) so creations attribute as in serial.
        if (Why.empty()) {
          StringMap<std::pair<uint64_t, uint64_t>> LiftKey;
          for (ShardState &St : State)
            for (const auto &R : St.FnKeys)
              if (R.Phase == 0 && R.F) {
                auto Key = std::make_pair(R.Slot, R.Seq);
                auto [It, New] = LiftKey.try_emplace(R.F->getName(), Key);
                if (!New)
                  It->second = std::min(It->second, Key);
              }
          const std::pair<uint64_t, uint64_t> Last{UINT64_MAX, UINT64_MAX};
          auto keyOf = [&](const llvm::Function *F) {
            auto It = LiftKey.find(F->getName());
            return It == LiftKey.end() ? Last : It->second;
          };
          for (ShardState &St : State) {
            if (!St.Mod)
              continue;
            std::vector<llvm::Function *> All;
            for (llvm::Function &F : *St.Mod)
              All.push_back(&F);
            llvm::stable_sort(All, [&](llvm::Function *A, llvm::Function *B) {
              return keyOf(A) < keyOf(B);
            });
            auto &List = St.Mod->getFunctionList();
            for (size_t I = All.size(); I-- > 0;)
              List.splice(List.begin(), List, All[I]->getIterator());
          }
        }
        // Give every global its serial name before cleanup runs: passes
        // derive local names from global names (simplifycfg's `<g>.mux`).
        // All globals come from lift, created in serial order funclets
        // first, then Todo order; replay LLVM's makeUniqueName over that
        // sequence (bump the module counter, append `.N` until free).
        if (Why.empty()) {
          std::vector<llvm::GlobalVariable *> Seq;
          std::vector<const SmallVector<llvm::GlobalVariable *, 4> *> ByIdx(
              Todo.size(), nullptr);
          for (ShardState &St : State)
            for (auto &[Idx, Globals] : St.Created)
              if (Idx == SIZE_MAX)
                Seq.insert(Seq.end(), Globals.begin(), Globals.end());
              else
                ByIdx[Idx] = &Globals;
          for (const auto *Globals : ByIdx)
            if (Globals)
              Seq.insert(Seq.end(), Globals->begin(), Globals->end());
          StringSet<> Taken;
          for (ShardState &St : State)
            if (St.Mod)
              for (llvm::Function &F : *St.Mod)
                Taken.insert(F.getName());
          uint32_t Counter = 0;
          GlobalOrder.reserve(Seq.size());
          for (llvm::GlobalVariable *G : Seq) {
            auto VA = ir::getGlobalVA(*G);
            if (!VA) {
              Why = "global without VA";
              break;
            }
            std::string Base = lift::globalNameFor(Prog, Va(*VA));
            std::string Name = Base;
            while (!Taken.insert(Name).second)
              Name = Base + "." + std::to_string(++Counter);
            GlobalOrder.push_back(std::move(Name));
          }
          if (Why.empty()) {
            for (size_t I = 0; I < Seq.size(); ++I)
              Seq[I]->setName("\1rvt." + Twine(I));
            for (size_t I = 0; I < Seq.size() && Why.empty(); ++I) {
              Seq[I]->setName(GlobalOrder[I]);
              if (Seq[I]->getName() != GlobalOrder[I])
                Why = "global name collision on " + GlobalOrder[I];
            }
          }
          SerialGlobalCounter = Counter;
        }
        // Wave 2: first clean block, then record what the merge restores.
        if (Why.empty()) {
          runWave([&](unsigned S) {
            ShardState &St = State[S];
            std::vector<StageReport> LocalReports;
            auto AfterRun = [&](size_t Pass, llvm::Function &F) {
              if (St.Mod->size() == St.FnSeen && kindCount(St) == St.KindSeen)
                return;
              noteNewFunctions(St, 1 + Pass, 0, F.getName());
            };
            if (Error E = runFunctionPipeline(
                    "ir-clean", *St.Mod, StringRef(CleanBlock1), LocalReports,
                    CxxFlavor::Auto, nullptr, /*Guarded=*/false, AfterRun)) {
              St.Err = toString(std::move(E));
              return;
            }
            for (llvm::Function &F : *St.Mod)
              if (!F.isDeclaration())
                St.Counters.emplace_back(
                    F.getName().str(), lastUnique(*F.getValueSymbolTable()));
            {
              DenseMap<const Instruction *, std::pair<uint32_t, uint32_t>>
                  Ord;
              for (llvm::Function &F : *St.Mod) {
                if (F.isDeclaration())
                  continue;
                uint32_t FnId = St.UseFns.size();
                St.UseFns.push_back(F.getName().str());
                uint32_t N = 0;
                for (Instruction &I : instructions(F))
                  Ord[&I] = {FnId, N++};
              }
              for (GlobalValue &GV : St.Mod->global_values()) {
                uint32_t Pos = 0;
                for (Use &U : GV.uses()) {
                  if (auto *I = dyn_cast<Instruction>(U.getUser()))
                    if (auto It = Ord.find(I); It != Ord.end())
                      St.Uses.push_back({It->second.first, It->second.second,
                                         U.getOperandNo(), Pos});
                  ++Pos;
                }
              }
            }
            raw_string_ostream BC(St.Bitcode);
            for (auto &R : St.FnKeys)
              if (R.F)
                R.Name = R.F->getName().str();
            // Use-list order drives pass iteration over users; keep it.
            WriteBitcodeToFile(*St.Mod, BC,
                               /*ShouldPreserveUseListOrder=*/true);
            BC.flush();
            St.Mod.reset();
            St.Ctx.reset();
          });
          for (unsigned S = 0; S < State.size(); ++S)
            if (!State[S].Err.empty() && Why.empty())
              Why = "clean shard: " + State[S].Err.substr(0, 400);
        }
        // Serial creation rank of every function by name, from lift-phase
        // keys: cleanup visits functions in this order.
        StringMap<uint64_t> SerialRank;
        if (Why.empty()) {
          StringMap<std::pair<uint64_t, uint64_t>> LiftKey;
          for (ShardState &St : State)
            for (const auto &R : St.FnKeys)
              if (R.Phase == 0 && !R.Name.empty()) {
                auto [It, New] = LiftKey.try_emplace(R.Name, R.Slot, R.Seq);
                if (!New)
                  It->second = std::min(It->second, std::make_pair(R.Slot, R.Seq));
              }
          std::vector<std::pair<std::pair<uint64_t, uint64_t>, StringRef>> Ord;
          for (auto &E : LiftKey)
            Ord.push_back({E.second, E.first()});
          llvm::sort(Ord);
          for (size_t I = 0; I < Ord.size(); ++I)
            SerialRank[Ord[I].second] = I;
          // Register custom metadata kinds in serial first-use order before
          // the shards' bitcode registers them in link order.
          using KKey = std::tuple<uint64_t, uint64_t, uint64_t>;
          StringMap<KKey> KindFirst;
          for (ShardState &St : State)
            for (const auto &R : St.KindKeys) {
              uint64_t Slot = R.Slot;
              if (R.Phase != 0) {
                auto It = SerialRank.find(R.By);
                Slot = It == SerialRank.end() ? UINT64_MAX : It->second;
              }
              KKey K{R.Phase, Slot, R.Seq};
              auto [It, New] = KindFirst.try_emplace(R.Name, K);
              if (!New)
                It->second = std::min(It->second, K);
            }
          std::vector<std::pair<KKey, StringRef>> Kinds;
          for (auto &E : KindFirst)
            Kinds.push_back({E.second, E.first()});
          llvm::sort(Kinds);
          for (auto &[K, Name] : Kinds)
            Ctx.getMDKindID(Name);
        }
        if (Why.empty()) {
          ParallelClean = true;
          // Shards are parsed into M's own context, so types, constants and
          // metadata are already shared: merging is splicing functions and
          // globals into M and folding same-name declarations onto one
          // value. IRMover would remap every operand and rescan the whole
          // destination per shard, which made merging quadratic.
          auto mergeShard = [&](Module &Src) -> std::string {
            for (llvm::Function &F : make_early_inc_range(Src)) {
              llvm::Function *Dst = M->getFunction(F.getName());
              if (Dst && Dst->getFunctionType() != F.getFunctionType())
                return "type clash on " + F.getName().str();
              if (Dst && F.isDeclaration()) {
                F.replaceAllUsesWith(Dst);
                F.eraseFromParent();
                continue;
              }
              if (Dst && !Dst->isDeclaration())
                return "duplicate definition of " + F.getName().str();
              std::string Name;
              if (Dst) {
                Name = Dst->getName().str();
                Dst->setName("");
              }
              M->getFunctionList().splice(M->end(), Src.getFunctionList(), F);
              if (Dst) {
                Dst->replaceAllUsesWith(&F);
                Dst->eraseFromParent();
                if (F.getName() != Name)
                  return "rename while merging " + Name;
              }
            }
            for (llvm::GlobalVariable &G : make_early_inc_range(Src.globals())) {
              if (M->getNamedValue(G.getName()))
                return "duplicate global " + G.getName().str();
              std::string Name = G.getName().str();
              Src.removeGlobalVariable(&G);
              M->insertGlobalVariable(&G);
              if (G.getName() != Name)
                return "rename while merging " + Name;
            }
            // Module-level named metadata (`rev.types`, module flags) is
            // written once per module from whole-program data, so every
            // shard holds the same uniqued nodes: adopt the first copy,
            // require the rest to match.
            for (NamedMDNode &N : Src.named_metadata()) {
              NamedMDNode *Dst = M->getOrInsertNamedMetadata(N.getName());
              if (Dst->getNumOperands() == 0) {
                for (MDNode *Op : N.operands())
                  Dst->addOperand(Op);
                continue;
              }
              if (Dst->getNumOperands() != N.getNumOperands())
                return "named metadata differs: " + N.getName().str();
              for (unsigned I = 0; I < N.getNumOperands(); ++I)
                if (Dst->getOperand(I) != N.getOperand(I))
                  return "named metadata differs: " + N.getName().str();
            }
            return "";
          };
          for (unsigned S = 0; S < State.size() && ParallelClean; ++S) {
            if (State[S].Bitcode.empty())
              continue;
            auto ShardOrErr = parseBitcodeFile(
                MemoryBufferRef(State[S].Bitcode, "clean-shard"), Ctx);
            if (!ShardOrErr) {
              Why = "parse: " + toString(ShardOrErr.takeError());
              ParallelClean = false;
              break;
            }
            std::string Err = mergeShard(**ShardOrErr);
            if (!Err.empty()) {
              Why = "merge: " + Err;
              ParallelClean = false;
            }
            State[S].Bitcode.clear();
          }
        }
        if (ParallelClean) {
          for (unsigned S = 0; S < State.size() && ParallelClean; ++S)
            for (auto &[Name, Counter] : State[S].Counters)
              if (llvm::Function *F = M->getFunction(Name))
                lastUnique(*F->getValueSymbolTable()) = Counter;
          // Linking re-points every use of a global value, so use lists come
          // out in link order. Passes such as revenant-lift-abi walk
          // F.users() and name what they create in the caller, so restore
          // the order: inside a caller, the worker's (= serial) order;
          // across callers, later-lifted first, since LLVM prepends uses.
          if (ParallelClean) {
            DenseMap<const Use *, uint32_t> PosOf;
            for (ShardState &St : State) {
              std::vector<std::vector<Instruction *>> InstsOf(St.UseFns.size());
              for (size_t FnId = 0; FnId < St.UseFns.size(); ++FnId)
                if (llvm::Function *F = M->getFunction(St.UseFns[FnId]))
                  for (Instruction &I : instructions(*F))
                    InstsOf[FnId].push_back(&I);
              for (const auto &R : St.Uses)
                if (R.Inst < InstsOf[R.Fn].size())
                  PosOf[&InstsOf[R.Fn][R.Inst]->getOperandUse(R.Op)] = R.Pos;
            }
            DenseMap<uint64_t, size_t> TodoRank;
            for (size_t I = 0; I < Todo.size(); ++I)
              TodoRank[Todo[I].Value] = I;
            DenseMap<const llvm::Function *, size_t> Rank;
            size_t Ordinal = 0;
            for (llvm::Function &F : *M) {
              auto VA = ir::getFunctionVA(F);
              auto It = VA ? TodoRank.find(*VA) : TodoRank.end();
              Rank[&F] = It != TodoRank.end() ? It->second
                                              : Todo.size() + Ordinal;
              ++Ordinal;
            }
            auto key = [&](const Use &U) {
              auto *I = dyn_cast<Instruction>(U.getUser());
              if (!I)
                return std::make_tuple(1, size_t(0), uint32_t(0));
              auto P = PosOf.find(&U);
              return std::make_tuple(
                  0, ~Rank.lookup(I->getFunction()),
                  P == PosOf.end() ? UINT32_MAX : P->second);
            };
            for (GlobalValue &GV : M->global_values())
              if (!GV.use_empty() && !GV.hasOneUse())
                GV.sortUseList([&](const Use &L, const Use &R) {
                  return key(L) < key(R);
                });
            // The serial module lists functions in creation order: a callee
            // appears when first referenced, an intrinsic when a pass first
            // needs it. Sort by each function's earliest creation key across
            // shards; later stages and the C emitter iterate in this order.
            using CKey = std::tuple<uint64_t, uint64_t, uint64_t>;
            auto minInto = [](DenseMap<const llvm::Function *, CKey> &Map,
                              const llvm::Function *F, CKey K) {
              auto [It, New] = Map.try_emplace(F, K);
              if (!New)
                It->second = std::min(It->second, K);
            };
            // Lift phase first: it fixes the module order cleanup visits.
            DenseMap<const llvm::Function *, CKey> FnCreated;
            for (ShardState &St : State)
              for (const auto &R : St.FnKeys)
                if (R.Phase == 0)
                  if (llvm::Function *F = M->getFunction(R.Name))
                    minInto(FnCreated, F, {0, R.Slot, R.Seq});
            std::vector<const llvm::Function *> LiftOrder;
            for (auto &[F, K] : FnCreated)
              LiftOrder.push_back(F);
            llvm::sort(LiftOrder, [&](const llvm::Function *A,
                                      const llvm::Function *B) {
              return FnCreated.lookup(A) < FnCreated.lookup(B);
            });
            StringMap<uint64_t> RankByName;
            for (size_t I = 0; I < LiftOrder.size(); ++I)
              RankByName[LiftOrder[I]->getName()] = I;
            for (ShardState &St : State)
              for (const auto &R : St.FnKeys)
                if (R.Phase != 0)
                  if (llvm::Function *F = M->getFunction(R.Name)) {
                    auto It = RankByName.find(R.By);
                    if (It == RankByName.end()) {
                      Why = "cleanup creation by unranked " + R.By;
                      ParallelClean = false;
                      continue;
                    }
                    minInto(FnCreated, F, {R.Phase, It->second, R.Seq});
                  }
            constexpr CKey Last{UINT64_MAX, UINT64_MAX, UINT64_MAX};
            std::vector<llvm::Function *> All;
            for (llvm::Function &F : *M)
              All.push_back(&F);
            llvm::stable_sort(All, [&](llvm::Function *A, llvm::Function *B) {
              return FnCreated.lookup_or(A, Last) < FnCreated.lookup_or(B, Last);
            });
            auto &List = M->getFunctionList();
            for (size_t I = All.size(); I-- > 0;)
              List.splice(List.begin(), List, All[I]->getIterator());
          }
        }
        if (!ParallelClean) {
          errs() << "[note] parallel clean fell back to serial: " << Why
                 << "\n";
          M = moduleFor(Ctx, Out);
          CleanSlots.assign(Todo.size(), CleanSlot{});
          CleanRemainder.clear();
        }
      }
    }
    DenseMap<uint64_t, llvm::Function *> Funclets;
    if (!ParallelClean)
      if (Error E = liftFunclets(*M, *Dis, Funclets, nullptr))
        return E;
    DenseSet<llvm::Function *> UserFunctions;
    size_t OpaqueSites = 0;
    if (ParallelClean) {
      for (size_t Idx = 0; Idx < Todo.size(); ++Idx) {
        if (CleanSlots[Idx].Failed) {
          record(R.Stages, "lift", "failed", CleanSlots[Idx].Note);
          errs() << "[failed] lift: " << CleanSlots[Idx].Note << "\n";
        }
        OpaqueSites += CleanSlots[Idx].Opaque;
      }
      for (llvm::Function &F : *M)
        if (!F.isDeclaration() &&
            F.getFnAttribute("revenant.user").getValueAsString() == "true")
          UserFunctions.insert(&F);
    }
    // Serial lift: the fallback, and the only path for funclet binaries
    // and non-source emits. Lift alone does not shard well: merging raw
    // lifted IR costs more than the lift (21.6s vs 9.3s on sqlite).
    for (size_t TodoIdx = 0; TodoIdx < Todo.size(); ++TodoIdx) {
      Va T = Todo[TodoIdx];
      if (FuncletAddresses.contains(T.Value) || ParallelClean)
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
      llvm::Function *F = L->Fn;
      if (Classification.UserReachable.count(T))
        F->addFnAttr("revenant.user", "true");
      auto Library = Classification.LibraryNames.find(T);
      if (Library != Classification.LibraryNames.end())
        F->addFnAttr("revenant.lib", Library->second);
      if (Classification.UserReachable.count(T))
        UserFunctions.insert(F);
      OpaqueSites += L->AsmFallbacks.size();
    }
    if (!ParallelClean)
      if (Error E = inlineFunclets(*M, Funclets, /*EraseUnused=*/true))
        return E;
    if (ParallelClean) {
      // Globals already carry their serial names. Restore the module name
      // counter serial leaves after lift and the global creation order.
      lastUnique(M->getValueSymbolTable()) = SerialGlobalCounter;
      for (const std::string &Name : GlobalOrder) {
        llvm::GlobalVariable *G = M->getNamedGlobal(Name);
        if (!G)
          return createStringError("parallel clean: lost global " + Name);
        M->removeGlobalVariable(G);
        M->insertGlobalVariable(G);
      }
    }
    if (Error E = verifyStage("lift", *M))
      return E;
    record(
        R.Stages, "lift", "ok",
        formatv("revenant::lift::liftFunction, {0} opaque sites", OpaqueSites)
            .str());
    // After parallel lift+clean the module already went through the first
    // clean block, so it is not raw IR: no ir-raw artifact is produced.
    if (!ParallelClean)
      RawText = moduleText(*M);
    if (Opts.Emit == EmitKind::IrRaw) {
      R.Text = RawText;
      break;
    }

    // ---- ir-clean ----------------------------------------------------------
    if (Error E = runPipelineText(
            "ir-clean", M,
            ParallelClean ? CleanRemainder : cleanPipeline(structDIEnabled()),
            R.Stages, CxxFlavor::Auto, nullptr, RawText))
      return std::move(E);
    record(R.Stages, "ir-clean", "ok", "cleanup + idioms");
    CleanText = moduleText(*M);
    if (Opts.Emit == EmitKind::IrClean) {
      R.Text = CleanText;
      break;
    }

    // ---- ir-pre-like --------------------------------------------------------
    // Run the deopt pipeline with locals-to-slots instead of ssaout, then
    // annotate the final CFG after critical-edge blocks have been added.
    if (Opts.Emit == EmitKind::IrPreLike) {
      std::string PreLikePipeline =
          formatv("function({0},revenant-unloopvec,revenant-types,"
                  "revenant-locals-to-slots,revenant-structure)",
                  UnoptPipeline)
              .str();
      if (auto E = runPipelineText("ir-pre-like", M, PreLikePipeline,
                                   R.Stages, uncxxFlavor(Compiler.Producer),
                                   &UserFunctions))
        return std::move(E);
      record(R.Stages, "ir-pre-like", "ok",
             "deopt + types + locals-to-slots + structure");
      R.Text = moduleText(*M);
      break;
    }

    // ---- ir-final ----------------------------------------------------------
    if (auto E = runPipelineText("ir-final", M,
                                 finalPipeline(Opts.Patterns, Out == Lang::Cpp),
                                 R.Stages, uncxxFlavor(Compiler.Producer),
                                 &UserFunctions, CleanText))
      return std::move(E);
    record(R.Stages, "ir-final", "ok",
           "Uncxx (C++ only) + unopt + structure + types + ssaout");
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
