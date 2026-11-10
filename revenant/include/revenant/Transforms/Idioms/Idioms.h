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

/// `revenant-copyruns`: the backend expands a small fixed-size `llvm.memcpy`
/// into loads and stores that move one contiguous byte range between two
/// bases; compiler IR at every optimization level carries the intrinsic.
/// A run of at least 16 bytes whose loads each feed exactly one store at
/// the same relative offset, with non-overlapping ranges, folds back. A
/// fixed-size `llvm.memset` expands the same way: in optimized functions,
/// stores of one repeated byte that cover at least 16 contiguous bytes of
/// one base fold back too (unoptimized code stores each field itself).
struct CopyRunsPass : llvm::PassInfoMixin<CopyRunsPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

/// `revenant-prune-eh`: an optimized compiler's IR calls a function it knows
/// cannot unwind with `call`, not `invoke`, and keeps no cleanup for it. The
/// lift sees every call inside a protected range as possibly unwinding. The
/// pass infers which lifted functions cannot unwind (no `resume`, no pad
/// exit to the caller, no call outside an invoke to a callee that may), then
/// turns each invoke of such a callee into a call and drops the pads only
/// those invokes reached. It infers nothing else: memory effects would let
/// later folding delete calls the lift cannot prove dead. Unoptimized
/// functions (`revenant.o0`) keep their invokes: an unoptimized front end
/// does not infer `nounwind`.
struct PruneEHPass : llvm::PassInfoMixin<PruneEHPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
