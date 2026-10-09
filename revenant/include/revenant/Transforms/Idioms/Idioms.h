//===-- Idioms.h - compiler-idiom reversal pass -------------------*- C++ -*-===//
// Part of the Revenant decompiler port. Reverse strength-reduced and
// idiomatic IR back into the operations the source program had.
//
// Owns the `revenant-idioms` Function pass. See the `crates/idioms`
// reference implementation (magic_div.rs, min_max.rs) for the shapes.
#ifndef REVENANT_TRANSFORMS_IDIOMS_H
#define REVENANT_TRANSFORMS_IDIOMS_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct IdiomsPass : llvm::PassInfoMixin<IdiomsPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
