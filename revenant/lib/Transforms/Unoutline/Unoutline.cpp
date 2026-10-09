//===- Unoutline.cpp - recover outlined machine bodies ------------------===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unoutline/Unoutline.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace {

constexpr unsigned MaxOutlinedInstructions = 12;
constexpr unsigned MinCallSites = 2;

// MachineOutliner fragments are small, single-entry straight-line functions.
// Requiring several direct users and rejecting address-taken functions avoids
// treating ordinary source helpers as compiler-generated fragments. The
// binary-derived IR has no reliable symbol name for outlined functions.
bool isOutlinedFragment(Function &F, unsigned CallSites) {
  if (F.isDeclaration() || F.isIntrinsic() || F.hasAddressTaken() ||
      F.hasFnAttribute(Attribute::Naked) ||
      CallSites < MinCallSites || F.size() != 1)
    return false;

  BasicBlock &BB = F.front();
  if (!isa<ReturnInst>(BB.getTerminator()))
    return false;

  unsigned Count = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<AllocaInst>(I) || I.isEHPad() ||
        I.getMetadata("revenant.opaque") || I.getMetadata("revenant.region"))
      return false;
    if (isa<DbgInfoIntrinsic>(I))
      continue;
    if (++Count > MaxOutlinedInstructions)
      return false;
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (CB->isInlineAsm() || !CB->getCalledFunction() ||
          CB->getCalledFunction() == &F)
        return false;
    }
  }
  return Count != 0;
}

} // namespace

PreservedAnalyses revenant::UnoutlinePass::run(Module &M,
                                                ModuleAnalysisManager &) {
  SmallVector<Function *, 16> Candidates;
  for (Function &F : M) {
    unsigned Calls = 0;
    for (User *U : F.users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != &F) {
        Calls = 0;
        break;
      }
      ++Calls;
    }
    if (isOutlinedFragment(F, Calls))
      Candidates.push_back(&F);
  }

  bool Changed = false;
  for (Function *F : Candidates) {
    SmallVector<CallBase *, 8> Sites;
    for (User *U : F->users())
      if (auto *CB = dyn_cast<CallBase>(U))
        Sites.push_back(CB);

    for (CallBase *CB : Sites) {
      InlineFunctionInfo IFI;
      InlineResult Result = InlineFunction(*CB, IFI);
      if (Result.isSuccess())
        Changed = true;
    }
    if (F->use_empty()) {
      F->eraseFromParent();
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

#if defined(REVENANT_BUILD_PLUGIN)
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"


extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantTransformUnoutline",
          LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != "revenant-unoutline")
                    return false;
                  MPM.addPass(revenant::UnoutlinePass());
                  return true;
                });
          }};
}
#endif
