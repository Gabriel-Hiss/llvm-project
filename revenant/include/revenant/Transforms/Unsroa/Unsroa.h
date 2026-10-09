//===- Unsroa.h - rebuild aggregates SROA/instcombine split -*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Rebuilds aggregate allocas and accesses that SROA plus instcombine split
// apart, so lifted IR reads like the compiler's pre-optimization IR again:
//
//   1. Scalar groups back to struct/array allocas with GEPs: a run of scalar
//      allocas loaded from / stored to adjacent constant offsets of one base
//      object folds back into one aggregate alloca plus field GEPs.
//   2. Scalarized struct copies back to memcpy/struct copies: a run of
//      scalar load/store pairs copying adjacent fields between two objects
//      folds back into one llvm.memcpy (or a single aggregate load/store
//      when the whole object moves through a value).
//   3. Split struct args/returns: extractvalue/insertvalue webs that spread
//      one aggregate value across scalar SSA names rebuild the aggregate
//      value (insertvalue chain) at the def and field extracts at the uses.
//
// Layouts come from the function itself: constant-offset GEP footprints
// grouped by base pointer, checked against the module DataLayout. Groups
// whose offsets/sizes disagree, overlap, or touch volatile/atomic accesses
// are left alone. The recovered (base -> field list) map uses the same
// shape TypeRecovery infers (two or more distinct constant offsets through
// one base), so the two passes agree on what a struct is.
//
// Guarantees: only fires on airtight adjacent-offset groups; preserves
// !revenant.va on every new instruction; never touches !revenant.opaque
// calls or !revenant.region terminators.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNSROA_UNSROA_H
#define REVENANT_TRANSFORMS_UNSROA_UNSROA_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnsroaPass : public llvm::PassInfoMixin<UnsroaPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
