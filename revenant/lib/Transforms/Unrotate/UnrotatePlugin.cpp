//===- UnrotatePlugin.cpp - pass plugin entry -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `opt -load-pass-plugin` entry so the Unrotate passes work without
// revenant-opt. revenant-opt links the library statically instead and does
// not need this; keep both paths working.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unrotate/Unrotate.h"
#include "revenant/Transforms/Unrotate/UntailRec.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

using namespace llvm;

static void registerCallbacks(PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "revenant-unrotate") {
          FPM.addPass(revenant::UnrotatePass());
          return true;
        }
        if (Name == "revenant-untailrec") {
          FPM.addPass(revenant::UntailRecPass());
          return true;
        }
        return false;
      });
}

PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnrotate", LLVM_VERSION_STRING,
          registerCallbacks};
}

extern "C" __declspec(dllexport) PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
