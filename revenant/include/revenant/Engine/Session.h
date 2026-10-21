//==- revenant/Engine/Session.h - decompile session facade --------*- C++ -*-==//
//
// C++ engine behind the C API and the revenant-decompile tool, mirroring
// crates/engine `Session`: open a PE, detect the producer, hold the
// analyzed Program, and decompile one or all functions through the stage
// pipeline (analysis -> signatures -> lift -> transforms -> structure ->
// source). Every stage is a real implementation; StageReport carries the
// status and elapsed time of each one.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ENGINE_SESSION_H
#define REVENANT_ENGINE_SESSION_H

#include "revenant/Core/Program.h"
#include "revenant/Engine/Detect.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace revenant {
namespace lift {
struct Signatures;
} // namespace lift


class Disasm;
struct PeFile;

/// One pipeline stage's outcome for the manifest and progress reporting.
struct StageReport {
  std::string Name;
  /// "ok" (the stage ran), "skipped" (not requested / not applicable).
  std::string Status;
  std::string Note;
  /// Artifact path written for this stage, when one exists.
  std::string Artifact;
  /// Time spent producing this stage, for CLI profiling.
  uint64_t ElapsedMicroseconds = 0;
};


/// Metadata policy for a decompile session.
enum class MetaMode { None, Embedded, Pdb };

/// Reachability and static-library labels for an analyzed PE.
struct FunctionClassification {
  std::optional<Va> UserEntry;
  /// Spelling for the traced entry ("main"/"wmain") from the startup path.
  /// Set only when the entry was traced via startup analysis; the pipeline
  /// applies it to the program when no discovered real name exists.
  std::optional<std::string> UserEntryName;
  std::set<Va> UserReachable;
  std::set<Va> Startup;
  std::set<Va> LibraryStarts;
  std::map<Va, std::string> LibraryNames;
  bool HasLibraryDB = false;
};

/// Which representation --emit / the API asks for.
enum class EmitKind { Source, Asm, IrRaw, IrClean, IrPreLike, IrFinal };

/// Options for one decompile run.
struct DecompileOpts {
  EmitKind Emit = EmitKind::Source;
  /// File base for per-stage artifacts: `<base>.<stage>.<ext>` plus
  /// `<base>.manifest.txt`. Empty = no artifact files are written; the
  /// stage texts are still produced and reported.
  std::string ArtifactBase;
  /// Run the optional patterns stage when the pass exists.
  bool Patterns = false;
  /// Optional static-library database used to recover function names/types.
  std::string LibraryDBPath;
};

/// Per-function decompile result plus stage outputs.
struct Decompiled {
  /// Emitted source, or the requested intermediate's text for non-source
  /// emit kinds.
  std::string Text;
  /// 1-based output line -> asm VAs feeding that line.
  std::vector<std::pair<uint32_t, std::vector<Va>>> LineVas;
  /// Stages in run order with their artifact files.
  std::vector<StageReport> Stages;
};

/// Session setup (mirrors `Session::open`): load, detect, analyze. The
/// setup stages' reports are kept in `SetupStages`.
class Session {
public:
  ~Session();
  Session(Session &&);
  Session &operator=(Session &&);
  Session(const Session &) = delete;

  /// Open `path` (PE32+ x86-64), detect the producer and build the
  /// function list + CFG model.
  static llvm::Expected<std::unique_ptr<Session>> open(llvm::StringRef Path);

  /// Open from already-parsed inputs (tests, embedders).
  static llvm::Expected<std::unique_ptr<Session>> create(PeFile Pe);

  const PeFile &pe() const { return *Pe; }
  const Program &program() const { return Prog; }
  const CompilerInfo &compiler() const { return Compiler; }
  Disasm &disasm() const { return *Dis; }

  /// Setup-stage outcomes (load/detect/analysis), for the manifest.
  const std::vector<StageReport> &setupStages() const { return SetupStages; }

  /// All known symbol names (imports, exports, function names).
  const std::vector<std::string> &symbolNames() const { return SymNames; }
  /// Apply a presentation metadata policy after discovery. None retains
  /// imported symbols for linking and restores generic names elsewhere.
  void setMetaMode(MetaMode Mode);


  /// Resolve `va` to a display name: externs, then the name table, then
  /// `sub_<hex>` for function starts.
  std::string nameFor(Va Addr) const;

  /// Find a function start by name (exact match on names, exports,
  /// `sub_<va>`), or nullopt.
  std::optional<Va> functionByName(llvm::StringRef Name) const;

  /// Classify user-reachable, startup, and matched library functions.
  /// An empty database path disables only library matching; entry and
  /// reachability analysis still run.
  llvm::Expected<FunctionClassification>
  classifyFunctions(Lang Out, llvm::StringRef LibraryDBPath = {}) const;

  /// Intel-syntax disassembly of the function at `start`.
  llvm::Expected<std::string> disasmFunction(Va Start) const;

  /// Intel-syntax disassembly of an already materialized function. The
  /// caller pays the decode once and reuses the result.
  std::string formatAsm(const Function &F) const;

  /// Materialize the fat `Function` (decoded instrs + edges) for `start`.
  llvm::Expected<Function> materialize(Va Start) const;

  /// Decompile one function through the stage pipeline. `Stages` in the
  /// result records what ran for real and what was stubbed.
  llvm::Expected<Decompiled> decompile(Va Start, Lang Out,
                                       const DecompileOpts &Opts = {});

  /// Decompile `Targets` into one shared LLVM module + one source file
  /// (empty = every function).
  llvm::Expected<Decompiled> decompileAll(llvm::ArrayRef<Va> Targets,
                                          Lang Out,
                                          const DecompileOpts &Opts = {});

  /// Assemble `Lines` into the VA line map for CAPI (first VA per line).
  static std::vector<std::pair<uint32_t, Va>>
  firstVaPerLine(const std::vector<std::pair<uint32_t, std::vector<Va>>> &In);

private:
  Session();

  llvm::Error setup();
  /// Build Program.Functions/Externs/Names with RevenantAnalysis.
  llvm::Error analyze();

  std::unique_ptr<PeFile> Pe;
  std::unique_ptr<Disasm> Dis;
  Program Prog;
  CompilerInfo Compiler;
  std::vector<std::string> SymNames;
  std::vector<StageReport> SetupStages;
  /// Recovered signatures, computed on the first decompile (whole-program).
  std::shared_ptr<const lift::Signatures> Sigs;
  std::map<Va, std::string> OriginalFunctionNames;
};

} // namespace revenant

#endif // REVENANT_ENGINE_SESSION_H
