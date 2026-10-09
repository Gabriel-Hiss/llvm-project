//===- LiftNormalize.h - machine state to compiler IR ------------*- C++ -*-===//
#ifndef REVENANT_TRANSFORMS_LIFTNORMALIZE_H
#define REVENANT_TRANSFORMS_LIFTNORMALIZE_H
#include "llvm/IR/PassManager.h"
namespace revenant {
class LiftNormalizePass : public llvm::PassInfoMixin<LiftNormalizePass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &, llvm::FunctionAnalysisManager &);
};
} // namespace revenant
#endif
