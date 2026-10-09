#ifndef REVENANT_TRANSFORMS_UNIV_H
#define REVENANT_TRANSFORMS_UNIV_H

#include "llvm/IR/PassManager.h"

namespace revenant {

/// Undo `indvars` IV rewriting and `loop-reduce` strength reduction.
///
/// An optimized loop walks a widened `i64` IV (`%indvars.iv`), tests it with
/// `icmp ne %iv, %wide.trip.count` (or an `eq` latch against a masked trip
/// count), and addresses elements with a scaled `scevgep`
/// (`shl %iv, 2` + `getelementptr i8`). Pointer loops instead thread a `ptr`
/// PHI stepped by `getelementptr i8, 1` and exit on `icmp eq %ptr, %end`.
/// Pointer-chasing rustc iterator loops compare the two fat-pointer halves
/// (`icmp eq %ptr, %end`) and step the head by the element size.
///
/// The pass rebuilds the source shape: a narrow integer index PHI starting
/// at 0, stepping by 1, tested with `icmp slt %i, %n`, plus one
/// `getelementptr <elem>, %base, %i` per access. IV widening (`sext`/`zext`
/// of the index or `trunc` of the trip count) folds away; `!revenant.va`
/// moves to the rebuilt instructions and `!revenant.opaque` is never
/// dropped. Anything ambiguous (unknown step, non-element stride, shared
/// IV, extra IV users) stays put.
struct UnivPass : llvm::PassInfoMixin<UnivPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
