//===- UntailRec.h - tail-recursion reversal ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Turns self tail-calls back into the loop the source had: `return f(args)`
// (or a void self-call before `ret void`, or a self-call before
// `unreachable`) becomes `params = args; br head`, with a parameter PHI per
// argument in the loop header. Ports revenant-unopt's `tail_rec` pass
// (crates/unopt/src/tail_rec.rs) to LLVM IR.
//
// Only exact tail calls are taken: a direct call to the current function
// with a matching argument count and return type, sitting immediately before
// the terminator, whose result (if any) is returned directly. Anything else
// stays recursive.
//
// Rewritten shape (head takes the old entry's name with a `.lp` suffix, the
// new entry just branches to it):
//
//   entry: br entry.lp       entry.lp: p = phi [p.in, entry], [a, tail] ...
//
// Guarantees: only fires on exact tail calls; preserves !revenant.va on
// rewritten instructions; never touches !revenant.opaque calls.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNROTATE_UNTAILREC_H
#define REVENANT_TRANSFORMS_UNROTATE_UNTAILREC_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UntailRecPass : llvm::PassInfoMixin<UntailRecPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
