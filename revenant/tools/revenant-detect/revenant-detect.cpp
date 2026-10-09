//===- revenant-detect.cpp - PE compiler profile probe ------------------===//
//
// Prints producer, language, and build traits for a PE as JSON:
//   revenant-detect <pe> [--pretty]
//
// JSON object: { "file", "producer", "language", "version",
//   "opt", "lto", "pgo", "stack_protection", "cfg", "frame_pointers",
//   "hot_cold_split", "evidence": [...] }.
// Unknown/unobservable fields are null (JSON) / "unknown".
//
//===----------------------------------------------------------------------===//

#include "revenant/Compiler/Profile.h"
#include "revenant/Object/PeFile.h"

#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace revenant;

namespace {

const char *optName(OptimizationLevel O) {
  switch (O) {
  case OptimizationLevel::Unknown: return "unknown";
  case OptimizationLevel::O0: return "O0";
  case OptimizationLevel::O1: return "O1";
  case OptimizationLevel::O2: return "O2";
  case OptimizationLevel::O3: return "O3";
  case OptimizationLevel::Os: return "Os";
  case OptimizationLevel::Oz: return "Oz";
  }
  return "unknown";
}

void printJsonString(raw_ostream &OS, StringRef S) {
  OS << '"';
  for (char C : S) {
    switch (C) {
    case '"': OS << "\\\""; break;
    case '\\': OS << "\\\\"; break;
    case '\n': OS << "\\n"; break;
    case '\r': OS << "\\r"; break;
    case '\t': OS << "\\t"; break;
    default:
      if ((unsigned char)C < 0x20) {
        static const char Hex[] = "0123456789abcdef";
        OS << "\\u00" << Hex[((unsigned)C >> 4) & 0xF] << Hex[(unsigned)C & 0xF];
      } else
        OS << C;
    }
  }
  OS << '"';
}

void printOptBool(raw_ostream &OS, std::optional<bool> B) {
  if (!B)
    OS << "null";
  else
    OS << (*B ? "true" : "false");
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  if (argc < 2 || argc > 3) {
    errs() << "usage: revenant-detect <pe> [--pretty]\n";
    return 2;
  }
  bool Pretty = argc == 3 && StringRef(argv[2]) == "--pretty";
  StringRef Path = argv[1];

  auto BufOrErr = MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    errs() << "revenant-detect: " << Path << ": "
           << BufOrErr.getError().message() << "\n";
    return 1;
  }
  StringRef Data = (*BufOrErr)->getBuffer();
  auto PeOrErr = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  if (!PeOrErr) {
    errs() << "revenant-detect: " << Path << ": "
           << toString(PeOrErr.takeError()) << "\n";
    return 1;
  }
  PeFile &PE = *PeOrErr;

  // Symbol names: imports + exports. No PDB: the detector never reads it.
  std::vector<std::string> Symbols;
  for (const Import &I : PE.Imports)
    if (I.Name)
      Symbols.push_back(*I.Name);
  for (const Export &E : PE.Exports)
    Symbols.push_back(E.Name);

  CompilerInfo Info = detectCompiler(PE, Symbols);
  const char *NL = Pretty ? "\n" : "";
  const char *Pad = Pretty ? "  " : "";
  const char *Sep = Pretty ? ": " : ":";
  outs() << "{" << NL;
  outs() << Pad << "\"file\"" << Sep;
  printJsonString(outs(), Path);
  outs() << "," << NL;
  outs() << Pad << "\"producer\"" << Sep;
  printJsonString(outs(), producerName(Info.Producer));
  outs() << "," << NL;
  outs() << Pad << "\"language\"" << Sep;
  printJsonString(outs(), languageName(Info.Language));
  outs() << "," << NL;
  outs() << Pad << "\"version\"" << Sep;
  if (Info.Version.empty())
    outs() << "null";
  else
    printJsonString(outs(), Info.Version);
  outs() << "," << NL;
  outs() << Pad << "\"opt\"" << Sep;
  printJsonString(outs(), optName(Info.Traits.Optimization));
  outs() << "," << NL;
  outs() << Pad << "\"lto\"" << Sep;
  printOptBool(outs(), Info.Traits.Lto);
  outs() << "," << NL;
  outs() << Pad << "\"pgo\"" << Sep;
  printOptBool(outs(), Info.Traits.Pgo);
  outs() << "," << NL;
  outs() << Pad << "\"stack_protection\"" << Sep;
  printOptBool(outs(), Info.Traits.StackProtection);
  outs() << "," << NL;
  outs() << Pad << "\"cfg\"" << Sep;
  printOptBool(outs(), Info.Traits.ControlFlowGuard);
  outs() << "," << NL;
  outs() << Pad << "\"frame_pointers\"" << Sep;
  printOptBool(outs(), Info.Traits.FramePointers);
  outs() << "," << NL;
  outs() << Pad << "\"hot_cold_split\"" << Sep
         << (Info.Traits.HotColdSplit ? "true" : "false") << "," << NL;
  outs() << Pad << "\"evidence\"" << Sep << "[";
  if (Pretty && !Info.Evidence.empty())
    outs() << "\n";
  for (size_t I = 0; I < Info.Evidence.size(); ++I) {
    if (Pretty)
      outs() << "    ";
    printJsonString(outs(), Info.Evidence[I]);
    if (I + 1 < Info.Evidence.size())
      outs() << ",";
    if (Pretty)
      outs() << "\n";
  }
  if (Pretty && !Info.Evidence.empty())
    outs() << Pad;
  outs() << "]";
  outs() << NL << "}\n";
  return 0;
}
