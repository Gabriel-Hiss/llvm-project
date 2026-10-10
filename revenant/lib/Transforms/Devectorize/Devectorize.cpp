//===- Devectorize.cpp - undo SLP/loop vectorization ---------------------===//
//
// Part of the Revenant LLVM fork.
//
// Port of `crates/unopt` (`lane0` + `devectorize`): fixed-width vector
// code produced by the loop vectorizer or the SLP vectorizer is rewritten
// back into the scalar operations a programmer would have written.
//
// See Devectorize.h for the phase order and the provenance guarantees.
// The pass queries no LLVM analyses (self-contained CFG utilities), so it
// needs no analysis registration from the pipeline host.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Devectorize/Devectorize.h"

#include "Lane0.h"
#include "LoopDevec.h"
#include "PairSplit.h"
#include "StraightDevec.h"

#include "llvm/IR/Function.h"

using namespace llvm;

namespace revenant {

PreservedAnalyses DevectorizePass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  // Phase 1: lane-0 scalarization exposes scalar low-lane accumulators.
  unsigned Changed = lane0::propagate(F);
  // Phase 2: single-block integer reduction loops explode into scalar
  // acc chains. Phase 3: straight-line SLP webs explode in place.
  // Phase 4: scalar i128 copy webs (loads/phis/stores of 16-byte units)
  // split into i64 halves. No CFG change in any phase.
  Changed += loopdevec::devectorizeLoops(F);
  Changed += straight::explodeStraight(F);
  Changed += pairsplit::splitPairs(F);
  if (Changed == 0)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant
// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-devectorize` name in Pipeline/Register.cpp, so the entry point
// is compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
// Windows needs the explicit dllexport; without it opt fails with
// "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantDevectorize", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-devectorize") {
                    PM.addPass(revenant::DevectorizePass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
