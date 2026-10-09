//===- Reroll.cpp - Reverse loop unrolling ----------------------*- C++ -*-===//
//
// Part of the Revenant decompiler port, under the Apache License v2.0 with
// LLVM Exceptions. See https://llvm.org/LICENSE.txt for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `revenant-reroll`: folds unrolled loop bodies back into a single
// induction-variable step (partial unrolls) and synthesizes loops for
// fully-unrolled straight-line code (full unrolls, e.g. `-O3` unroll-by-N
// with epilogue or `-funroll-loops` targets).
//
// Ports `crates/unopt/src/reroll.rs` (`Reroll`), which runs after lane-0
// scalarization and devectorization in the Rust pipeline:
//
//   lane0 -> devectorize -> reroll -> unrotate -> tail-recursion
//
// Two shapes, mirroring the reference:
//
// - Partial (in-loop fold): a single-block loop whose IV steps by K per
//   iteration contains D identical-modulo-offset copies of one element's
//   work (same instructions, address constants advanced). Folding keeps
//   one copy and divides the step by D, so `i += 8` over 8 identical
//   `s += a[i+j]` windows becomes `i += 1` over `s += a[i]`.
// - Full (straight-line synthesis): straight-line runs of K identical
//   windows (fully-unrolled loops) are rewritten as a fresh counted loop
//   around the first window's body, with accumulator threads threaded
//   through new PHIs.
//
// Matching is shape/offset based; anything ambiguous stays put. Metadata
// contract (see RvIRContract): every rewritten instruction keeps its
// `!revenant.va` provenance, `!revenant.opaque` calls are never created
// or dropped by this pass, and `!revenant.region` annotations on header
// terminators are preserved (loop-region metadata touched by the fold is
// moved to the new latch/branch).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Reroll/Reroll.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;


namespace revenant {
namespace {

// Provenance metadata kind. RvIRContract owns the canonical spelling;
// read it dynamically so the pass works before/after Contract.h lands.
constexpr char VA_MD[] = "revenant.va";
constexpr char REGION_MD[] = "revenant.region";
constexpr char OPAQUE_MD[] = "revenant.opaque";

// Only fold steps inside this magnitude band (reference: 2..=4096).
constexpr int64_t MinStep = 2;
constexpr int64_t MaxStep = 4096;

// Synthesis needs enough evidence to be worth a loop (reference: >= 4
// windows before a loop is synthesized).
constexpr unsigned MinWindowsForSynth = 4;

// Shape key of an instruction: opcode plus type-class, mirroring the
// reference `expr::insn_kind` discriminant.
struct ShapeKey {
  unsigned Opcode = 0;
  unsigned TypeClass = 0; // 0 = void/none, else bit-width class bucket
  unsigned Extra = 0;     // predicate/call-kind discriminator

  bool operator==(const ShapeKey &O) const {
    return Opcode == O.Opcode && TypeClass == O.TypeClass && Extra == O.Extra;
  }
};



unsigned widthClass(Type *T) {
  if (T->isVoidTy())
    return 0;
  if (T->isPointerTy())
    return 4096;
  if (T->isFloatingPointTy())
    return 2048 + T->getScalarSizeInBits();
  if (T->isIntegerTy())
    return T->getIntegerBitWidth();
  if (auto *VT = dyn_cast<VectorType>(T))
    return 1024 + VT->getElementCount().getKnownMinValue();
  return 8192;
}

ShapeKey shapeOf(const Instruction &I) {
  ShapeKey K;
  K.Opcode = I.getOpcode();
  K.TypeClass = widthClass(I.getType());
  if (auto *CI = dyn_cast<CmpInst>(&I))
    K.Extra = CI->getPredicate();
  else if (auto *BO = dyn_cast<BinaryOperator>(&I))
    K.Extra = BO->getOpcode();
  else if (auto *CB = dyn_cast<CallBase>(&I)) {
    if (CB->getCalledFunction())
      K.Extra = (unsigned)CB->getCalledFunction()->getName().size() + 7;
    else
      K.Extra = CB->isIndirectCall() ? 3 : 5;
  } else if (auto *CI = dyn_cast<CastInst>(&I))
    K.Extra = CI->getOpcode();
  else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
    K.Extra = (unsigned)GEP->getNumIndices();
  return K;
}

// Copy !revenant.va (and any revenant.region on terminators) from Src to
// Dst so provenance survives the rewrite.
void copyProvenance(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
  if (MDNode *N = Src->getMetadata(REGION_MD))
    Dst->setMetadata(REGION_MD, N);
  if (MDNode *N = Src->getMetadata(OPAQUE_MD))
    Dst->setMetadata(OPAQUE_MD, N);
}



} // namespace

PreservedAnalyses RerollPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  // Skeleton: the fold and synthesis phases land in later commits.
  (void)F;
  return PreservedAnalyses::all();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin` interop. revenant-opt links
// statically and does not need it. Guarded to the standalone plugin DLL:
// a static link of every transform would otherwise export
// llvmGetPassPluginInfo more than once. Windows needs the explicit
// dllexport; without it opt fails with "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN
llvm::PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantReroll", "0.1",
          [](llvm::PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](llvm::StringRef Name, llvm::FunctionPassManager &FPM,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "revenant-reroll") {
                    FPM.addPass(revenant::RerollPass());
                    return true;
                  }
                  return false;
                });
          }};
}

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
#endif
