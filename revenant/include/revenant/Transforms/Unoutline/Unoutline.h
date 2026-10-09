//===- Unoutline.h - recover outlined machine bodies -*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNOUTLINE_H
#define REVENANT_TRANSFORMS_UNOUTLINE_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnoutlinePass : public llvm::PassInfoMixin<UnoutlinePass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
