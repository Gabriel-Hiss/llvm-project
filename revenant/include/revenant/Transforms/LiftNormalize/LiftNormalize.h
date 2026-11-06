//===- LiftNormalize.h - machine state to compiler IR ------------*- C++ -*-===//
#ifndef REVENANT_TRANSFORMS_LIFTNORMALIZE_H
#define REVENANT_TRANSFORMS_LIFTNORMALIZE_H
#include "llvm/IR/PassManager.h"
#include <vector>
namespace revenant {
class LiftNormalizePass : public llvm::PassInfoMixin<LiftNormalizePass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &, llvm::FunctionAnalysisManager &);
};
/// `revenant-frame-slots`: splits a lifted frame byte array whose addresses
/// escape into one alloca per object (see `splitFrame` in LiftNormalize.cpp),
/// the per-variable layout of a compiler's pre-optimization IR. The object
/// bounds of an escaped address are inferred from the frame's direct
/// accesses, so the ir-final storage tail runs it, not the cleanup the
/// source is built from.
class FrameSlotsPass : public llvm::PassInfoMixin<FrameSlotsPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &, llvm::FunctionAnalysisManager &);
};
/// `revenant-lift-abi`: recovers pointer parameters and returns, void and
/// narrow returns, and absolute addresses as module symbols.
/// `revenant-lift-abi(<function passes>)` then runs those passes, pass by
/// pass, over the functions it changed only: a later ABI round rewrites a
/// few hundred functions of thousands, and the rest need no new cleanup.
class LiftABIPass : public llvm::PassInfoMixin<LiftABIPass> {
public:
  LiftABIPass() = default;
  explicit LiftABIPass(std::vector<llvm::FunctionPassManager> Refine)
      : Refine(std::move(Refine)) {}
  llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &);

private:
  std::vector<llvm::FunctionPassManager> Refine;
};
class LiftStructsPass : public llvm::PassInfoMixin<LiftStructsPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &);
};
} // namespace revenant
#endif
