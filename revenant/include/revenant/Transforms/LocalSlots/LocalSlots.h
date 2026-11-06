#ifndef REVENANT_TRANSFORMS_LOCALSLOTS_H
#define REVENANT_TRANSFORMS_LOCALSLOTS_H

#include "llvm/IR/PassManager.h"

namespace revenant {

// Recreate Clang -O0 storage after type recovery, before generic SSA-out.
// `revenant-locals-to-slots<unoptimized>` applies it only to functions with
// the `revenant.o0` attribute: optimized code keeps its SSA values, as the
// compiler's IR before instruction selection does.
struct LocalSlotsPass : public llvm::PassInfoMixin<LocalSlotsPass> {
  explicit LocalSlotsPass(bool OnlyUnoptimized = false)
      : OnlyUnoptimized(OnlyUnoptimized) {}
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }

private:
  bool OnlyUnoptimized;
};

} // namespace revenant

#endif
