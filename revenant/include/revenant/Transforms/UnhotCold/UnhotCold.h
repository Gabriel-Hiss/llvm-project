//===- UnhotCold.h - merge hot/cold splits, undo PGO reorder -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Undoes hot/cold function splitting and PGO-driven block reordering on
// lifted IR so downstream passes see one function in source order:
//
// - GCC -O2/-O3 outlines unlikely code into `foo.cold` chunks that live far
//   from the hot body (corpus: sqlite-gcc-o2, lua-gcc-o2: `.cold` symbols in
//   the oracle binary, single `jmp`/`je` from the hot caller into the cold
//   chunk, cold chunk ends in `call abort`/`ud2` traps). The lift recovers
//   the cold chunk as its own function; this pass inlines it back as cold
//   branches of the hot caller.
// - MSVC PGO emits cold chunks as chained-unwind `.pdata` rows of the same
//   primary (corpus: cjson-cl-o2pgo: 208 chained entries under 83
//   primaries, cold chunk reached by one conditional jump from hot code).
//   Discovery already joins chained rows into the primary's chunks; the
//   lift emits one function whose cold blocks sit far away in VA order.
//   This pass moves them next to their hot predecessors.
// - clang machine-function-split (`foo.cold` in `.text.unlikely.*`) has the
//   same shape as the GCC case and is handled by the same merge.
// - PGO block placement inverts branches (cold target becomes fallthrough)
//   and scatters blocks far from source order. This pass reorders blocks
//   toward VA order and re-canonicalizes inverted branches, using
//   `!prof`/`!branch_weights` metadata when present and VA order otherwise.
//
// Detection is purely CFG + `!revenant.va` shape based: a cold candidate is
// a function whose blocks all carry VAs far (>= 64 KiB) from the hot
// caller's VAs and that has exactly one hot predecessor edge. Compiler
// profile gating (Swe2CompilerProfile `ColdChunkConvention`, lift
// `"revenant.hotcold"` metadata) refines the thresholds once it lands; the
// shape match is conservative enough to run without it.
//
// Guarantees: only fires on an airtight match (single hot caller, cold
// blocks dominated by the merge edge, no live-in phis except the edge
// value); preserves `!revenant.va` on moved instructions; never drops
// `!revenant.opaque` calls; never changes semantics (merge = inline a
// single-caller noreturn/returning chunk; reorder = block permutation +
// branch canonicalization, both refinement-free).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNHOTCOLD_H
#define REVENANT_TRANSFORMS_UNHOTCOLD_H

#include "llvm/IR/PassManager.h"

namespace revenant {

/// Module pass: the cold-chunk search is module-wide (a cold chunk is a
/// sibling function of its hot caller), so the candidate index is built
/// once per module instead of once per function.
struct UnhotColdPass : public llvm::PassInfoMixin<UnhotColdPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
