#ifndef REVENANT_TRANSFORMS_LOCALSLOTS_H
#define REVENANT_TRANSFORMS_LOCALSLOTS_H

#include "llvm/IR/PassManager.h"

namespace revenant {

// Recreate Clang -O0 storage after type recovery, before generic SSA-out.
struct LocalSlotsPass : public llvm::PassInfoMixin<LocalSlotsPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
