//==- revenant/Analysis/LibMatch.h - Static library matches ---*- C++ -*-==//
//
// Database records and runtime matching for known static-library functions.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_LIBMATCH_H
#define REVENANT_ANALYSIS_LIBMATCH_H

#include "revenant/Core/Va.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace revenant {

struct Program;
struct Signature;
struct Signatures;

struct LibraryParam {
  std::string Name;
  std::string Type;
  uint32_t Bits = 0;
  bool Float = false;
};
struct LibraryPrototype {
  std::string ReturnType;
  uint32_t ReturnBits = 0;
  bool ReturnFloat = false;
  bool ReturnVoid = false;
  bool VarArgs = false;
  std::vector<LibraryParam> Params;
};

struct LibraryFunction {
  std::string Name;
  std::string MangledName;
  std::string Archive;
  std::string Member;
  uint64_t Size = 0;
  /// Original function bytes. Relocation fields listed below are zeroed when
  /// matching, so database records can be inspected without losing contents.
  std::vector<uint8_t> Bytes;
  std::vector<uint32_t> MaskedRelocations;
  std::vector<std::string> Callees;
  std::optional<LibraryPrototype> Prototype;
};

struct LibraryDatabase {
  std::vector<LibraryFunction> Functions;

  static llvm::Expected<LibraryDatabase> load(llvm::StringRef Path);
  llvm::Error save(llvm::StringRef Path) const;
};

struct LibraryMatch {
  Va Start;
  uint32_t FunctionIndex = 0;
};

/// Match exact function bytes after masking relocation fields recorded from
/// the source archive. Only one-chunk functions with matching sizes are
/// considered.
std::vector<LibraryMatch> matchLibraryFunctions(const Program &P,
                                                 const LibraryDatabase &DB);

/// Attach matched names and prototypes to the existing program and inferred
/// signatures. Ambiguous byte matches are omitted unless callee evidence picks
/// one database record.
void applyLibraryMatches(Program &P, Signatures &Sigs,
                         llvm::ArrayRef<LibraryMatch> Matches,
                         const LibraryDatabase &DB);

} // namespace revenant

#endif // REVENANT_ANALYSIS_LIBMATCH_H
