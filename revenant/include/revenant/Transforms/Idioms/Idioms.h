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

/// `revenant-refold`: the inverse of the intrinsic expansion above, for the
/// ir-final storage of optimized functions. Optimized LLVM canonicalizes a
/// min/max, abs, saturating add/sub or three-way compare written as
/// compare and select into its intrinsic (`llvm.umin`, `llvm.abs`,
/// `llvm.uadd.sat`, `llvm.scmp`, ...), so the IR before instruction
/// selection has the intrinsic. Unoptimized functions (`revenant.o0`) keep
/// the source's compare and select. An atomic update whose only use tests
/// its result becomes the `llvm.x86.atomic.<op>.cc` intrinsic X86's atomic
/// expansion leaves in the pre-isel IR.
struct RefoldPass : llvm::PassInfoMixin<RefoldPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
