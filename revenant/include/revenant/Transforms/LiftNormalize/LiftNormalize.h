//===- LiftNormalize.h - machine state to compiler IR ------------*- C++ -*-===//
#ifndef REVENANT_TRANSFORMS_LIFTNORMALIZE_H
#define REVENANT_TRANSFORMS_LIFTNORMALIZE_H
#include "llvm/IR/PassManager.h"
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
class LiftABIPass : public llvm::PassInfoMixin<LiftABIPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &);
};
class LiftStructsPass : public llvm::PassInfoMixin<LiftStructsPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &);
};
} // namespace revenant
#endif
