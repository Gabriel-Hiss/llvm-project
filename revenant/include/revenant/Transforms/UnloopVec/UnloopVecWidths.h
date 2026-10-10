//===- UnloopVecWidths.h - recover scalar element type/stride ---*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Matcher family that recovers the scalar element type and stride from
// vectorized loops with type changes: widening/narrowing (pmovzx/pmovsx,
// packus/packss, punpck), i8/i16 element types, interleaved/strided
// access via shuffles, unaligned heads, conversions int<->float.
//
// The existing UnloopVec pass handles CFG-level folding (dispatch to
// scalar tail). This family handles type-level recovery inside the
// vector body: it proves the scalar element type and stride, then
// rewrites vector operations to scalar operations.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNLOOPVEC_UNLOOPVECWIDTHS_H
#define REVENANT_TRANSFORMS_UNLOOPVEC_UNLOOPVECWIDTHS_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnloopVecWidthsPass : public llvm::PassInfoMixin<UnloopVecWidthsPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
