//===-- Unswitch.h - switch deoptimization pass -------------------*- C++ -*-===//
// Part of the Revenant decompiler port. Undo SimplifyCFG switch lowerings
// (lookup tables, linear maps, bitmaps, range-to-icmp, switch-to-select,
// switch-of-select, icmp/select merges) back into `switch` instructions.
//
// Owns the `revenant-unswitch` Function pass.
#ifndef REVENANT_TRANSFORMS_UNSWITCH_UNSWITCH_H
#define REVENANT_TRANSFORMS_UNSWITCH_UNSWITCH_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnswitchPass : llvm::PassInfoMixin<UnswitchPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
