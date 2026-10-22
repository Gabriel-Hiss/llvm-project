//===- UnvecSearch.h - fold vectorized early-exit scans ------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Folds vectorized early-exit search loops (find/strchr/strlen/memchr-like
// scans over bytes, 16-bit or 32-bit lanes, SSE2..AVX2, gcc/clang/MSVC
// forms) back to the scalar search loop the compiler started from. Runs
// as its own pass `revenant-unvecsearch`, AFTER revenant-devectorize:
// Devectorize owns per-instruction vector->scalar explosion inside a
// block and never changes the CFG; UnloopVec owns counted-loop CFG
// collapse and never rewrites inside vector bodies; this pass owns the
// early-exit scan skeleton only and never touches counted loops.
//
// What it does: finds a chunk-scan loop H (a single-block self-loop that
// loads one vector per pass, compares lanes against a broadcast needle
// or a second buffer, folds the lane mask to a scalar bitmask, and exits
// on the first nonzero mask bit with a bit-scan index) plus its scalar
// tail T (byte/element compare loop covering the remaining range), and
// rewrites the function so the scalar tail covers the whole range from
// element 0. The chunk loop and its dispatch are erased.
//
// Firing shape (all structural):
//   H: single-block self-loop latch with a counted index/pointer IV.
//      Body, in order: one vector load, one lane compare (icmp eq, or
//      icmp eq folded with a second compare via or for the strchr-like
//      two-way scan), mask fold to scalar (direct bitcast <N x i1> to
//      integer, or the sext+bitcast+icmp-slt sign-bit chain the lifter
//      emits for wider lanes), cond br on mask==0 back to H, and on the
//      taken edge a bit-scan (llvm.cttz/llvm.ctpop-derived index,
//      optionally lshr-scaled for wide lanes and xor-inverted for the
//      compare-like inverted mask) added to the chunk base to form the
//      result.
//   T: scalar compare loop over the same base pointer and trip count,
//      entered with the chunk-covered prefix as its start; after the
//      fold it starts at 0 and covers the full count.
//   The needle must be a loop-invariant broadcast (splat) of the
//   searched value, or a second-buffer load at the same index for the
//   compare form. No calls except LLVM intrinsics, no inline asm, no
//   volatile/atomic memory, no stores in H, !revenant.opaque bails.
//
// Why restarting the tail at 0 is sound: the chunk loop is a pure
// prefix scan (each pass reads a disjoint chunk and exits at the first
// hit, never writing), so the first hit in chunk order equals the
// first hit in element order; the scalar tail computes exactly that
// first hit over any range it is given, and the small-N path proves it
// correct from 0. The rewrite keeps every kept instruction unchanged
// (only branch targets and dead code change), so it is refinement-free.
//
// Disjointness from UnloopVec (owner: MuseUnloopVecR): this pass never
// fires on counted trip-bound loops (n.vec/masked-bound skeletons) and
// requires the mask+bit-scan skeleton; UnloopVec never fires on the
// mask+bit-scan skeleton. Scan-fold output is a plain scalar loop with
// no vector types. Coordination is marker-free by shape.
//
// Guarantees: only fires on the airtight match above; preserves
// !revenant.va on the rewritten dispatch branch (copied from the old
// terminator) and never drops !revenant.opaque (any opaque call in H
// bails); !revenant.region annotations on kept instructions survive
// because kept blocks are never rebuilt.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNVECSEARCH_UNVECSEARCH_H
#define REVENANT_TRANSFORMS_UNVECSEARCH_UNVECSEARCH_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnvecSearchPass : public llvm::PassInfoMixin<UnvecSearchPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
