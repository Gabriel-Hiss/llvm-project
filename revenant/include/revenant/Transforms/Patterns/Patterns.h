//===- Patterns.h - inline library-idiom recognition ----------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Port of `crates/patterns`: recognize inlined library routines
// (memcpy/memset/strlen loops, Rust core/alloc helpers, C++ STL helpers)
// by IR shape and replace them with calls to the named routine.
//
// Pipeline: the snippet catalog (one small source per idiom family) is
// compiled once by the lead through the official frontends into prebuilt
// IR (see docs/patterns.md for the paths); each oracle function is
// canonicalized into a shape (slots for SSA values, wildcard slots for
// repeated non-trivial constants, erased lift-unstable types, normalized
// callee needles) and matched against target functions by an
// anchor-seeded subgraph search tolerant of operand commutation,
// swappable icmp predicates and extra target code. Only matches covering
// every required instruction and meeting min_cov are reported. Contiguous
// single-block regions above the rewrite threshold become abstract
// `__pat_<helper>(args)` calls; everything else is annotation only.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_PATTERNS_H
#define REVENANT_TRANSFORMS_PATTERNS_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct PatternsPass : public llvm::PassInfoMixin<PatternsPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
