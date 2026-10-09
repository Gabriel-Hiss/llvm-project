//===- Unhoist.h - undo LICM/GVN-hoisting/PRE -----------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Reverses loop-invariant code motion and redundant-load elimination on
// lifted code: computations the optimizer hoisted out of loops (LICM),
// loads it forwarded across stores (GVN), and expressions it merged across
// branches (GVN/PRE, cross-branch CSE) are sunk back to the blocks that
// use them, recovering the straightforward -O0-style lowering the source
// implies. See the .cpp for the phase order and the safety rules.
//
// Guarantees: only semantics-preserving rewrites; preserves !revenant.va
// on rewritten instructions; never touches !revenant.opaque calls.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNHOIST_UNHOIST_H
#define REVENANT_TRANSFORMS_UNHOIST_UNHOIST_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnhoistPass : public llvm::PassInfoMixin<UnhoistPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
