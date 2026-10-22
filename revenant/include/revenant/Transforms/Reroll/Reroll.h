#ifndef REVENANT_TRANSFORMS_REROLL_H
#define REVENANT_TRANSFORMS_REROLL_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct RerollPass : llvm::PassInfoMixin<RerollPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
