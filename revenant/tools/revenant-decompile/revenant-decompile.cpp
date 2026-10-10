//==- tools/revenant-decompile - end-to-end driver ------------------------==//
//
//   revenant-decompile <pe> [--func <va|name>]... [--lang c|cpp|rust]
//                        [--meta none|embedded|pdb]
//                        [--emit asm|ir-raw|ir-clean|ir-final|source]
//                        [-o <file>] [--patterns] [--list] [--no-artifacts]
//
// Runs the whole decompiler pipeline (object -> analysis -> sigs -> lift ->
// transforms -> structure -> source) and writes every stage artifact next
// to the output: <out>.asm.txt, <out>.ir-raw.ll, <out>.ir-clean.ll,
// <out>.ir-final.ll, <out>.source.<ext>, <out>.manifest.txt. With no -o,
// artifacts land next to the input binary as <pe>.decompiled.*.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Session.h"

#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/STLExtras.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;
using namespace revenant;



static cl::opt<std::string> Input(cl::Positional, cl::desc("<pe>"),
                                  cl::Required);
static cl::list<std::string>
    Funcs("func", cl::desc("function start VA (0x...) or name; repeatable. "
                           "Default: all discovered functions"),
          cl::value_desc("va|name"));
static cl::opt<std::string>
    FuncFile("func-file", cl::desc("file of function start VAs or names"),
             cl::value_desc("file"));
static cl::opt<std::string>
    LangOpt("lang", cl::desc("output language: c|cpp|rust "
                             "(default: detected producer language)"),
            cl::value_desc("lang"));
static cl::opt<std::string>
    EmitOpt("emit",
            cl::desc("what to print/write: asm|ir-raw|ir-clean|ir-pre-like|"
                     "ir-final|source (default: source)"),
            cl::value_desc("kind"));
static cl::opt<std::string> Output("o", cl::desc("output file (default stdout)"),
                                   cl::value_desc("file"));
static cl::opt<bool> ListFuncs("list",
                               cl::desc("list discovered functions and exit"));
static cl::opt<bool> ClassifyFuncs(
    "classify", cl::desc("with --list, classify each function as user/library/startup"));
static cl::opt<bool> Patterns("patterns",
                              cl::desc("run the opt-in revenant-patterns pass"));
static cl::opt<bool> NoArtifacts("no-artifacts",
                                 cl::desc("do not write stage artifact files"));
static cl::opt<bool> ShowTime("time", cl::desc("print elapsed time per stage"));
static cl::opt<std::string>
    MetaOpt("meta", cl::desc("metadata: none|embedded|pdb (default: embedded)"),
            cl::value_desc("mode"), cl::init("embedded"));

static std::optional<MetaMode> metaMode(StringRef S) {
  return StringSwitch<std::optional<MetaMode>>(S)
      .Case("none", MetaMode::None)
      .Case("embedded", MetaMode::Embedded)
      .Case("pdb", MetaMode::Pdb)
      .Default(std::nullopt);
}

static cl::opt<std::string>
    LibraryDB("libdb",
              cl::desc("static-library match database (default: optional "
                       "libdb.json next to the executable)"),
              cl::value_desc("file"));
static std::optional<EmitKind> emitKind(StringRef S) {
  return StringSwitch<std::optional<EmitKind>>(S)
      .Case("asm", EmitKind::Asm)
      .Case("ir-raw", EmitKind::IrRaw)
      .Case("ir-clean", EmitKind::IrClean)
      .Case("ir-final", EmitKind::IrFinal)
      .Case("ir-pre-like", EmitKind::IrPreLike)
      .Case("source", EmitKind::Source)
      .Default(std::nullopt);
}

static std::optional<Lang> langOf(StringRef S) {
  return StringSwitch<std::optional<Lang>>(S)
      .Case("c", Lang::C)
      .Case("cpp", Lang::Cpp)
      .Case("c++", Lang::Cpp)
      .Case("rust", Lang::Rust)
      .Case("rs", Lang::Rust)
      .Default(std::nullopt);
}

