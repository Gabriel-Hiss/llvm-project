//==- revenant/Engine/Detect.h - producer language detection ------*- C++ -*-==//
//
// Port of crates/detect: decide what language/compiler produced the binary
// from symbol names, imports and embedded strings. The PDB compiland pass
// of the Rust implementation has no LLVM-side counterpart yet; evidence is
// gathered without it (pass 1 of crates/detect is a no-op here).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ENGINE_DETECT_H
#define REVENANT_ENGINE_DETECT_H

#include "revenant/Core/Image.h"

#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"

namespace revenant {

struct PeFile;

/// Producer language (crates/detect `Lang`).
enum class Lang { C, Cpp, Rust };

/// Producer toolchain (crates/detect `Compiler`).
enum class Compiler { Msvc, ClangCl, Rustc, MinGwGcc, Unknown };

struct CompilerInfo {
  Lang Language = Lang::C;
  Compiler Producer = Compiler::Unknown;
  std::optional<std::string> Version;
  /// Human-readable lines describing what produced the verdict.
  std::vector<std::string> Evidence;
};

/// Detect the producer. `symbolNames` is every known name in the binary:
/// imports, exports and any discovered function names.
CompilerInfo detectProducer(const PeFile &Pe,
                            llvm::ArrayRef<std::string> SymbolNames,
                            const Image &Img);

llvm::StringRef langName(Lang L);
llvm::StringRef compilerName(Compiler C);

} // namespace revenant

#endif // REVENANT_ENGINE_DETECT_H
