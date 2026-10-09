//===- Pipeline.cpp - default pipeline text ---------------------------===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Builds the default decompiler pipeline (engine order: lift -> cleanup ->
// idioms -> unopt -> structure) by parsing defaultPipeline() text. Parsing
// instead of hand-adding passes keeps the in-process pipeline and the
// revenant-opt command line on one definition that cannot drift.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Pipeline/Pipeline.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Error.h"

using namespace llvm;

namespace revenant {

std::string defaultPipeline(bool withPatterns) {
  std::string P;
  P += "function(";
  P += CleanupPipeline;
  P += ",";
  P += IdiomsPipeline;
  P += ",";
  P += UnoptPipeline;
  if (withPatterns)
    P += ",revenant-patterns";
  P += ",revenant-structure)";
  return P;
}

void addDefaultPipeline(FunctionPassManager &FPM, bool withPatterns) {
  PassBuilder PB;
  registerRevenantPasses(PB);
  ExitOnError Err("revenant: cannot build default pipeline: ");
  Err(PB.parsePassPipeline(FPM, defaultPipeline(withPatterns)));
}

void addDefaultModulePipeline(ModulePassManager &MPM, bool withPatterns) {
  PassBuilder PB;
  registerRevenantPasses(PB);
  ExitOnError Err("revenant: cannot build default pipeline: ");
  Err(PB.parsePassPipeline(MPM, defaultPipeline(withPatterns)));
}

} // namespace revenant
