//===- Unipo.h - undo interprocedural (IPO) optimizations -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Inverts one family of LLVM IPO transforms on lifted IR: DeadArgElim's
// return elimination (i32/i64 -> void when every caller drops the value)
// and dead-argument removal (a trailing formal nobody reads), IPSCCP/
// function-specialization clones (`.constprop.N`, `.isra.N`,
// `.specialized.N`, `.lto_priv`, `.llvm.N` folded back under the base
// name with the constant as an argument), and ArgPromotion's by-value
// struct splitting. Each rewrite is a semantics-preserving equivalence;
// see Unipo.cpp for the per-shape guards.
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
