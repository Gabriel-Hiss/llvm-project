//===- Devectorize.h - undo SLP/loop vectorization ---------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Port of `crates/unopt` (`lane0` + `devectorize`): fixed-width vector
// code produced by the loop vectorizer or the SLP vectorizer is rewritten
// back into the scalar operations a programmer would have written. Runs in
// three phases: lane-0 web scalarization, single-block loop reduction
// devectorization (with exit `llvm.vector.reduce.*` aliasing), then
// straight-line SLP explosion. Anything unrecognized leaves the region
// untouched; the CFG is never changed, so `!revenant.region` annotations
// survive, and every replacement instruction inherits the replaced
// instruction's `!revenant.va` provenance and debug location (manual copy
// until `revenant::ir::copyInstructionVA` from RevenantIR lands).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_DEVECTORIZE_H
#define REVENANT_TRANSFORMS_DEVECTORIZE_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct DevectorizePass : public llvm::PassInfoMixin<DevectorizePass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
