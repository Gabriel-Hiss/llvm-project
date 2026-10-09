//===- Uninline.h - recover calls inlined from a known callee -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Inverts LLVM function inlining for one family of shapes: a region of a
// caller that is the body of a callee that still exists as a standalone
// definition in the same module, spliced in with the call arguments bound.
// When the region is single-entry/single-exit and matches the callee body
// up to argument binding, the region is replaced by a call to the callee.
//
// Runs on LLVM IR only. See Uninline.cpp for the matching rules.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNINLINE_H
#define REVENANT_TRANSFORMS_UNINLINE_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UninlinePass : public llvm::PassInfoMixin<UninlinePass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
