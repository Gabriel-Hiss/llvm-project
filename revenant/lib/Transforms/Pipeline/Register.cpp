//===- Register.cpp - revenant-<name> pass registration ---------------===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single registration site for every Revenant transform pass. Each pass is
// a FunctionPassManager pass named `revenant-<name>` — except the
// signature-rewriting module passes (`revenant-unabi`, `revenant-uninline`,
// `revenant-unipo`), which must run as ModulePassManager passes (erasing a
// Function while the FPM iterates the module is a use-after-free; LLVM's
// own ArgPromotion/DeadArgElim are module passes for the same reason).
// Each pass is declared in its own header under
// revenant/include/revenant/Transforms/<Name>/ and defined in
// its own library RevenantTransform<Name> (both owned by the pass agent).
// This file only wires names to classes so agents never conflict.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Pipeline/Pipeline.h"
#include "revenant/Transforms/LiftNormalize/LiftNormalize.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

// Per-pass headers. One include per pass, guarded so this file builds before
// the pass agent lands.
#if __has_include("revenant/Transforms/Devectorize/Devectorize.h")
#include "revenant/Transforms/Devectorize/Devectorize.h"
#endif
#if __has_include("revenant/Transforms/Reroll/Reroll.h")
#include "revenant/Transforms/Reroll/Reroll.h"
#endif
#if __has_include("revenant/Transforms/Unrotate/Unrotate.h")
#include "revenant/Transforms/Unrotate/Unrotate.h"
#endif
#if __has_include("revenant/Transforms/Idioms/Idioms.h")
#include "revenant/Transforms/Idioms/Idioms.h"
#endif
#if __has_include("revenant/Transforms/GsCookie/GsCookie.h")
#include "revenant/Transforms/GsCookie/GsCookie.h"
#endif
#if __has_include("revenant/Transforms/RustBlocks/RustBlocks.h")
#include "revenant/Transforms/RustBlocks/RustBlocks.h"
#endif
#if __has_include("revenant/Transforms/Patterns/Patterns.h")
#include "revenant/Transforms/Patterns/Patterns.h"
#endif
#if __has_include("revenant/Structure/RevenantStructure.h")
#include "revenant/Structure/RevenantStructure.h"
#endif
#if __has_include("revenant/Transforms/TypeRecovery/TypeRecovery.h")
#include "revenant/Transforms/TypeRecovery/TypeRecovery.h"
#endif
#if __has_include("revenant/Transforms/SsaOut/SsaOut.h")
#include "revenant/Transforms/SsaOut/SsaOut.h"
#endif
#if __has_include("revenant/Transforms/LocalSlots/LocalSlots.h")
#include "revenant/Transforms/LocalSlots/LocalSlots.h"
#endif
#if __has_include("revenant/Transforms/Unabi/Unabi.h")
#include "revenant/Transforms/Unabi/Unabi.h"
#endif
#if __has_include("revenant/Transforms/Unhoist/Unhoist.h")
#include "revenant/Transforms/Unhoist/Unhoist.h"
#endif
#if __has_include("revenant/Transforms/Uninline/Uninline.h")
#include "revenant/Transforms/Uninline/Uninline.h"
#endif
#if __has_include("revenant/Transforms/UnhotCold/UnhotCold.h")
#include "revenant/Transforms/UnhotCold/UnhotCold.h"
#endif
#if __has_include("revenant/Transforms/Unselect/Unselect.h")
#include "revenant/Transforms/Unselect/Unselect.h"
#endif
#if __has_include("revenant/Transforms/UntailDup/UntailDup.h")
#include "revenant/Transforms/UntailDup/UntailDup.h"
#endif
#if __has_include("revenant/Transforms/Uneh/Uneh.h")
#include "revenant/Transforms/Uneh/Uneh.h"
#endif
#if __has_include("revenant/Transforms/Unipo/Unipo.h")
#include "revenant/Transforms/Unipo/Unipo.h"
#endif
#if __has_include("revenant/Transforms/UnloopVec/UnloopVec.h")
#include "revenant/Transforms/UnloopVec/UnloopVec.h"
#endif
#if __has_include("revenant/Transforms/UnvecSearch/UnvecSearch.h")
#include "revenant/Transforms/UnvecSearch/UnvecSearch.h"
#endif
#if __has_include("revenant/Transforms/Unsroa/Unsroa.h")
#include "revenant/Transforms/Unsroa/Unsroa.h"
#endif
#if __has_include("revenant/Transforms/Unrust/Unrust.h")
#include "revenant/Transforms/Unrust/Unrust.h"
#endif
#if __has_include("revenant/Transforms/Uncxx/Uncxx.h")
#include "revenant/Transforms/Uncxx/Uncxx.h"
#endif
#if __has_include("revenant/Transforms/Univ/Univ.h")
#include "revenant/Transforms/Univ/Univ.h"
#endif
#if __has_include("revenant/Transforms/Unswitch/Unswitch.h")
#include "revenant/Transforms/Unswitch/Unswitch.h"
#endif
#if __has_include("revenant/Transforms/Unoutline/Unoutline.h")
#include "revenant/Transforms/Unoutline/Unoutline.h"
#endif
using namespace llvm;

