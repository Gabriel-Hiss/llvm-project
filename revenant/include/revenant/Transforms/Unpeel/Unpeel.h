//===- revenant/Transforms/Unpeel/Unpeel.h - fold peeled prologues -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// `revenant-unpeel` folds a peeled loop prologue back into the loop.
//
// A compiler peels iterations out of a loop to make the remaining trip count
// suit a vectorized or unrolled body (and to align the first access). The
// deopt passes that run before this one restore the *body* to one element per
// iteration but leave the prologue in place, so the recovered source carries a
// straight-line copy of the first iteration before the loop instead of the one
// loop the author wrote.
//
// The pass fires only on evidence: the header must already be in canonical
// per-element form (an `i32` index PHI from the entry value stepping by 1,
// exited by `icmp slt` against a loop-invariant bound), every induction value
// the peeled path produces must be exactly the entry-state value of that value
// after one iteration, and every side-effecting instruction in the peeled body
// must match the loop body's own first iteration. Anything else stays put.
//
// `!revenant.va` moves to every instruction the pass rewrites and
// `!revenant.opaque` is never created, matched or dropped.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNPEEL_H
#define REVENANT_TRANSFORMS_UNPEEL_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnpeelPass : public llvm::PassInfoMixin<UnpeelPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
