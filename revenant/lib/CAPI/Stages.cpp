//==- lib/CAPI/Stages.cpp - the decompile stage pipeline ------------------==//
//
// Per run: asm -> sigs -> lift (ir-raw) -> cleanup + idioms (ir-clean) ->
// unopt + types, then locals-to-slots + structure (ir-final) or ssaout +
// structure (source). Every stage is the real implementation; a stage that
// cannot run fails the whole run. Each stage reports its status and elapsed
// time into StageReport, and writes its artifact next to the output when
// `DecompileOpts::ArtifactBase` is set.
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
#include "llvm/TargetParser/Triple.h"
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
#include "llvm/IR/Constants.h"
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

using StageClock = std::chrono::steady_clock;

/// Set REVENANT_ARTIFACT_TIMING=1 to print what the artifact path costs.
bool artifactTimingEnabled() {
  static const bool On = [] {
    const char *V = std::getenv("REVENANT_ARTIFACT_TIMING");
    return V && *V == '1';
  }();
  return On;
}

void reportArtifactTiming(StringRef What, int64_t Micros) {
  if (!artifactTimingEnabled())
    return;
  errs() << "[artifact-time] " << What << ": "
         << formatv("{0:F1}", Micros / 1000.0) << " ms\n";
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

thread_local StageClock::time_point LastStageTime;
thread_local bool StageClockStarted = false;

void resetStageClock() {
  LastStageTime = StageClock::now();
  StageClockStarted = true;
}

/// Set REVENANT_TRACE_STAGES=1 to print each stage as it finishes. The driver
/// otherwise prints the stage list only after the whole run, so a decompile
/// that takes minutes shows nothing until the end.
bool traceStagesEnabled() {
  static const bool Enabled = [] {
    const char *V = std::getenv("REVENANT_TRACE_STAGES");
    return V && *V && StringRef(V) != "0";
  }();
  return Enabled;
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
  if (traceStagesEnabled())
    errs() << "[trace] " << R.Status << " " << R.Name << " ("
           << formatv("{0:F2}", R.ElapsedMicroseconds / 1000.0) << " ms)\n";
  Stages.push_back(std::move(R));
}

/// Prepare a module that is about to be destroyed half-merged. Constant
/// expressions such as `ptrtoint (ptr @g to i64)` outlive their users and
/// keep a use on `@g`; an assertions build aborts in ~Value when the global
/// is destroyed with one left. Dropping every reference first makes them dead
/// so they can be deleted.
void releaseModule(Module &M) {
  M.dropAllReferences();
  for (GlobalValue &GV : M.global_values())
    GV.removeDeadConstantUsers();
}

//==--------------------------------------------------------------------==//
// Module skeleton and signatures
//==--------------------------------------------------------------------==//

/// Triple, datalayout and language flag. The lifter declares imports and
/// callees itself, typed from the recovered signatures.
std::unique_ptr<Module> moduleFor(LLVMContext &Ctx, Lang Out,
                                  Compiler Producer) {
  auto M = std::make_unique<Module>("revenant", Ctx);
  M->setTargetTriple(Triple(Producer == Compiler::MinGwGcc
                                ? "x86_64-w64-windows-gnu"
                                : "x86_64-pc-windows-msvc"));
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
    if (!P.Type.empty())
      LP.Type = P.Type;
    Out.Params.push_back(std::move(LP));
  }
  if (S.HasRet)
    Out.Ret = lift::RetLoc{S.Ret.Float, S.Ret.Bits};
  Out.NoReturn = S.Noreturn;
  Out.VarArgs = S.VarArgs;
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
    auto DB = loadLibraryDatabaseCached(LibraryDBPath);
    if (!DB)
      return DB.takeError();
    std::vector<LibraryMatch> Matches = matchLibraryFunctions(Prog, **DB);
    extendLibraryMatchesByCalleeEvidence(Prog, **DB, Matches);
    applyLibraryMatches(Prog, A, Matches, **DB);
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

/// The function the verifier complained about, when its message names one.
/// The verifier prints "Basic Block in function 'f' does not have
/// terminator!" for the block a pass left without a terminator. Returns an
/// empty name when the message names no function.
std::string verifierFunctionName(StringRef Message) {
  size_t Pos = Message.find("in function '");
  if (Pos == StringRef::npos)
    return "";
  Pos += sizeof("in function '") - 1;
  size_t End = Message.find('\'', Pos);
  if (End == StringRef::npos)
    return "";
  return Message.slice(Pos, End).str();
}

/// Parse and run `Pipeline` over M, then verify it as `Stage`. A pass name
/// this binary does not know is an error, not a skipped stage.
/// `Only` runs the element for the given functions alone (the recovery of
/// one broken function); `BrokenName` names the function the element broke,
/// when the verifier's message names one.
Error runFunctionPipeline(StringRef Stage, Module &M, StringRef Pipeline,
                          std::vector<StageReport> &Reports,
                          CxxFlavor Flavor,
                          const StringSet<> *UserNames,
                          bool Guarded,
                          function_ref<void(size_t, llvm::Function &)>
                              AfterRun = nullptr,
                          ArrayRef<llvm::Function *> Only = {},
                          std::string *BrokenName = nullptr) {
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
      FPM.addPass(UncxxPass(Flavor, UserNames));
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
      if (!F.isDeclaration() && (Only.empty() || llvm::is_contained(Only, &F)))
        Functions.push_back(&F);
    for (llvm::Function *F : Functions) {
      if (!Guarded && Only.empty()) {
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
      // The function alone is checked: a pass that breaks it is rolled back
      // to the state before that pass, which is what the guarded run of the
      // whole stage did for it.
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
  std::string VerifyMessage;
  bool Valid = true;
  {
    raw_string_ostream OS(VerifyMessage);
    Valid = !verifyModule(M, &OS);
  }
  // The verifier names the function in the message it prints for the
  // checks that can fail here ("Basic Block in function 'f' does not have
  // terminator!"). The recovery of one broken function needs that name.
  if (BrokenName)
    *BrokenName = Valid ? "" : verifierFunctionName(VerifyMessage);
  if (!Valid)
    return createStringError("IR verifier failed after " + Stage.str() + ": " +
                             VerifyMessage);
  return Error::success();
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

/// The recovery of one broken function. The unguarded run left the module
/// invalid because a pass broke one function. Reset that function to the
/// stage input's version and rerun the elements up to the failing one for it
/// alone, so it reaches the failing element in the state it had there and
/// the passes of that element are rolled back where they break it. Every
/// other function keeps the result the unguarded run gave it, which is what
/// the guarded rerun of the whole stage produced for them too (a function
/// that never fails the verifier is left as the pass left it).
Error recoverBrokenFunction(StringRef Stage, Module &M, ArrayRef<StringRef> Elements,
                            size_t FailedIndex, StringRef BrokenName,
                            std::vector<StageReport> &Reports, CxxFlavor Flavor,
                            const StringSet<> *UserNames, StringRef InputText,
                            function_ref<Expected<std::unique_ptr<Module>>()> Rebuild) {
  // The stage input: the text the caller kept, or the module it can build
  // again. The input module has its own globals and declarations, so the
  // function body is copied with a map onto the live module's.
  std::unique_ptr<Module> Input;
  if (!InputText.empty()) {
    SMDiagnostic ParseErr;
    Input = parseIR(MemoryBufferRef(InputText, Stage), ParseErr, M.getContext());
    if (!Input)
      return createStringError("failed to parse the stage input for " +
                               Stage.str() + ": " + ParseErr.getMessage().str());
  } else if (Rebuild) {
    auto Rebuilt = Rebuild();
    if (!Rebuilt)
      return createStringError("failed to rebuild the stage input for " +
                               Stage.str() + ": " + toString(Rebuilt.takeError()));
    Input = std::move(*Rebuilt);
  } else {
    return createStringError("no stage input to recover " + Stage.str() + " from");
  }
  llvm::Function *Src = Input->getFunction(BrokenName);
  llvm::Function *Dst = M.getFunction(BrokenName);
  if (!Src || !Dst || Src->isDeclaration() || Dst->isDeclaration())
    return createStringError("the broken function " + BrokenName.str() +
                             " is not in the stage input");
  if (Src->arg_size() != Dst->arg_size())
    return createStringError("the broken function " + BrokenName.str() +
                             " has another signature in the stage input");
  ValueToValueMapTy VMap;
  // The input's globals and declarations point at the live module's.
  for (GlobalValue &GV : Input->global_values())
    if (GlobalValue *Live = M.getNamedValue(GV.getName()))
      VMap[&GV] = Live;
  auto Dest = Dst->arg_begin();
  for (Argument &A : Src->args())
    VMap[&A] = &*Dest++;
  Dst->copyAttributesFrom(Src);
  Dst->deleteBody();
  SmallVector<ReturnInst *, 8> Returns;
  // The input is another module, so the copy is a cross-module clone: the
  // value map points the input's globals and declarations at the live
  // module's.
  CloneFunctionInto(Dst, Src, VMap,
                    CloneFunctionChangeType::DifferentModule, Returns);
  // The elements before the failing one, and the failing one itself, run
  // for that function alone. The module passes in between are left alone:
  // they already ran over the whole module, and running them again would
  // change the functions they are not supposed to change.
  for (size_t I = 0; I <= FailedIndex && I < Elements.size(); ++I) {
    StringRef Element = Elements[I];
    if (!Element.starts_with("function("))
      continue;
    if (Error E = runFunctionPipeline(Stage, M, Element, Reports, Flavor,
                                      UserNames, /*Guarded=*/true, nullptr,
                                      {Dst}, nullptr))
      return E;
  }
  return Error::success();
}

Error runPipelineText(
    StringRef Stage, std::unique_ptr<Module> &M, StringRef Pipeline,
    std::vector<StageReport> &Reports, CxxFlavor Flavor = CxxFlavor::Auto,
    const StringSet<> *UserNames = nullptr, StringRef InputText = "",
    function_ref<Expected<std::unique_ptr<Module>>()> Rebuild = nullptr) {
  // Validate the whole pipeline first: unknown pass names are configuration
  // errors, not recoverable failures of a particular function or module.
  PassBuilder PB;
  registerRevenantPasses(PB);
  ModulePassManager Parsed;
  if (Error E = PB.parsePassPipeline(Parsed, Pipeline))
    return E;
  auto Elements = pipelineElements(Pipeline);
  auto RunAll = [&](bool Guarded) -> Error {
    // Both element kinds verify the module when they succeed; verify at the
    // end only when the last element left it unverified (a rolled-back
    // module pass, or an empty pipeline).
    bool Verified = false;
    for (size_t I = 0; I < Elements.size(); ++I) {
      StringRef Element = Elements[I];
      if (Element.starts_with("function(")) {
        // The unguarded run names the function it broke, when the
        // verifier's message names one.
        std::string BrokenName;
        if (Error E = runFunctionPipeline(Stage, *M, Element, Reports,
                                          Flavor, UserNames, Guarded, nullptr,
                                          {}, Guarded ? nullptr : &BrokenName)) {
          if (Guarded || BrokenName.empty())
            return E;
          // One function was broken by a pass: recover it alone and keep
          // the element. The recovery resets the function to the stage
          // input's version and reruns the elements up to this one for it,
          // which is what the guarded rerun of the whole stage did for it,
          // without the guarded rerun's cost (a function clone and a
          // verify per function per pass). The error of the failed run is
          // consumed here: the recovery replaces its result.
          std::string Message = toString(std::move(E));
          if (Error R = recoverBrokenFunction(Stage, *M, Elements, I,
                                              BrokenName, Reports, Flavor,
                                              UserNames, InputText, Rebuild)) {
            // The function could not be rebuilt from the stage input: the
            // stage falls back to the old path (restore the stage input,
            // rerun the whole stage guarded).
            errs() << "[failed] " << Stage << ": recovery of " << BrokenName
                   << " failed: " << toString(std::move(R)) << "\n";
            return createStringError("IR verifier failed after " +
                                     Stage.str() + ": " + Message);
          }
          record(Reports, Stage, "recovered", BrokenName + ": " + Message);
          errs() << "[recovered] " << Stage << ": " << BrokenName << ": "
                 << Message << "\n";
          Verified = true;
          continue;
        }
        Verified = true;
        continue;
      }
      FunctionStageTrace Trace(Stage, Element, "module");
      // Unguarded: a failing module pass aborts the run, and the caller
      // restores the stage input (reparsed text or the upfront clone) and
      // reruns guarded. Guarded: snapshot the module so a failing pass is
      // rolled back and the rest of the pipeline still runs.
      const bool Snapshot = Guarded;
      std::unique_ptr<Module> Saved;
      if (Snapshot)
        Saved = CloneModule(*M);
      // Analysis managers die inside runModulePass before the module is
      // swapped.
      Error E = runModulePass(*M, Element);
      Verified = !E;
      if (E) {
        if (!Snapshot)
          return E;
        std::string Message = toString(std::move(E));
        M.swap(Saved);
        record(Reports, Stage, "failed",
               "module: " + Element.str() + ": " + Message);
        errs() << "[failed] " << Stage << ": module: " << Element << ": "
               << Message << "\n";
      }
    }
    return Verified ? Error::success() : verifyStage(Stage, *M);
  };
  // Insurance is paid only on failure. When the caller already holds the
  // stage input as text (the common path), restore by reparsing it; the
  // old premium was one module clone per stage plus one function clone
  // and one full verify per function, which cost more than the passes.
  // A caller that can rebuild the input passes `Rebuild`. Without either,
  // fall back to one upfront module clone per stage.
  std::unique_ptr<Module> Saved;
  if (InputText.empty() && !Rebuild)
    Saved = CloneModule(*M);
  if (Error E = RunAll(/*Guarded=*/false)) {
    errs() << "[failed] " << Stage << ": " << toString(std::move(E)) << "\n";
    if (Saved) {
      M.swap(Saved);
    } else if (InputText.empty()) {
      auto Rebuilt = Rebuild();
      if (!Rebuilt)
        return createStringError("failed to restore stage input for " +
                                 Stage.str() + ": " +
                                 toString(Rebuilt.takeError()));
      M = std::move(*Rebuilt);
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

/// ir-final, the part the ir-final artifact and the source share:
/// deoptimization, then type recovery. `revenant-unhotcold` is a module pass
/// (the cold-chunk search looks at sibling functions), so it sits between
/// the two function blocks. `revenant-rustblocks` and `revenant-gscookie` run
/// at the end of the deopt block: both change the CFG or the instruction set,
/// and both have to run before `revenant-structure` (in both storage tails),
/// which names the blocks and records the region indices of the final CFG.
std::string deoptPipeline(bool IncludeUncxx) {
  return formatv("function({0}{1},revenant-rustblocks,revenant-gscookie),"
                 "revenant-unhotcold,function(revenant-types)",
                 IncludeUncxx ? "revenant-uncxx," : "", UnoptPipeline)
      .str();
}

/// The storage the ir-final artifact ends with for clang, MSVC and GCC
/// binaries: one alloca per frame object even where frame addresses escape
/// (the object bounds are inferred, so only ir-final takes this), then one
/// stack slot per parameter, phi web and multi-return value, as clang lays
/// out an unoptimized function, then the control-flow shape.
std::string finalStoragePipeline(bool Patterns) {
  return formatv("function(revenant-frame-slots,revenant-locals-to-slots,"
                 "revenant-structure{0})",
                 Patterns ? ",revenant-patterns" : "")
      .str();
}

/// The storage the source emitters read: SSA-out locals, which become the
/// C/C++/Rust variables, then the control-flow shape.
std::string sourceStoragePipeline(bool Patterns) {
  return formatv("function(revenant-ssaout,revenant-structure{0})",
                 Patterns ? ",revenant-patterns" : "")
      .str();
}

/// A module after the shared ir-final passes, as bitcode plus the name
/// counters bitcode drops, so another LLVMContext can rebuild it exactly.
/// Later passes name new values from those counters (`bb_X558`).
struct ModuleSnapshot {
  std::string Bitcode;
  uint32_t GlobalCounter = 0;
  std::vector<std::pair<std::string, uint32_t>> Counters;
};

ModuleSnapshot snapshotModule(Module &M) {
  ModuleSnapshot S;
  S.GlobalCounter = lastUnique(M.getValueSymbolTable());
  for (llvm::Function &F : M)
    if (!F.isDeclaration())
      S.Counters.emplace_back(F.getName().str(),
                              lastUnique(*F.getValueSymbolTable()));
  raw_string_ostream OS(S.Bitcode);
  // Use-list order drives pass iteration over users; keep it.
  WriteBitcodeToFile(M, OS, /*ShouldPreserveUseListOrder=*/true);
  OS.flush();
  return S;
}

Expected<std::unique_ptr<Module>> restoreSnapshot(const ModuleSnapshot &S,
                                                  LLVMContext &Ctx) {
  auto MOrErr =
      parseBitcodeFile(MemoryBufferRef(S.Bitcode, "ir-final-input"), Ctx);
  if (!MOrErr)
    return MOrErr.takeError();
  Module &M = **MOrErr;
  lastUnique(M.getValueSymbolTable()) = S.GlobalCounter;
  for (const auto &[Name, Counter] : S.Counters)
    if (llvm::Function *F = M.getFunction(Name))
      lastUnique(*F->getValueSymbolTable()) = Counter;
  return MOrErr;
}

/// The ir-final storage passes and print on a worker thread, in their own
/// LLVMContext, while the main thread runs the source storage passes and
/// the source emit on the shared module. The two never touch one context.
/// The snapshot must outlive the worker.
class FinalStorageWorker {
public:
  void start(const ModuleSnapshot &Snapshot, std::string Pipeline) {
    Input = &Snapshot;
    Thread = std::thread([this, Pipeline = std::move(Pipeline)] {
      auto Start = StageClock::now();
      run(Pipeline);
      Micros = std::chrono::duration_cast<std::chrono::microseconds>(
                   StageClock::now() - Start)
                   .count();
    });
  }
  bool started() const { return Thread.joinable(); }
  /// Waits for the worker. Its stage records go to `Stages`, then one
  /// record with the worker's own duration.
  Expected<std::string> take(std::vector<StageReport> &Stages) {
    Thread.join();
    Stages.insert(Stages.end(), Reports.begin(), Reports.end());
    if (!Err.empty())
      return createStringError("ir-final storage: " + Err);
    Stages.push_back({"ir-final", "ok", "locals-to-slots + structure (worker)",
                      "", static_cast<uint64_t>(Micros)});
    return std::move(Text);
  }
  ~FinalStorageWorker() {
    if (Thread.joinable())
      Thread.join();
  }

private:
  void run(StringRef Pipeline) {
    LLVMContext Ctx;
    auto M = restoreSnapshot(*Input, Ctx);
    if (!M) {
      Err = toString(M.takeError());
      return;
    }
    auto Rebuild = [&]() { return restoreSnapshot(*Input, Ctx); };
    if (Error E = runPipelineText("ir-final", *M, Pipeline, Reports,
                                  CxxFlavor::Auto, nullptr, "", Rebuild)) {
      Err = toString(std::move(E));
      return;
    }
    Text = moduleText(**M);
  }

  const ModuleSnapshot *Input = nullptr;
  std::thread Thread;
  std::vector<StageReport> Reports;
  std::string Text;
  std::string Err;
  int64_t Micros = 0;
};

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
Expected<EmitResult> emitClangSource(Module &M, bool Cpp, bool UserBodiesOnly) {
  clangx::ClangFrontendOptions Opts;
  Opts.Lang = Cpp ? "cpp" : "c";
  Opts.UserBodiesOnly = UserBodiesOnly;
  auto TOrErr = clangx::createASTContext(M, Opts);
  if (!TOrErr)
    return TOrErr.takeError();
  std::unique_ptr<clangx::TranslationUnit> T = std::move(*TOrErr);
  auto Types = clangx::createTypeBuilder(T->frontend());
  auto Stmts = clangx::createStmtBuilder(T->frontend(), *Types);
  auto Exprs = clangx::createExprBuilder(T->frontend(), *Types);
  clangx::fillTranslationUnit(*T, M, *Types, *Stmts, *Exprs,
                              Opts.UserBodiesOnly);
  EmitResult R;
  raw_string_ostream OS(R.Text);
  SmallVector<StringRef, 4> Inc = {"stdint.h", "stddef.h", "stdbool.h"};
  // The emitter spells `llvm.x86.int 0x29`/`llvm.trap` as the MSVC
  // intrinsics __fastfail/__debugbreak; those live in <intrin.h>, which
  // only the MSVC ABI family has, so pull it in there (a bare prototype
  // does not link: LNK2019). MinGW targets keep the plain three.
  if (llvm::Triple(M.getTargetTriple()).isKnownWindowsMSVCEnvironment())
    Inc.push_back("intrin.h");
  // The frontend asks for a header when it lowers an IR form to a library
  // call (e.g. `llvm.memset` -> `memset`, which needs <string.h>).
  clangx::printTranslationUnit(T->tu(), OS, Inc,
                               T->frontend().requestedIncludes());
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
  // The listing feeds the asm emit and the artifact. The formatting reuses
  // the function the materialize call already decoded, so the listing costs
  // one decode per function instead of two.
  const bool WantAsmText =
      Opts.Emit == EmitKind::Asm || !Opts.ArtifactBase.empty();
  std::string AsmText;
  size_t InstrCount = 0;
  std::vector<Va> Ready;
  int64_t AsmDisasmMicros = 0;
  for (Va T : Todo) {
    auto FOrErr = materialize(T);
    if (!FOrErr) {
      std::string Note =
          formatv("0x{0:x}: {1}", T.Value, toString(FOrErr.takeError())).str();
      record(R.Stages, "asm", "failed", Note);
      errs() << "[failed] asm: " << Note << "\n";
      continue;
    }
    if (WantAsmText) {
      auto DisasmStart = StageClock::now();
      AsmText += formatAsm(*FOrErr);
      AsmDisasmMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                             StageClock::now() - DisasmStart)
                             .count();
    }
    Ready.push_back(T);
    InstrCount += FOrErr->Instrs.size();
    if (Opts.Emit == EmitKind::Asm)
      for (const revenant::Instr &I : FOrErr->Instrs)
        R.LineVas.push_back(
            {static_cast<uint32_t>(R.LineVas.size() + 1), {I.Addr}});
  }
  Todo = std::move(Ready);
  reportArtifactTiming("asm-disasm", AsmDisasmMicros);
  record(
      R.Stages, "asm", "ok",
      formatv("{0} instructions across {1} functions", InstrCount, Todo.size())
          .str());
  if (Opts.Emit == EmitKind::Asm)
    R.Text = AsmText;

  // Each stage below runs only when a later artifact was asked for: `break`
  // leaves the block once the requested emit kind is produced.
  std::string RawText, CleanText, FinalText;
  // The worker reads the snapshot, so it is declared after it: on an early
  // return the worker joins before the snapshot is freed.
  ModuleSnapshot FinalInput;
  FinalStorageWorker FinalWorker;
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
    // has no hosted entry, and a DLL's traced entry is DllMain, not main.
    if (Classification.UserEntry && (Out == Lang::C || Out == Lang::Cpp) &&
        pe().Exports.empty()) {
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
    auto M = moduleFor(Ctx, Out, Compiler.Producer);
    // Decode the image's EH metadata once, then associate each lifted function
    // with its table entry. The EH vector owns these entries through the loop.
    std::vector<FunctionEH> EHFunctions = decodeEHTables(pe());
    DenseSet<uint64_t> FuncletAddresses;
    DenseMap<uint64_t, uint64_t> FuncletAliases;
    // Parents whose EH handlers resolved to a funclet, with the funclet each
    // handler inlines (one entry per handler, so a funclet can repeat).
    DenseMap<uint64_t, SmallVector<uint64_t, 4>> OwnerFunclets;
    // EH map targets can point at the entry instruction inside a discovered
    // funclet, rather than at its containing Program function's start.
    auto collectFunclet = [&](Va Handler, Va Owner) {
      auto Exact = Prog.Functions.find(Handler);
      if (Exact != Prog.Functions.end() && Handler != Owner) {
        FuncletAddresses.insert(Handler.Value);
        FuncletAliases[Handler.Value] = Handler.Value;
        OwnerFunclets[Owner.Value].push_back(Handler.Value);
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
        OwnerFunclets[Owner.Value].push_back(Start.Value);
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
    // Serial lifts the funclets in this order, before any other function.
    const std::vector<uint64_t> FuncletOrder(FuncletAddresses.begin(),
                                             FuncletAddresses.end());
    // A funclet takes the parent frame in rdx and returns its continuation
    // in rax.
    lift::Signature FuncletSig;
    FuncletSig.Params.push_back(
        {lift::ParamLoc::reg(llvm::X86::RDX), 64, false, std::string("frame")});
    FuncletSig.Ret = lift::RetLoc{false, 64};
    // Funclet lifting and inlining work on any module: the serial path runs
    // them on M, the parallel path inside every shard that holds a parent.
    // `SigMap` is a private copy of *Sigs: the funclet signature replaces
    // the recovered one only while that funclet itself is lifted, so calls
    // between funclets bind to their recovered signatures.
    auto liftFunclet = [&](Module &Mod, const Disasm &D,
                           lift::Signatures &SigMap,
                           uint64_t Address) -> Expected<llvm::Function *> {
      Va At(Address);
      std::optional<lift::Signature> Recovered;
      if (auto It = SigMap.Funcs.find(At); It != SigMap.Funcs.end())
        Recovered = std::move(It->second);
      SigMap.Funcs[At] = FuncletSig;
      auto L = lift::liftFunction(Mod, Prog, Prog.Functions.at(At), D, &SigMap,
                                  /*TM=*/nullptr,
                                  /*RunPasses=*/false, /*EH=*/nullptr);
      if (Recovered)
        SigMap.Funcs[At] = std::move(*Recovered);
      else
        SigMap.Funcs.erase(At);
      if (!L) {
        std::string Message = toString(L.takeError());
        return createStringError(
            inconvertibleErrorCode(), "failed to lift EH funclet 0x%llx: %s",
            static_cast<unsigned long long>(Address), Message.c_str());
      }
      return L->Fn;
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
    // `AfterParent` runs after each parent that has handler blocks.
    auto inlineFunclets = [&](Module &Mod,
                              DenseMap<uint64_t, llvm::Function *> &Funclets,
                              bool EraseUnused,
                              function_ref<void(llvm::Function &)> AfterParent =
                                  nullptr) -> Error {
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
            // Serial lifts every funclet; a shard lifts those of its parents.
            if (!Funclet)
              return createStringError(
                  inconvertibleErrorCode(),
                  "EH funclet 0x%llx of %s was not lifted",
                  static_cast<unsigned long long>(Alias->second),
                  Parent.getName().str().c_str());
            if (Funclet->arg_size() != 1 || BB.empty())
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
          if (AfterParent && !HandlerBlocks.empty())
            AfterParent(Parent);
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
    // EH parents spread over the shards like any other function. A shard
    // lifts the funclets its parents use and inlines them before cleanup,
    // as the serial path does on M; a funclet shared by parents in several
    // shards is lifted in each (inlining copies its body anyway), and the
    // merge keeps one copy of the funclet and of its globals.
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
    // Builds the merged module from the shards' bitcode (parallel path). It
    // stays set through ir-clean, whose failure restore it is.
    std::function<Expected<std::unique_ptr<Module>>()> MergeShards;
    {
      // Parallel lift+clean is the default source path; REVENANT_PARALLEL_CLEAN=0
      // restores the serial algorithm (useful for debugging or artifact parity).
      const char *PC = std::getenv("REVENANT_PARALLEL_CLEAN");
      unsigned Threads = std::thread::hardware_concurrency();
      if (const char *V = std::getenv("REVENANT_THREADS"))
        Threads = std::max(1u, static_cast<unsigned>(std::atoi(V)));
      bool Wanted = (!PC || StringRef(PC) != "0") && Threads > 1 &&
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
        // Shards keep Todo order inside, like the serial module. An EH
        // parent costs its own size plus one inlined funclet body per
        // handler, plus the lift of each funclet its shard lacks so far.
        // An inlined body is raw lifted IR: cloning it and cleaning the
        // grown parent measured about five times the per-instruction cost
        // of a plain lift and clean (fmt-cl-o2gl: one parent, 11k inlined
        // instructions, 7 s).
        constexpr size_t InlinedWeight = 5;
        std::vector<std::vector<size_t>> Shards(Threads);
        std::vector<size_t> Loads(Threads, 0);
        std::vector<DenseSet<uint64_t>> ShardFunclets(Threads);
        auto sizeOf = [&](uint64_t Address) {
          return Prog.Functions.at(Va(Address)).InstrRvas.size();
        };
        std::vector<size_t> Cost(Todo.size());
        for (size_t Idx = 0; Idx < Todo.size(); ++Idx) {
          Cost[Idx] = sizeOf(Todo[Idx].Value);
          if (auto It = OwnerFunclets.find(Todo[Idx].Value);
              It != OwnerFunclets.end())
            for (uint64_t Funclet : It->second)
              Cost[Idx] += InlinedWeight * sizeOf(Funclet);
        }
        std::vector<size_t> Order(Todo.size());
        std::iota(Order.begin(), Order.end(), size_t(0));
        llvm::stable_sort(
            Order, [&](size_t A, size_t B) { return Cost[A] > Cost[B]; });
        for (size_t Idx : Order) {
          if (FuncletAddresses.contains(Todo[Idx].Value))
            continue;
          auto Owned = OwnerFunclets.find(Todo[Idx].Value);
          auto liftCost = [&](size_t S) {
            size_t Extra = 0;
            if (Owned != OwnerFunclets.end()) {
              SmallVector<uint64_t, 8> Seen;
              for (uint64_t Funclet : Owned->second)
                if (!ShardFunclets[S].contains(Funclet) &&
                    !llvm::is_contained(Seen, Funclet)) {
                  Seen.push_back(Funclet);
                  Extra += sizeOf(Funclet);
                }
            }
            return Extra;
          };
          size_t Best = 0;
          size_t BestLoad = Loads[0] + liftCost(0);
          for (size_t S = 1; S < Threads; ++S) {
            size_t Load = Loads[S] + liftCost(S);
            if (Load < BestLoad) {
              Best = S;
              BestLoad = Load;
            }
          }
          Shards[Best].push_back(Idx);
          Loads[Best] = BestLoad + Cost[Idx];
          if (Owned != OwnerFunclets.end())
            ShardFunclets[Best].insert(Owned->second.begin(),
                                       Owned->second.end());
        }
        for (std::vector<size_t> &Shard : Shards)
          llvm::sort(Shard);
        // Every funclet is lifted at least once, also one that no parent
        // inlines (its owners are funclets themselves). Its home shard, the
        // first that lifts it, keeps the copy that survives the merge.
        DenseMap<uint64_t, unsigned> FuncletHome;
        for (uint64_t Address : FuncletOrder) {
          unsigned Home = 0;
          while (Home < Threads && !ShardFunclets[Home].contains(Address))
            ++Home;
          if (Home == Threads) {
            Home = 0;
            for (unsigned S = 1; S < Threads; ++S)
              if (Loads[S] < Loads[Home])
                Home = S;
            ShardFunclets[Home].insert(Address);
            Loads[Home] += sizeOf(Address);
          }
          FuncletHome[Address] = Home;
        }
        // The EH personality is a module-level name: the lifter creates the
        // declaration lazily, and a function can carry the same name (a
        // library match names the personality thunk after the runtime symbol
        // it forwards to). The first creator keeps the plain name and the
        // other is renamed to `.decl`, so the lift order decides which is
        // which. Serial lifts in program order: when an earlier function
        // owns the name, its definition holds it and the EH conversion does
        // not fire for the functions lifted after it. The shards lift out of
        // program order, so they need the owner slot to keep the same
        // module state.
        StringMap<size_t> PersonalityOwner;
        for (size_t I = 0; I < Todo.size(); ++I) {
          std::string Name = lift::definitionName(Prog.Functions.at(Todo[I]));
          for (EHModel Model :
               {EHModel::MSVC, EHModel::SEH, EHModel::Itanium, EHModel::Rust}) {
            StringRef Personality = personalityName(Model);
            if (Personality.empty() || Name != Personality)
              continue;
            PersonalityOwner.try_emplace(Personality.str(), I);
          }
        }
        // The personality names this program uses: every shard declares them
        // (see the lift wave) so that the calls bind the way they do in
        // serial, and the merge puts each declaration at the slot of the
        // first function whose EH conversion fired.
        std::vector<std::string> PersonalityNames;
        for (const FunctionEH &EH : EHFunctions) {
          StringRef Name = personalityName(EH.Model);
          if (Name.empty() || llvm::is_contained(PersonalityNames, Name.str()))
            continue;
          PersonalityNames.push_back(Name.str());
        }
        std::vector<unsigned> ShardOf(Todo.size(), ~0u);
        for (unsigned S = 0; S < Shards.size(); ++S)
          for (size_t Idx : Shards[S])
            ShardOf[Idx] = S;
        if (Compiler.Producer == revenant::Compiler::Rustc)
          for (FunctionEH &EH : EHFunctions)
            EH.Model = EHModel::Rust;
        struct ShardState {
          std::unique_ptr<LLVMContext> Ctx;
          std::unique_ptr<Module> Mod;
          // (Todo index, globals that lift created) in lift order.
          std::vector<std::pair<size_t, SmallVector<llvm::GlobalVariable *, 4>>>
              Created;
          // (FuncletOrder index, globals that funclet's lift created).
          std::vector<std::pair<size_t, SmallVector<llvm::GlobalVariable *, 4>>>
              FuncletCreated;
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
        // Shared with MergeShards, which outlives this block.
        auto StateOwner = std::make_shared<std::vector<ShardState>>(Threads);
        std::vector<ShardState> &State = *StateOwner;
        auto runWave = [&](auto &&Body) {
          std::vector<std::thread> Workers;
          for (unsigned S = 0; S < State.size(); ++S)
            if (!Shards[S].empty() || !ShardFunclets[S].empty())
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
        // Each personality name binds to whatever `getFunction` returns at
        // conversion time, and serial can already have a typed callee
        // declaration for it: a funclet or an earlier-lifted function that
        // calls the personality thunk declares the name before the first
        // EH conversion runs. Only when no such caller exists does the
        // conversion invent `declare i32 @P(...)` itself. The shards
        // pre-declare each name up front, so they must pick the same
        // shape. A caller after the first conversion changes nothing: it
        // reuses the vararg stub serial already created, and the thunk's
        // own lift renames either stub to `.decl` all the same.
        StringSet<> TypedPersonalityDecls;
        {
          DenseMap<uint64_t, std::string> OwnerNameByVa;
          for (const auto &Entry : PersonalityOwner)
            OwnerNameByVa[Todo[Entry.second].Value] = Entry.getKey().str();
          StringMap<size_t> FirstBinder;
          for (size_t Idx = 0; Idx < Todo.size(); ++Idx) {
            const FunctionEH *EH = nullptr;
            for (const FunctionEH &Entry : EHFunctions)
              if (Entry.Function == Todo[Idx]) {
                EH = &Entry;
                break;
              }
            if (!EH || EH->TryRegions.empty() ||
                (EH->Handlers.empty() && EH->Cleanups.empty()))
              continue;
            std::string Name = personalityName(EH->Model).str();
            if (Name.empty())
              continue;
            auto Owner = PersonalityOwner.find(Name);
            if (Owner != PersonalityOwner.end() && Owner->second <= Idx)
              continue;
            FirstBinder.try_emplace(std::move(Name), Idx);
          }
          auto noteCaller = [&](const FuncRecord &F,
                                std::optional<size_t> Slot) {
            for (Va Target : F.CallTargets) {
              auto It = OwnerNameByVa.find(Target.Value);
              if (It == OwnerNameByVa.end())
                continue;
              auto Binder = FirstBinder.find(It->second);
              if (Binder != FirstBinder.end() &&
                  (!Slot || *Slot < Binder->second))
                TypedPersonalityDecls.insert(It->second);
            }
          };
          for (uint64_t Address : FuncletOrder)
            if (auto It = Prog.Functions.find(Va(Address));
                It != Prog.Functions.end())
              noteCaller(It->second, std::nullopt);
          for (size_t Idx = 0; Idx < Todo.size(); ++Idx)
            if (!FuncletAddresses.contains(Todo[Idx].Value))
              noteCaller(Prog.Functions.at(Todo[Idx]), Idx);
        }
        // Wave 1: lift.
        runWave([&](unsigned S) {
          ShardState &St = State[S];
          auto ThreadDis = Disasm::create(*Pe->Img);
          if (!ThreadDis) {
            St.Err = toString(ThreadDis.takeError());
            return;
          }
          St.Ctx = std::make_unique<LLVMContext>();
          St.Mod = moduleFor(*St.Ctx, Out, Compiler.Producer);
          St.KindSeen = kindCount(St);
          // The lifter creates the EH personality declaration lazily, at
          // the first function whose EH conversion fires, and serial has it
          // for every function lifted after that. A shard that lifts such
          // a function without the declaration binds the calls to the
          // callee instead of the personality, and the extra declaration
          // takes the name the definition wants. Create it up front; the
          // merge gives it the serial creation slot. When an earlier lift
          // already declares the name (a funclet or an earlier function
          // that calls the personality thunk), serial binds the typed
          // callee declaration instead: claim it with the owner's own
          // signature so call sites and personalities resolve the same
          // way, and the thunk's lift reuses it unchanged.
          for (const std::string &Name : PersonalityNames) {
            if (St.Mod->getFunction(Name))
              continue;
            if (TypedPersonalityDecls.contains(Name)) {
              auto Owner = PersonalityOwner.find(Name);
              lift::claimDefinition(*St.Mod, Name,
                                    Sigs->of(Todo[Owner->second]));
              continue;
            }
            St.Mod->getOrInsertFunction(
                Name, llvm::FunctionType::get(
                          llvm::Type::getInt32Ty(St.Mod->getContext()),
                          true));
          }
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
          // Serial lifts every funclet first, in FuncletOrder. Lift the ones
          // this shard's parents inline and claim the rest. A funclet lift
          // gets slot 0 and a sequence that starts at its order index, so
          // the minimum over shards is the serial creation order.
          std::optional<lift::Signatures> FuncletSigs;
          for (size_t K = 0; K < FuncletOrder.size(); ++K) {
            uint64_t Address = FuncletOrder[K];
            if (!ShardFunclets[S].contains(Address)) {
              lift::claimDefinition(
                  *St.Mod,
                  lift::definitionName(Prog.Functions.at(Va(Address))),
                  &FuncletSig);
              St.FnSeen = St.Mod->size();
              continue;
            }
            if (!FuncletSigs)
              FuncletSigs.emplace(*Sigs);
            size_t Before = St.Mod->global_size();
            auto F = liftFunclet(*St.Mod, *ThreadDis, *FuncletSigs, Address);
            if (!F) {
              St.Err = toString(F.takeError());
              return;
            }
            St.Funclets[Address] = *F;
            SmallVector<llvm::GlobalVariable *, 4> New;
            size_t I = 0;
            for (llvm::GlobalVariable &G : St.Mod->globals())
              if (I++ >= Before)
                New.push_back(&G);
            St.FuncletCreated.emplace_back(K, std::move(New));
            St.FnSeq = uint64_t(K) << 32;
            noteNewFunctions(St, 0, 0, "");
          }
          for (size_t Idx : Shards[S]) {
            Va T = Todo[Idx];
            claimForeignBefore(Idx);
            const FunctionEH *EH = nullptr;
            auto EHIt = llvm::find_if(EHFunctions,
                                      [T](const FunctionEH &Entry) {
                                        return Entry.Function == T;
                                      });
            if (EHIt != EHFunctions.end())
              EH = &*EHIt;
            // An earlier function owns the personality name: serial has its
            // definition under that name when this function is lifted, so
            // the EH conversion does not fire here. Drop the EH entry to
            // keep the shard's module in the serial state.
            if (EH) {
              auto Owner =
                  PersonalityOwner.find(personalityName(EH->Model).str());
              if (Owner != PersonalityOwner.end() && Owner->second < Idx)
                EH = nullptr;
            }
            size_t Before = St.Mod->global_size();
            auto LiftStart = StageClock::now();
            auto L = lift::liftFunction(*St.Mod, Prog, Prog.Functions.at(T),
                                        *ThreadDis, Sigs.get(),
                                        /*TM=*/nullptr,
                                        /*RunPasses=*/false, EH);
            uint64_t FailedLiftMicros = 0;
            if (!L)
              FailedLiftMicros =
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      StageClock::now() - LiftStart)
                      .count();
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
                  formatv("0x{0:x}: {1} (liftFunction {2:F2} ms)",
                          T.Value, Message, FailedLiftMicros / 1000.0)
                      .str();
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
          if (!St.Funclets.empty()) {
            // Serial inlines parent by parent in module (= Todo) order.
            // Funclets are erased only once no shard uses them; decided
            // after the wave.
            auto AfterParent = [&](llvm::Function &Parent) {
              auto VA = ir::getFunctionVA(Parent);
              noteNewFunctions(
                  St, 0, Todo.size() + 1 + (VA ? TodoSlot.lookup(*VA) : 0), "");
            };
            if (Error E = inlineFunclets(*St.Mod, St.Funclets,
                                         /*EraseUnused=*/false, AfterParent))
              St.Err = toString(std::move(E));
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
        // funclets and their declarations everywhere. A funclet still in
        // use survives once, from its home shard; the other copies become
        // declarations that the merge folds onto it.
        if (Why.empty() && !FuncletAddresses.empty()) {
          for (uint64_t Address : FuncletOrder) {
            std::string Name =
                lift::definitionName(Prog.Functions.at(Va(Address)));
            bool Used = false;
            for (ShardState &St : State)
              if (St.Mod)
                if (llvm::Function *F = St.Mod->getFunction(Name))
                  Used |= !F->use_empty();
            unsigned Home = FuncletHome.lookup(Address);
            for (unsigned S = 0; S < State.size(); ++S)
              if (State[S].Mod)
                if (llvm::Function *F = State[S].Mod->getFunction(Name)) {
                  if (!Used)
                    F->eraseFromParent();
                  else if (S != Home && !F->isDeclaration())
                    F->deleteBody();
                }
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
            auto Home = FuncletHome.find(Todo[Idx].Value);
            unsigned Owner =
                Home != FuncletHome.end() ? Home->second : ShardOf[Idx];
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
          // Cleanup is pass-major over the module's function list, and the
          // first function in that order to need an intrinsic creates it.
          // Put each shard's functions in serial order (lift-phase creation
          // keys across all shards) so creations attribute as in serial.
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
          // Per shard: each worker writes only its own module and context
          // and reads definitions elsewhere, which nothing writes now.
          std::vector<std::string> ShardWhy(State.size());
          runWave([&](unsigned S) {
            ShardState &St = State[S];
            if (!St.Mod)
              return;
            LLVMContext &C = *St.Ctx;
            for (llvm::Function &D : *St.Mod) {
              if (!D.isDeclaration() || D.isIntrinsic())
                continue;
              auto It = DefByName.find(D.getName());
              if (It == DefByName.end() || It->second == &D)
                continue;
              llvm::Function *Def = It->second;
              if (Def->arg_size() != D.arg_size()) {
                ShardWhy[S] = "signature mismatch for " + D.getName().str();
                return;
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
                ShardWhy[S] = "non-portable attribute on " + D.getName().str();
                return;
              }
              D.setAttributes(To);
            }
            std::vector<llvm::Function *> All;
            for (llvm::Function &F : *St.Mod)
              All.push_back(&F);
            llvm::stable_sort(All, [&](llvm::Function *A, llvm::Function *B) {
              return keyOf(A) < keyOf(B);
            });
            auto &List = St.Mod->getFunctionList();
            for (size_t I = All.size(); I-- > 0;)
              List.splice(List.begin(), List, All[I]->getIterator());
          });
          for (const std::string &W : ShardWhy)
            if (!W.empty() && Why.empty())
              Why = W;
        }
        // Give every global its serial name before cleanup runs: passes
        // derive local names from global names (simplifycfg's `<g>.mux`).
        // All globals come from lift, created in serial order funclets
        // first, then Todo order; replay LLVM's makeUniqueName over that
        // sequence (bump the module counter, append `.N` until free). A
        // funclet lifted in several shards made one copy of each global per
        // shard: the home copy takes the slot, and every copy the name.
        // `SharedGlobals` lists those names for the merge.
        StringSet<> SharedGlobals;
        if (Why.empty()) {
          std::vector<llvm::GlobalVariable *> Seq;
          // Seq position -> the copies in other shards.
          DenseMap<size_t, SmallVector<llvm::GlobalVariable *, 2>> Copies;
          std::vector<std::vector<std::pair<
              unsigned, const SmallVector<llvm::GlobalVariable *, 4> *>>>
              ByFunclet(FuncletOrder.size());
          std::vector<const SmallVector<llvm::GlobalVariable *, 4> *> ByIdx(
              Todo.size(), nullptr);
          for (unsigned S = 0; S < State.size(); ++S) {
            for (auto &[K, Globals] : State[S].FuncletCreated)
              ByFunclet[K].push_back({S, &Globals});
            for (auto &[Idx, Globals] : State[S].Created)
              ByIdx[Idx] = &Globals;
          }
          for (size_t K = 0; K < FuncletOrder.size() && Why.empty(); ++K) {
            unsigned Home = FuncletHome.lookup(FuncletOrder[K]);
            auto HomeIt = llvm::find_if(
                ByFunclet[K], [&](const auto &C) { return C.first == Home; });
            if (HomeIt == ByFunclet[K].end()) {
              Why = "funclet lifted without its home shard";
              break;
            }
            const auto &HomeGlobals = *HomeIt->second;
            for (const auto &[S, Globals] : ByFunclet[K]) {
              if (S == Home)
                continue;
              if (Globals->size() != HomeGlobals.size()) {
                Why = "funclet copies created different globals";
                break;
              }
              for (size_t I = 0; I < Globals->size(); ++I) {
                if (ir::getGlobalVA(*(*Globals)[I]) !=
                        ir::getGlobalVA(*HomeGlobals[I]) ||
                    (*Globals)[I]->getValueType()->getArrayNumElements() !=
                        HomeGlobals[I]->getValueType()->getArrayNumElements()) {
                  Why = "funclet copies created different globals";
                  break;
                }
                Copies[Seq.size() + I].push_back((*Globals)[I]);
              }
            }
            Seq.insert(Seq.end(), HomeGlobals.begin(), HomeGlobals.end());
          }
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
          for (size_t I = 0; I < Seq.size() && Why.empty(); ++I) {
            auto VA = ir::getGlobalVA(*Seq[I]);
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
            for (size_t I = 0; I < Seq.size(); ++I) {
              Seq[I]->setName("\1rvt." + Twine(I));
              for (llvm::GlobalVariable *Copy : Copies.lookup(I))
                Copy->setName("\1rvt." + Twine(I));
            }
            for (size_t I = 0; I < Seq.size() && Why.empty(); ++I) {
              Seq[I]->setName(GlobalOrder[I]);
              if (Seq[I]->getName() != GlobalOrder[I])
                Why = "global name collision on " + GlobalOrder[I];
              auto It = Copies.find(I);
              if (It == Copies.end())
                continue;
              SharedGlobals.insert(GlobalOrder[I]);
              for (llvm::GlobalVariable *Copy : It->second) {
                Copy->setName(GlobalOrder[I]);
                if (Copy->getName() != GlobalOrder[I])
                  Why = "global name collision on " + GlobalOrder[I];
              }
            }
          }
          SerialGlobalCounter = Counter;
        }
        // Wave 2: first clean block, then record what the merge restores.
        if (Why.empty()) {
          runWave([&](unsigned S) {
            ShardState &St = State[S];
            std::vector<StageReport> LocalReports;
            llvm::Function *LastFn = &St.Mod->getFunctionList().back();
            auto AfterRun = [&](size_t Pass, llvm::Function &F) {
              // Module::size() walks the function list; this callback runs
              // after every function in every pass. New functions append.
              if (&St.Mod->getFunctionList().back() == LastFn &&
                  kindCount(St) == St.KindSeen)
                return;
              noteNewFunctions(St, 1 + Pass, 0, F.getName());
              LastFn = &St.Mod->getFunctionList().back();
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
          // Shards are parsed into M's own context, so types, constants and
          // metadata are already shared: merging is splicing functions and
          // globals into the module and folding same-name declarations onto
          // one value. IRMover would remap every operand and rescan the whole
          // destination per shard, which made merging quadratic.
          MergeShards =
              [&Ctx, &Todo, &GlobalOrder, &SerialGlobalCounter, Out, StateOwner,
               SharedGlobals, this,
               PersonalityNames]() -> Expected<std::unique_ptr<Module>> {
            std::vector<ShardState> &State = *StateOwner;
            std::unique_ptr<Module> M = moduleFor(Ctx, Out, Compiler.Producer);
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
              for (llvm::GlobalVariable &G :
                   make_early_inc_range(Src.globals())) {
                if (llvm::GlobalValue *Old = M->getNamedValue(G.getName())) {
                  // Another shard's copy of a shared funclet global.
                  auto *OldG = dyn_cast<llvm::GlobalVariable>(Old);
                  if (!OldG || !SharedGlobals.contains(G.getName()) ||
                      OldG->getValueType() != G.getValueType())
                    return "duplicate global " + G.getName().str();
                  G.replaceAllUsesWith(OldG);
                  G.eraseFromParent();
                  continue;
                }
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
            for (ShardState &St : State) {
              if (St.Bitcode.empty())
                continue;
              auto ShardOrErr = parseBitcodeFile(
                  MemoryBufferRef(St.Bitcode, "clean-shard"), Ctx);
              if (!ShardOrErr)
                return createStringError("parse: " +
                                         toString(ShardOrErr.takeError()));
              std::string Err = mergeShard(**ShardOrErr);
              if (!Err.empty()) {
                releaseModule(**ShardOrErr);
                releaseModule(*M);
                return createStringError("merge: " + Err);
              }
            }
            for (ShardState &St : State)
              for (auto &[Name, Counter] : St.Counters)
                if (llvm::Function *F = M->getFunction(Name))
                  lastUnique(*F->getValueSymbolTable()) = Counter;
            // Linking re-points every use of a global value, so use lists come
            // out in link order. Passes such as revenant-lift-abi walk
            // F.users() and name what they create in the caller, so restore
            // the order: inside a caller, the worker's (= serial) order;
            // across callers, later-lifted first, since LLVM prepends uses.
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
              Rank[&F] =
                  It != TodoRank.end() ? It->second : Todo.size() + Ordinal;
              ++Ordinal;
            }
            auto key = [&](const Use &U) {
              auto *I = dyn_cast<Instruction>(U.getUser());
              if (!I)
                return std::make_tuple(1, size_t(0), uint32_t(0));
              auto P = PosOf.find(&U);
              return std::make_tuple(0, ~Rank.lookup(I->getFunction()),
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
            // The personality declaration is created at the first EH
            // conversion, so serial has it right after that function. The
            // shards declare it up front (the lifter creates it lazily and
            // a shard can miss the first conversion), so put it back at the
            // slot of the first function that uses it.
            for (const std::string &Name : PersonalityNames) {
              llvm::Function *Decl = M->getFunction(Name + ".decl");
              if (!Decl) {
                Decl = M->getFunction(Name);
                if (!Decl || !Decl->isDeclaration())
                  continue;
              }
              size_t First = SIZE_MAX;
              for (llvm::Function &F : *M) {
                if (!F.hasPersonalityFn())
                  continue;
                auto *P = dyn_cast<llvm::Function>(
                    F.getPersonalityFn()->stripPointerCasts());
                if (P != Decl)
                  continue;
                size_t Slot = Rank.lookup(&F);
                if (Slot < Todo.size())
                  First = std::min(First, Slot);
              }
              if (First == SIZE_MAX) {
                // No EH conversion fired: serial never created the
                // declaration. Drop it when nothing uses it.
                if (Decl->use_empty())
                  Decl->eraseFromParent();
                continue;
              }
              FnCreated[Decl] = {0, First + 1, UINT64_MAX};
            }
            std::vector<const llvm::Function *> LiftOrder;
            for (auto &[F, K] : FnCreated)
              LiftOrder.push_back(F);
            llvm::sort(LiftOrder,
                       [&](const llvm::Function *A, const llvm::Function *B) {
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
                    if (It == RankByName.end())
                      return createStringError("cleanup creation by unranked " +
                                               R.By);
                    minInto(FnCreated, F, {R.Phase, It->second, R.Seq});
                  }
            constexpr CKey Last{UINT64_MAX, UINT64_MAX, UINT64_MAX};
            std::vector<llvm::Function *> All;
            for (llvm::Function &F : *M)
              All.push_back(&F);
            llvm::stable_sort(All, [&](llvm::Function *A, llvm::Function *B) {
              return FnCreated.lookup_or(A, Last) <
                     FnCreated.lookup_or(B, Last);
            });
            auto &List = M->getFunctionList();
            for (size_t I = All.size(); I-- > 0;)
              List.splice(List.begin(), List, All[I]->getIterator());
            // Globals already carry their serial names. Restore the module
            // name counter serial leaves after lift and the global creation
            // order.
            lastUnique(M->getValueSymbolTable()) = SerialGlobalCounter;
            for (const std::string &Name : GlobalOrder) {
              llvm::GlobalVariable *G = M->getNamedGlobal(Name);
              if (!G)
                return createStringError("lost global " + Name);
              M->removeGlobalVariable(G);
              M->insertGlobalVariable(G);
            }
            return std::move(M);
          };
          if (auto Merged = MergeShards()) {
            M = std::move(*Merged);
            ParallelClean = true;
          } else {
            Why = toString(Merged.takeError());
            MergeShards = nullptr;
          }
        }
        if (!ParallelClean) {
          errs() << "[note] parallel clean fell back to serial: " << Why
                 << "\n";
          M = moduleFor(Ctx, Out, Compiler.Producer);
          CleanSlots.assign(Todo.size(), CleanSlot{});
          CleanRemainder.clear();
        }
      }
    }
    DenseMap<uint64_t, llvm::Function *> Funclets;
    if (!ParallelClean && !FuncletOrder.empty()) {
      lift::Signatures FuncletSigs = *Sigs;
      for (uint64_t Address : FuncletOrder) {
        auto F = liftFunclet(*M, *Dis, FuncletSigs, Address);
        if (!F)
          return F.takeError();
        Funclets[Address] = *F;
      }
    }
    // The user functions the uncxx pass may rewrite. The set holds names, not
    // pointers: a stage that fails is restored by reparsing its input text,
    // which builds a new module with new function objects, so a set of
    // pointers would stop matching (and match by accident when the
    // allocator reuses an address).
    StringSet<> UserFunctionNames;
    size_t OpaqueSites = 0;
    if (ParallelClean) {
      for (size_t Idx = 0; Idx < Todo.size(); ++Idx) {
        if (CleanSlots[Idx].Failed) {
          // The parallel wave ran before this loop; do not charge its
          // entire duration to the first function that happened to fail.
          R.Stages.push_back({"lift", "failed", CleanSlots[Idx].Note, "", 0});
          errs() << "[failed] lift: " << CleanSlots[Idx].Note << "\n";
        }
        OpaqueSites += CleanSlots[Idx].Opaque;
      }
      for (llvm::Function &F : *M)
        if (!F.isDeclaration() &&
            F.getFnAttribute("revenant.user").getValueAsString() == "true")
          UserFunctionNames.insert(F.getName());
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
        UserFunctionNames.insert(F->getName());
      OpaqueSites += L->AsmFallbacks.size();
    }
    if (!ParallelClean)
      if (Error E = inlineFunclets(*M, Funclets, /*EraseUnused=*/true))
        return E;
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
    // A failed parallel ir-clean is restored by merging the shards again.
    using ModuleBuilder = function_ref<Expected<std::unique_ptr<Module>>()>;
    if (Error E = runPipelineText(
            "ir-clean", M,
            ParallelClean ? CleanRemainder : cleanPipeline(structDIEnabled()),
            R.Stages, CxxFlavor::Auto, nullptr, RawText,
            MergeShards ? ModuleBuilder(MergeShards) : nullptr))
      return std::move(E);
    record(R.Stages, "ir-clean", "ok", "cleanup + idioms");
    // The serial path restores a failed ir-final from this text. The
    // parallel path rebuilds its input instead (merge, then ir-clean again),
    // so it prints the text only for the artifact.
    if (!ParallelClean || !Opts.ArtifactBase.empty()) {
      auto PrintStart = StageClock::now();
      CleanText = moduleText(*M);
      reportArtifactTiming(
          "print-ir-clean",
          std::chrono::duration_cast<std::chrono::microseconds>(
              StageClock::now() - PrintStart)
              .count());
    }
    if (Opts.Emit == EmitKind::IrClean) {
      R.Text = CleanText;
      break;
    }
    auto RebuildClean = [&]() -> Expected<std::unique_ptr<Module>> {
      auto Merged = MergeShards();
      if (!Merged)
        return Merged.takeError();
      std::unique_ptr<Module> Clean = std::move(*Merged);
      std::vector<StageReport> Repeated;
      if (Error E = runPipelineText("ir-clean", Clean, CleanRemainder,
                                    Repeated, CxxFlavor::Auto, nullptr, "",
                                    ModuleBuilder(MergeShards)))
        return std::move(E);
      return std::move(Clean);
    };

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
                                   &UserFunctionNames))
        return std::move(E);
      record(R.Stages, "ir-pre-like", "ok",
             "deopt + types + locals-to-slots + structure");
      R.Text = moduleText(*M);
      break;
    }

    // ---- ir-final ----------------------------------------------------------
    // Deoptimization and type recovery run once. The ir-final artifact then
    // keeps one stack slot per variable (locals-to-slots), the way clang
    // lays out unoptimized IR, and the source stage reads SSA-out locals.
    // When both are wanted, the ir-final storage runs on a worker thread
    // over a bitcode copy in its own context while the source stage runs
    // here. rustc's unoptimized IR keeps scalar locals in SSA values, and
    // SSA-out is the closer ir-final for it (rust-o0 0.656 against 0.614
    // with slots), so a rustc binary uses one storage for both.
    const bool SlotStorage = Compiler.Producer != revenant::Compiler::Rustc;
    const std::string FinalStorage = SlotStorage
                                         ? finalStoragePipeline(Opts.Patterns)
                                         : sourceStoragePipeline(Opts.Patterns);
    const std::string Deopt = deoptPipeline(Out == Lang::Cpp);
    if (auto E = runPipelineText(
            "ir-final", M, Deopt, R.Stages, uncxxFlavor(Compiler.Producer),
            &UserFunctionNames,
            ParallelClean ? StringRef() : StringRef(CleanText),
            ParallelClean ? ModuleBuilder(RebuildClean) : nullptr))
      return std::move(E);
    record(R.Stages, "ir-final", "ok", "Uncxx (C++ only) + unopt + types");
    // The input of both storage stages, which a failed storage stage is
    // restored from: the snapshot when there is one, else ir-clean again
    // through the shared passes.
    auto RebuildDeopt = [&]() -> Expected<std::unique_ptr<Module>> {
      if (!FinalInput.Bitcode.empty())
        return restoreSnapshot(FinalInput, Ctx);
      std::unique_ptr<Module> Input;
      if (ParallelClean) {
        auto Clean = RebuildClean();
        if (!Clean)
          return Clean.takeError();
        Input = std::move(*Clean);
      } else {
        SMDiagnostic ParseErr;
        Input = parseIR(MemoryBufferRef(CleanText, "ir-clean"), ParseErr, Ctx);
        if (!Input)
          return createStringError("failed to parse ir-clean: " +
                                   ParseErr.getMessage().str());
      }
      std::vector<StageReport> Repeated;
      if (Error E = runPipelineText(
              "ir-final", Input, Deopt, Repeated,
              uncxxFlavor(Compiler.Producer), &UserFunctionNames,
              ParallelClean ? StringRef() : StringRef(CleanText),
              ParallelClean ? ModuleBuilder(RebuildClean) : nullptr))
        return std::move(E);
      return std::move(Input);
    };
    if (Opts.Emit == EmitKind::IrFinal) {
      if (auto E = runPipelineText("ir-final", M, FinalStorage, R.Stages,
                                   CxxFlavor::Auto, nullptr, "",
                                   ModuleBuilder(RebuildDeopt)))
        return std::move(E);
      MergeShards = nullptr;
      record(R.Stages, "ir-final", "ok",
             SlotStorage ? "locals-to-slots + structure"
                         : "ssaout + structure");
      auto PrintStart = StageClock::now();
      FinalText = moduleText(*M);
      reportArtifactTiming(
          "print-ir-final",
          std::chrono::duration_cast<std::chrono::microseconds>(
              StageClock::now() - PrintStart)
              .count());
      R.Text = FinalText;
      break;
    }
    if (SlotStorage && !Opts.ArtifactBase.empty()) {
      auto SnapshotStart = StageClock::now();
      FinalInput = snapshotModule(*M);
      reportArtifactTiming(
          "snapshot-ir-final",
          std::chrono::duration_cast<std::chrono::microseconds>(
              StageClock::now() - SnapshotStart)
              .count());
      FinalWorker.start(FinalInput, FinalStorage);
    }

    // ---- source ------------------------------------------------------------
    if (auto E = runPipelineText("source", M,
                                 sourceStoragePipeline(Opts.Patterns),
                                 R.Stages, CxxFlavor::Auto, nullptr, "",
                                 ModuleBuilder(RebuildDeopt)))
      return std::move(E);
    MergeShards = nullptr;
    // One storage for both: the ir-final artifact is this module. The
    // Clang source emitter can mutate the module, so print it first.
    if (!SlotStorage && !Opts.ArtifactBase.empty()) {
      auto PrintStart = StageClock::now();
      FinalText = moduleText(*M);
      reportArtifactTiming(
          "print-ir-final",
          std::chrono::duration_cast<std::chrono::microseconds>(
              StageClock::now() - PrintStart)
              .count());
    }
    {
      FunctionStageTrace Trace("source", Out == Lang::Rust ? "rustc" : "clang",
                               "module");
      Expected<EmitResult> Em =
          Out == Lang::Rust
              ? emitRustcSource(FinalText.empty() ? moduleText(*M) : FinalText)
              : emitClangSource(*M, Out == Lang::Cpp, Opts.UserBodiesOnly);
      if (!Em)
        return Em.takeError();
      record(R.Stages, "source", "ok",
             Out == Lang::Rust  ? "rustc fork"
             : Out == Lang::Cpp ? "Clang frontend (C++)"
                                : "Clang frontend (C)");
      R.Text = std::move(Em->Text);
      R.LineVas = std::move(Em->LineVas);
    }
    if (FinalWorker.started()) {
      auto Text = FinalWorker.take(R.Stages);
      if (!Text)
        return Text.takeError();
      FinalText = std::move(*Text);
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
      auto WriteStart = StageClock::now();
      if (auto E = writeText(Path, A.Text))
        return std::move(E);
      reportArtifactTiming(
          ("write-" + A.Suffix).str(),
          std::chrono::duration_cast<std::chrono::microseconds>(
              StageClock::now() - WriteStart)
              .count());
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
