//===- Unselect.h - undo if-conversion and predicate canonicalization -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Undoes two families of optimizer transforms that hide -O0-style control
// flow and comparisons:
//
// 1. SimplifyCFG speculation / if-conversion: `select` chains and
//    min/max/abs intrinsics that came from `if` statements (clang-cl O1/O2
//    vs ir-pre corpus) are rewritten back to branches. Corpus examples
//    (c-clang-o1 main.pre.ll vs main.post.ll):
//      cmp_unsigned: nested if/return ladder -> select chain
//      nested_if:    nested if/return ladder -> partial branch fold + select
//      minmax:       ternary min/max/abs if-ladders -> selects (fabs, smax)
//      strings:      if/else pointer pick -> select
// 2. InstCombine predicate canonicalization: swapped/inverted compares with
//    poison-generating flags (nneg, samesign) and select-operand swaps are
//    rewritten toward the straightforward clang -O0 forms, e.g. `icmp ne`
//    back to the source `icmp eq` shape where the swap is provable.
//
// Only fires when ir-pre had control flow for the construct: every select
// rewrite requires a diamond region (two single-pred single-succ sides with
// a common post-dominator) or a min/max/abs intrinsic whose expansion is
// provably branch-equivalent. Anything else stays put. Every new
// instruction inherits the replaced instruction's `!revenant.va`
// provenance and debug location; `!revenant.opaque` calls are never
// touched.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNSELECT_UNSELECT_H
#define REVENANT_TRANSFORMS_UNSELECT_UNSELECT_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnselectPass : public llvm::PassInfoMixin<UnselectPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
