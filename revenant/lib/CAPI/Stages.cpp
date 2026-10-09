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

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace revenant {
namespace {

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
    LP.Loc = P.Loc.isStack()
                 ? lift::ParamLoc::stack(P.Loc.Off)
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

std::shared_ptr<const lift::Signatures>
recoverSignatures(const Program &Prog, const Disasm &Dis) {
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

/// Parse and run `Pipeline` over M. A pass name this binary does not know is
/// an error, not a skipped stage.
Error runPipelineText(Module &M, StringRef Pipeline) {
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
  if (Error E = PB.parsePassPipeline(MPM, Pipeline))
    return createStringError("pipeline '" + Pipeline.str() +
                             "': " + toString(std::move(E)));
  MPM.run(M, MAM);
  return Error::success();
}

/// ir-clean: lifter cleanup plus the IR idiom stage.
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
  for (Va T : Todo) {
    auto FOrErr = materialize(T);
    if (!FOrErr)
      return FOrErr.takeError();
    auto A = disasmFunction(T);
    if (!A)
      return A.takeError();
    AsmText += *A;
    InstrCount += FOrErr->Instrs.size();
    if (Opts.Emit == EmitKind::Asm)
      for (const revenant::Instr &I : FOrErr->Instrs)
        R.LineVas.push_back(
            {static_cast<uint32_t>(R.LineVas.size() + 1), {I.Addr}});
  }
  record(R.Stages, "asm", "ok",
         formatv("{0} instructions across {1} functions", InstrCount,
                 Todo.size())
             .str());
  if (Opts.Emit == EmitKind::Asm)
    R.Text = AsmText;

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
  size_t OpaqueSites = 0;
  for (Va T : Todo) {
    auto L = lift::liftFunction(*M, Prog, Prog.Functions.at(T), *Dis,
                                Sigs.get(), /*TM=*/nullptr,
                                /*RunPasses=*/false);
    if (!L)
      return L.takeError();
    OpaqueSites += L->AsmFallbacks.size();
  }
  record(R.Stages, "lift", "ok",
         formatv("revenant::lift::liftFunction, {0} opaque sites",
                 OpaqueSites)
             .str());
  std::string RawText = moduleText(*M);
  if (Opts.Emit == EmitKind::IrRaw)
    R.Text = RawText;

  // ---- ir-clean ----------------------------------------------------------
  if (auto E = runPipelineText(*M, cleanPipeline()))
    return std::move(E);
  record(R.Stages, "ir-clean", "ok", "cleanup + idioms");
  std::string CleanText = moduleText(*M);
  if (Opts.Emit == EmitKind::IrClean)
    R.Text = CleanText;

  // ---- ir-final ----------------------------------------------------------
  if (auto E = runPipelineText(*M, finalPipeline(Opts.Patterns)))
    return std::move(E);
  record(R.Stages, "ir-final", "ok", "unopt + structure + types + ssaout");
  std::string FinalText = moduleText(*M);
  if (Opts.Emit == EmitKind::IrFinal)
    R.Text = FinalText;

  // ---- source ------------------------------------------------------------
  if (Opts.Emit == EmitKind::Source) {
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
