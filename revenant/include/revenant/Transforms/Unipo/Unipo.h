//===- Unipo.h - undo interprocedural (IPO) optimizations -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Inverts selected LLVM IPO transforms on lifted IR: source-DI-backed return
// restoration, DI-backed dead-formal restoration, named clone recovery, and
// the narrow GCC constprop `this`-formal restoration. Structural clone
// folding is disabled (no body comparison proves shared source identity).
// Every signature rewrite needs independent evidence for its source-level
// shape; see Unipo.cpp guards.
//
// Runs on LLVM IR only, as the module pass `revenant-unipo` (signature
// rewrites need a new Function object; see revenant-unabi for why a
// FunctionPassManager pass cannot do this).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNIPO_UNIPO_H
#define REVENANT_TRANSFORMS_UNIPO_UNIPO_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnipoPass : llvm::PassInfoMixin<UnipoPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
