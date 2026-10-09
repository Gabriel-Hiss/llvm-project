//===- Unrotate.h - undo loop rotation ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reverses LLVM-style loop rotation on lifted code: a guarded do-while
//
//   if (c) { do { body } while (c); }
//
// becomes a plain while loop when the guard and the latch test are provably
// the same condition. Ports revenant-unopt's `unrotate` pass
// (crates/unopt/src/unrotate.rs) to LLVM IR.
//
// Expects a canonical rotation shape: a self-loop block L whose only
// non-self predecessor G ends in a conditional branch to {L, X}, where X is
// also L's exit. The guard and latch conditions must be structurally equal
// once L's PHIs take their G-incoming values (or exact predicate inverses
// when the guard enters on the opposite edge). The latch condition's def
// chain must be pure (binop/cmp/cast/select/gep); anything else is left
// untouched.
//
// Rewritten shape (T keeps L's name, B is the new body block):
//
//   G: br L                  L: phis; cond chain; br c2, L.body, X
//                           L.body: rest; br L
//
// Guarantees: only fires on an airtight match; preserves !revenant.va on
// rewritten instructions; never touches !revenant.opaque calls.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNROTATE_UNROTATE_H
#define REVENANT_TRANSFORMS_UNROTATE_UNROTATE_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnrotatePass : llvm::PassInfoMixin<UnrotatePass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

struct UntailRecPass : llvm::PassInfoMixin<UntailRecPass> {
  // Undoes optimizer tail-call elimination: a TailCallElim-style loop
  // (entry split, per-argument header phis, pure latch backedge) becomes
  // the recursive call the source had. Never fires when the function
  // already contains a self-call.
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
