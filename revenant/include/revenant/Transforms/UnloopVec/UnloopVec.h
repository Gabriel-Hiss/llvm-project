//===- UnloopVec.h - undo loop-vectorizer skeletons ------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Reverses the loop vectorizer's multi-block skeleton (vector body +
// scalar remainder + runtime checks + interleave) back to the single
// scalar loop the compiler started from (ir-pre shape). Runs AFTER
// revenant-devectorize in the unopt group: Devectorize owns
// per-instruction vector->scalar explosion inside a block and never
// changes the CFG; this pass owns the loop-level CFG collapse and never
// rewrites inside vector bodies.
//
// What it does: finds the vectorizer's dispatch block D (min-iters check,
// possibly OR-ed with a runtime memcheck) that branches to a scalar-tail
// entry S or into a vector region R, and folds D to always take the
// scalar edge. S plus everything downstream is the vectorizer's own
// complete fallback program for the range, so starting it at the
// loop-start values is exactly the pre-vectorization loop. Dead vector
// tiers (wide body, middle reduce/combine, epilogue vector loop,
// memcheck) are erased.
//
// Firing shape (all structural, no loop metadata needed):
//   D: cond br to {S, V}, V in R. D's condition is never inspected.
//   R: forward closure from V stopping at S/L/ret blocks. Single entry:
//      every R block's preds are in R or D. No multi-block cycles: every
//      cycle is a single-block self-loop that is vector-typed or a
//      counted loop from constant 0 (covers Devectorize-scalarized
//      tiers). No calls except LLVM intrinsics, no inline asm, no
//      volatile/atomic memory, no ret/unreachable/trap, br-only
//      terminators.
//   S: preds are exactly {D} plus R blocks; S is not a loop header; S's
//      own terminator condition never depends on an S phi (tier routing
//      stays N-driven, as the vectorizer built it).
//   L: single-block scalar self-loop with preds exactly {S, L}; its IV
//      starts at an S phi whose D-incoming is constant 0; other L phis
//      start at S phis or values defined outside R; L's exit bound
//      derives from the same trip count N as n.vec (through pure ops).
//   Top vector body B in R: IV from constant 0, constant step K >= 2,
//      exit compares IV-next == n.vec where n.vec is N with low bits
//      masked (and-form) or N minus N mod K (sub/urem-form), and that
//      same n.vec value feeds S's resume phi from an R block.
//   Escape: R definitions are used only inside R, by S phis, by phis of
//      non-S outsiders through R-block edges (dropped with the edge),
//      or by debug intrinsics.
//   FP: an FP-typed accumulator is only collapsed when every FP binop in
//      R carries reassoc/fast flags (the IR's own license to reassociate,
//      the same one the vectorizer used); integer reductions always go.
//
// Why starting the tail at 0 is sound for tiered shapes (wide loop +
// epilogue vector loop + unrolled scalar tiers, e.g. AVX2 c/avx2 sum):
// every tier dispatch downstream of S tests the trip count N (N mod
// step congruences) or the remaining count (N minus resume), never an
// assumed resume alignment, and every counted tier loop entered under
// its check covers exactly its range when started at a multiple of its
// step. 0 satisfies every such check simultaneously, and the vectorizer
// proved the tiers consistent for tier resumes; the small-N path proves
// S correct from 0. Anything else bails and is left for a later cut
// (tail-folding with no scalar tail, fixed-size epilogues, multi-block
// vector loops, FP without reassoc flags).
//
// Guarantees: only fires on the airtight match above; preserves
// !revenant.va on the rewritten dispatch branch (copied from the old
// terminator) and never drops !revenant.opaque (any opaque call in R
// bails); !revenant.region annotations on kept instructions survive
// because kept blocks are never rebuilt.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNLOOPVEC_UNLOOPVEC_H
#define REVENANT_TRANSFORMS_UNLOOPVEC_UNLOOPVEC_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnloopVecPass : public llvm::PassInfoMixin<UnloopVecPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
