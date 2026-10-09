//===- revenant-opt.cpp - Revenant IR pass driver, like opt ------------===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// revenant-opt is opt with every Revenant `revenant-<name>` pass linked in
// statically. Pass agents run a single pass as:
//
//   revenant-opt -passes=revenant-<name> in.ll -S -o out.ll
//
// or the whole decompiler pipeline (lift -> cleanup -> idioms -> unopt ->
// structure; lift itself is done by the driver before this tool runs) as:
//
//   revenant-opt -revenant-pipeline in.ll -S -o out.ll
//   revenant-opt -revenant-pipeline -revenant-patterns in.ll -S -o out.ll
//
// Everything else behaves exactly like opt (same flags, same -passes
// syntax, stock LLVM passes mix freely with revenant-* ones).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Pipeline/Pipeline.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

extern "C" int optMain(
    int argc, char **argv,
    ArrayRef<std::function<void(PassBuilder &)>> PassBuilderCallbacks);

static cl::opt<bool>
    RevenantPipeline("revenant-pipeline",
                     cl::desc("Run the default Revenant decompiler pipeline "
                              "(cleanup -> idioms -> unopt -> structure)"),
                     cl::init(false));

static cl::opt<bool> RevenantPatterns(
    "revenant-patterns",
    cl::desc("Include the opt-in revenant-patterns stage (only with "
             "-revenant-pipeline; matches the engine --patterns path)"),
    cl::init(false));

namespace {
// Builds the default pipeline text. Takes the patterns flag as a plain bool:
// the cl::opt is parsed inside optMain, after this text is already built, so
// reading the flag object here would always see the default.
std::string buildPassesArg(bool WithPatterns) {
  std::string P = revenant::defaultPipeline(WithPatterns);
  return P;
}
} // namespace

int main(int argc, char **argv) {
  // Register Revenant passes on every PassBuilder opt creates, and expand
  // -revenant-pipeline into a -passes value before opt parses options.
  auto RegisterRevenant = [](PassBuilder &PB) {
    revenant::registerRevenantPasses(PB);
  };

  // Scan for -revenant-pipeline early: opt's -passes default is empty, so
  // injecting "-passes=<text>" only when the user did not give -passes
  // keeps explicit pipelines working. When both are given, run the Revenant
  // pipeline first, then the user's -passes text.
  bool WantPipeline = false;
  bool WantPatterns = false;
  bool HasPasses = false;
  std::string UserPasses;
  for (int i = 1; i < argc; ++i) {
    StringRef A(argv[i]);
    if (A == "-revenant-pipeline")
      WantPipeline = true;
    if (A == "-revenant-patterns")
      WantPatterns = true;
    if (A.starts_with("-passes=")) {
      HasPasses = true;
      UserPasses = A.drop_front(strlen("-passes=")).str();
    } else if (A == "-passes" && i + 1 < argc) {
      HasPasses = true;
      UserPasses = argv[i + 1];
    }
  }

  int Ret;
  if (!WantPipeline) {
    Ret = optMain(argc, argv, {RegisterRevenant});
  } else {
    std::string PipelinePasses = buildPassesArg(WantPatterns);
    std::string Combined =
        HasPasses ? PipelinePasses + "," + UserPasses : PipelinePasses;
    // Rebuild argv: drop -revenant-pipeline* flags (opt does not know
    // them), replace/inject -passes.
    std::vector<std::string> Args;
    Args.reserve(argc + 1);
    Args.push_back(argv[0]);
    bool PassesInjected = false;
    for (int i = 1; i < argc; ++i) {
      StringRef A(argv[i]);
      if (A == "-revenant-pipeline" || A == "-revenant-patterns")
        continue;
      if (A.starts_with("-passes=")) {
        if (!PassesInjected) {
          Args.push_back("-passes=" + Combined);
          PassesInjected = true;
        }
        continue;
      }
      if (A == "-passes" && i + 1 < argc) {
        if (!PassesInjected) {
          Args.push_back("-passes=" + Combined);
          PassesInjected = true;
        }
        ++i;
        continue;
      }
      Args.push_back(argv[i]);
    }
    if (!PassesInjected)
      Args.insert(Args.begin() + 1, "-passes=" + Combined);
    errs() << "revenant-opt: running "
            << (HasPasses ? "default pipeline + user passes" : "default pipeline")
            << "\n";
    std::vector<char *> NewArgv;
    NewArgv.reserve(Args.size());
    for (auto &S : Args)
      NewArgv.push_back(S.data());
    Ret = optMain((int)NewArgv.size(), NewArgv.data(), {RegisterRevenant});
  }
  return Ret;
}
