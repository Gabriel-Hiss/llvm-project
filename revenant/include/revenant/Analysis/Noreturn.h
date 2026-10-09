//===- revenant/Analysis/Noreturn.h - Noreturn detection ---------*- C++ -*-==//
//
// Mirrors crates/analysis/src/noreturn.rs: known noreturn imports, panic
// name bits, and a fixpoint over the call graph (a function is noreturn
// only when every reachable exit is proven not to return).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_NORETURN_H
#define REVENANT_ANALYSIS_NORETURN_H

#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"

namespace revenant {

class TargetHooks;

/// True when `Name` (a `dll!name` import or plain symbol name) is a known
/// noreturn API: an exact short-name match, a decorated `name@...` form, a
/// `...::name` qualified form, or a noreturn name bit (Rust panic paths).
bool isNoreturnImport(llvm::StringRef Name);

/// True when `Name` contains a noreturn demangled-name bit
/// (`core::panicking`, `rust_begin_unwind`, ...).
bool nameIsNoreturn(llvm::StringRef Name);

/// Mark `IsNoreturn` on every function; returns the set of noreturn starts.
///
/// A function is noreturn when a known-noreturn name says so, or when every
/// reachable exit is proven not to return. `ret` anywhere, an unresolved
/// indirect exit, a dead end at undecodable bytes, or a tail call to a
/// target that isn't proven noreturn each rule it out. A block ending in a
/// noreturn call proves only that one exit: a sibling normal exit (`jmp
/// memcpy` after the error path) keeps the function ordinary.
///
/// Inferred flags from any earlier pass are reset first; only explicit name
/// proofs carry over. Trap classification (ud0/ud1/ud2, int3, int 0x29)
/// comes from `Hooks` (Discovery's `trapKind`); a null hook still honors
/// `FlowKind::Trap` terminators. The fixpoint iterates the returned set,
/// hence `std::set` for determinism.
///
/// `callSiteTarget` (direct call target / RIP-relative slot) is Discovery's
/// and lives in `revenant/Analysis/Analysis.h`; this header does not
/// declare it.
std::set<Va> propagateNoreturn(std::vector<Function> &Funcs,
                               const std::map<Va, Extern> &Externs,
                               const std::map<Va, std::string> &Names,
                               const TargetHooks &Hooks);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_NORETURN_H
