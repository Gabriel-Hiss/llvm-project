//==- revenant/Analysis/Resolver.h - Symbol resolver for asm output -*- C++ -*-==//
//
// Maps operand target VAs to display names: functions and data via the
// program's name table, IAT slots as `dll!name` via the extern table, and
// referenced strings as short `aIDENT` labels. Hooks into the skeleton's
// Intel printer output through `NameProvider::decorateLine`, substituting
// resolved names for the printed branch-target addresses and RIP-relative
// displacements. Mirrors crates/analysis/src/resolver.rs.
//
// The three tables are shared with `Program`'s maps (no copies); the maps
// must outlive the resolver.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_RESOLVER_H
#define REVENANT_ANALYSIS_RESOLVER_H

#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"
#include "revenant/Disasm/Disasm.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace revenant {

/// Concrete `NameProvider` over the program's name tables.
class Resolver : public NameProvider {
public:
  Resolver(std::shared_ptr<const std::map<Va, std::string>> Names,
           std::shared_ptr<const std::map<Va, Extern>> Externs,
           std::shared_ptr<const std::map<Va, std::string>> Strings);

  /// Display name for `Addr` on an exact table hit (names, then externs,
  /// then a short label derived from the referenced string).
  std::optional<std::string> nameAt(Va Addr) const;

  std::optional<std::string> nameFor(Va Addr, RefKind Kind) const override;

  /// Substitute resolved names into one Intel-syntax line: branch/call
  /// target addresses become the target's name, `rip + 0x<disp>` becomes the
  /// referenced name. Operands without a name keep their printed form.
  std::string
  decorateLine(const Instr &I, llvm::StringRef Line,
               llvm::ArrayRef<std::pair<Va, RefKind>> Refs) const override;

private:
  std::shared_ptr<const std::map<Va, std::string>> Names;
  std::shared_ptr<const std::map<Va, Extern>> Externs;
  std::shared_ptr<const std::map<Va, std::string>> Strings;
};

/// Wrap a `Resolver` over the three shared tables into a `SharedNames`.
SharedNames
makeResolver(std::shared_ptr<const std::map<Va, std::string>> Names,
             std::shared_ptr<const std::map<Va, Extern>> Externs,
             std::shared_ptr<const std::map<Va, std::string>> Strings);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_RESOLVER_H