int main(int Argc, char **Argv) {
  InitLLVM X(Argc, Argv);
  cl::ParseCommandLineOptions(
      Argc, Argv,
      "revenant-decompile: PE -> asm -> IR -> source, all stages\n");

  SmallString<256> LibraryDBPath(LibraryDB);
  if (LibraryDBPath.empty()) {
    SmallString<256> ExePath(
        sys::fs::getMainExecutable(Argv[0], reinterpret_cast<void *>(&main)));
    LibraryDBPath = sys::path::parent_path(ExePath);
    sys::path::append(LibraryDBPath, "libdb.json");
    if (!sys::fs::exists(LibraryDBPath))
      LibraryDBPath.clear();
  }

  EmitKind Emit = EmitKind::Source;
  if (!EmitOpt.empty()) {
    auto K = emitKind(EmitOpt);
    if (!K) {
      errs() << "revenant-decompile: unknown --emit '" << EmitOpt << "'\n";
      return 2;
    }
    Emit = *K;
  }
  auto M = metaMode(MetaOpt);
  if (!M) {
    errs() << "revenant-decompile: unknown --meta '" << MetaOpt
           << "' (expected none|embedded|pdb)\n";
    return 2;
  }
  if (*M == MetaMode::Pdb) {
    errs() << "revenant-decompile: --meta=pdb is not implemented yet\n";
    return 2;
  }
  if (LibraryDB.empty() && !sys::fs::exists(LibraryDBPath))
    LibraryDBPath.clear();
  auto SOrErr = Session::open(Input);
  if (!SOrErr) {
    errs() << "revenant-decompile: " << toString(SOrErr.takeError()) << "\n";
    return 1;
  }
  std::unique_ptr<Session> S = std::move(*SOrErr);
  S->setMetaMode(*M);

  Lang Out = Lang::C;
  if (!LangOpt.empty()) {
    auto L = langOf(LangOpt);
    if (!L) {
      errs() << "revenant-decompile: unknown --lang '" << LangOpt << "'\n";
      return 2;
    }
    Out = *L;
  } else {
    Out = S->compiler().Language;
  }

  if (ClassifyFuncs && !ListFuncs) {
    errs() << "revenant-decompile: --classify requires --list\n";
    return 2;
  }
  if (ListFuncs) {
    FunctionClassification Classification;
    if (ClassifyFuncs) {
      auto ClassificationOrErr =
          S->classifyFunctions(Out, LibraryDBPath);
      if (!ClassificationOrErr) {
        errs() << "revenant-decompile: "
               << toString(ClassificationOrErr.takeError()) << "\n";
        return 2;
      }
      Classification = std::move(*ClassificationOrErr);
      if (!Classification.UserEntry) {
        errs() << "revenant-decompile: unable to trace user entry from PE startup\n";
        return 2;
      }
      outs() << "user-entry\t"
             << format_hex(Classification.UserEntry->Value, 0) << "\n";
    }
    for (const auto &[Start, Rec] : S->program().Functions) {
      StringRef Name = Rec.Name;
      auto Lib = Classification.LibraryNames.find(Start);
      if (Lib != Classification.LibraryNames.end())
        Name = Lib->second;
      // The startup-traced entry carries its real spelling even though it
      // is user code, so downstream renaming and the C emitter's main
      // normalization fire without a PDB.
      if (Classification.UserEntry && Start == *Classification.UserEntry &&
          Classification.UserEntryName)
        Name = *Classification.UserEntryName;
      if (!ClassifyFuncs)
        outs() << Rec.InstrRvas.size() << " instrs\n";
      else if (Classification.UserReachable.count(Start))
        outs() << "user\n";
      else if (Classification.LibraryStarts.count(Start))
        outs() << "library\n";
      else if (Classification.Startup.count(Start))
        outs() << "startup\n";
      else if (Classification.HasLibraryDB)
        outs() << "library\n";
      else
        outs() << "unclassified\n";
    }
    return 0;
  }
  std::vector<std::string> FunctionInputs(Funcs.begin(), Funcs.end());
  if (!FuncFile.empty()) {
    auto FileOrErr = MemoryBuffer::getFile(FuncFile);
    if (!FileOrErr) {
      errs() << "revenant-decompile: cannot read function list '"
             << FuncFile << "': " << FileOrErr.getError().message() << "\n";
      return 2;
    }
    SmallVector<StringRef, 32> Lines;
    (*FileOrErr)->getBuffer().split(Lines, '\n');
    for (StringRef Line : Lines) {
      Line = Line.trim();
      if (!Line.empty())
        FunctionInputs.push_back(Line.str());
    }
    if (FunctionInputs.empty()) {
      errs() << "revenant-decompile: function list is empty\n";
      return 2;
    }
  }


  // Resolve --func arguments.
  std::vector<Va> Targets;
  for (const std::string &F : FunctionInputs) {
    StringRef Fs(F);
    std::optional<Va> Addr;
    uint64_t V = 0;
    if (!Fs.getAsInteger(0, V))
      Addr = Va(V);
    else if (auto ByName = S->functionByName(Fs))
      Addr = *ByName;
    if (!Addr || !S->program().Functions.count(*Addr)) {
      errs() << "revenant-decompile: '" << F
             << "' is not a discovered function\n";
      return 2;
    }
    Targets.push_back(*Addr);
  }

  DecompileOpts Opts;
  Opts.Emit = Emit;
  Opts.Patterns = Patterns;
  Opts.LibraryDBPath = LibraryDBPath.str().str();
  if (!NoArtifacts) {
    // Artifact base: the output path if given, else <pe>.decompiled.
    std::string Base =
        Output.empty() ? (Input + ".decompiled") : Output.getValue();
    Opts.ArtifactBase = Base;
  }

  auto DOrErr = S->decompileAll(Targets, Out, Opts);
  if (!DOrErr) {
    errs() << "revenant-decompile: " << toString(DOrErr.takeError()) << "\n";
    return 1;
  }
  const Decompiled &D = *DOrErr;

  // Stage report on stderr; the payload goes to stdout/-o.
  for (const StageReport &St : D.Stages) {
    errs() << "[" << St.Status << "] " << St.Name
           << (St.Note.empty() ? "" : ": " + St.Note);
    if (ShowTime)
      errs() << " (" << formatv("{0:F2}", St.ElapsedMicroseconds / 1000.0)
             << " ms)";
    errs() << "\n";
  }

  if (Output.empty()) {
    outs() << D.Text;
    return 0;
  }
  std::error_code EC;
  ToolOutputFile Out2(Output, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "revenant-decompile: " << Output << ": " << EC.message() << "\n";
    return 1;
  }
  Out2.os() << D.Text;
  Out2.keep();
  return 0;
}