namespace revenant {

void registerRevenantPasses(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "revenant-lift-normalize") {
          FPM.addPass(LiftNormalizePass());
          return true;
        }
        if (Name == "revenant-frame-slots") {
          FPM.addPass(FrameSlotsPass());
          return true;
        }
#if __has_include("revenant/Transforms/Devectorize/Devectorize.h")
        if (Name == "revenant-devectorize") {
          FPM.addPass(DevectorizePass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Reroll/Reroll.h")
        if (Name == "revenant-reroll") {
          FPM.addPass(RerollPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unrotate/Unrotate.h")
        if (Name == "revenant-unrotate") {
          FPM.addPass(UnrotatePass());
          return true;
        }
        if (Name == "revenant-untailrec") {
          FPM.addPass(UntailRecPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Idioms/Idioms.h")
        if (Name == "revenant-idioms") {
          FPM.addPass(IdiomsPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/GsCookie/GsCookie.h")
        if (Name == "revenant-gscookie") {
          FPM.addPass(GsCookiePass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/RustBlocks/RustBlocks.h")
        if (Name == "revenant-rustblocks") {
          FPM.addPass(RustBlocksPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Patterns/Patterns.h")
        if (Name == "revenant-patterns") {
          FPM.addPass(PatternsPass());
          return true;
        }
#endif
#if __has_include("revenant/Structure/RevenantStructure.h")
        if (Name == "revenant-structure") {
          FPM.addPass(RevenantStructurePass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/TypeRecovery/TypeRecovery.h")
        if (Name == "revenant-types") {
          FPM.addPass(TypeRecoveryPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/SsaOut/SsaOut.h")
        if (Name == "revenant-ssaout") {
          FPM.addPass(SsaOutPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/LocalSlots/LocalSlots.h")
        if (Name == "revenant-locals-to-slots") {
          FPM.addPass(LocalSlotsPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unhoist/Unhoist.h")
        if (Name == "revenant-unhoist") {
          FPM.addPass(UnhoistPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/UnloopVec/UnloopVec.h")
        if (Name == "revenant-unloopvec") {
          FPM.addPass(UnloopVecPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/UnvecSearch/UnvecSearch.h")
        if (Name == "revenant-unvecsearch") {
          FPM.addPass(UnvecSearchPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unselect/Unselect.h")
        if (Name == "revenant-unselect") {
          FPM.addPass(UnselectPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/UntailDup/UntailDup.h")
        if (Name == "revenant-untaildup") {
          FPM.addPass(UntailDupPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Uneh/Uneh.h")
        if (Name == "revenant-uneh") {
          FPM.addPass(UnehPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unsroa/Unsroa.h")
        if (Name == "revenant-unsroa") {
          FPM.addPass(UnsroaPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unrust/Unrust.h")
        if (Name == "revenant-unrust") {
          FPM.addPass(UnrustPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Uncxx/Uncxx.h")
        if (Name == "revenant-uncxx") {
          FPM.addPass(UncxxPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Univ/Univ.h")
        if (Name == "revenant-univ") {
          FPM.addPass(UnivPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unswitch/Unswitch.h")
        if (Name == "revenant-unswitch") {
          FPM.addPass(UnswitchPass());
          return true;
        }
#endif
        return false;
      });
  // Module passes: only passes that must erase/replace Function objects
  // (revenant-unabi/uninline/unipo, which rewrite signatures or outline
  // regions). Registered on the Module pipeline so top-level
  // `-passes=revenant-<name>` resolves without the function adaptor.
  PB.registerPipelineParsingCallback(
      [](StringRef Name, ModulePassManager &MPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "revenant-lift-abi") {
          MPM.addPass(LiftABIPass());
          return true;
        }
        if (Name == "revenant-lift-structs") {
          MPM.addPass(LiftStructsPass());
          return true;
        }
#if __has_include("revenant/Transforms/UnhotCold/UnhotCold.h")
        if (Name == "revenant-unhotcold") {
          MPM.addPass(UnhotColdPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unabi/Unabi.h")
        if (Name == "revenant-unabi") {
          MPM.addPass(UnabiPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Uninline/Uninline.h")
        if (Name == "revenant-uninline") {
          MPM.addPass(UninlinePass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unipo/Unipo.h")
        if (Name == "revenant-unipo") {
          MPM.addPass(UnipoPass());
          return true;
        }
#endif
#if __has_include("revenant/Transforms/Unoutline/Unoutline.h")
        if (Name == "revenant-unoutline") {
          MPM.addPass(UnoutlinePass());
          return true;
        }
#endif
        return false;
      });
}

} // namespace revenant
