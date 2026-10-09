//===- Uneh.h - rebuild exception handling ------------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Rebuilds source-level exception handling on lifted IR: MSVC C++ EH
// (FuncInfo try/unwind maps, catch funclets), SEH scope tables, GCC/MinGW
// LSDA landing pads, and Rust unwind cleanups. Ports nothing from
// crates/unopt (no EH handling exists there); the shapes come from the
// compilers' own pre-optimization IR in target/parity/<id>/artifacts
// (*.pre.ll, see the .cod/.map/.s/.ll corpus artifacts).
//
// Input: lifted functions where throwing calls are plain `call`s (the
// lifter cannot emit `invoke`: the unwind destination is only known from
// the EH tables) plus table-derived region markers (owner: analysis/lift,
// TBD): which call ranges belong to a try, which funclet names are catch
// vs dtor handlers, which personality governs the function.
//
// Output: native LLVM EH only (LLVM EH constructs preferred, agreed with
// LunaIRContract): `personality` + `invoke`/`catchswitch`/`catchpad`/
// `catchret` for try/catch, `cleanuppad`/`cleanupret` for unwind-only and
// Rust drop glue, `landingpad`/`resume` + `llvm.eh.typeid.for` for MinGW
// LSDA. No new `!revenant.region` descriptors (schema locked): consumers
// use labels/gotos. SEH filter expressions stay opaque `revenant.asm.*`
// calls; only the control flow is native. Provenance (`!revenant.va`) is
// copied from the source call/region onto every synthesized instruction;
// `!revenant.opaque` is never moved or dropped.
//
// Profile gating: every compiler-specific habit (personality names,
// `?catch$N`/`?dtor$N`/filt/fin funclet recognition, FuncInfo-vs-LSDA
// layout) sits behind the local EHProfile (include/revenant/Transforms/
// Uneh/EHProfile.h), a fallback until LunaCompilerProfile's
// `revenant/Compiler/Profile.h` (`profileFor`, `ehModel()`) lands; the
// generic path (unknown producer) assumes Itanium/LLVM-backend shapes.
//
// Guarantees: only fires on an airtight match (known personality +
// table-backed region); noexcept/nounwind/panic=abort functions are never
// given EH; each rewrite is semantics-preserving (invoke/call split keeps
// the normal edge identical, pads only run on unwind).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNEH_UNEH_H
#define REVENANT_TRANSFORMS_UNEH_UNEH_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnehPass : llvm::PassInfoMixin<UnehPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
