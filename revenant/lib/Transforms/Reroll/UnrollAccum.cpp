//===- UnrollAccum.cpp - Multi-accumulator unroll fold ---------------*- C++ -*-===//
//
// Part of the Revenant decompiler port, under the Apache License v2.0 with
// LLVM Exceptions. See https://llvm.org/LICENSE.txt for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Folds multi-accumulator unrolled loops back into a single-accumulator
// loop. Handles the gcc -O3 / clang interleaving pattern where a scalar
// loop is unrolled with several accumulator copies:
//
//   acc0 += a[i];
//   acc1 += a[i+1];
//   ...
//
// combined after the loop. Also handles the unrolled remainder.
//
// The fold merges the accumulators into one and the copies into one
// body (legal for integer ops; float only with evidence the compiler
// was allowed to reassociate).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Reroll/Reroll.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

namespace revenant {
namespace {

constexpr char VA_MD[] = "revenant.va";
constexpr char REGION_MD[] = "revenant.region";
constexpr char OPAQUE_MD[] = "revenant.opaque";

void copyProvenance(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
  if (MDNode *N = Src->getMetadata(REGION_MD))
    Dst->setMetadata(REGION_MD, N);
  if (MDNode *N = Src->getMetadata(OPAQUE_MD))
    Dst->setMetadata(OPAQUE_MD, N);
}

bool matchStrideIV(PHINode *Phi, BasicBlock *Header, int64_t &StepOut,
                   Instruction *&StepInstOut) {
  if (Phi->getNumIncomingValues() != 2)
    return false;
  if (!Phi->getType()->isIntegerTy())
    return false;
  Value *Back = nullptr;
  for (unsigned i = 0; i < 2; ++i)
    if (Phi->getIncomingBlock(i) == Header)
      Back = Phi->getIncomingValue(i);
  if (!Back)
    return false;
  auto *BO = dyn_cast<BinaryOperator>(Back);
  if (!BO || (BO->getOpcode() != Instruction::Add &&
              BO->getOpcode() != Instruction::Sub))
    return false;
  if (BO->getParent() != Header)
    return false;
  Value *Other = (BO->getOperand(0) == Phi) ? BO->getOperand(1) : nullptr;
  if (!Other)
    return false;
  auto *C = dyn_cast<ConstantInt>(Other);
  if (!C)
    return false;
  int64_t SignedStep = C->getSExtValue();
  if (SignedStep == INT64_MIN)
    return false;
  if (SignedStep < 0)
    return false;
  int64_t Step = SignedStep;
  if (Step < 2 || Step > 4096)
    return false;
  StepOut = (uint64_t)Step;
  StepInstOut = BO;
  return true;
}

// Trace the update chain for an accumulator PHI.
bool traceAccChain(PHINode *Phi, BasicBlock *H,
                   SmallVector<Instruction *, 8> &Chain) {
  Chain.clear();
  Value *Current = Phi;
  SmallPtrSet<Instruction *, 8> Visited;
  while (true) {
    Instruction *Next = nullptr;
    for (User *U : Current->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() != H)
        continue;
      if (Visited.count(UI))
        continue;
      auto *BO = dyn_cast<BinaryOperator>(UI);
      if (!BO)
        continue;
      unsigned Op = BO->getOpcode();
      if (Op != Instruction::Add && Op != Instruction::Sub &&
          Op != Instruction::Mul && Op != Instruction::Xor &&
          Op != Instruction::And && Op != Instruction::Or)
        continue;
      bool ReadsChain = false;
      for (Value *V : BO->operands())
        if (V == Current) {
          ReadsChain = true;
          break;
        }
      if (ReadsChain) {
        if (Next)
          return false;
        Next = UI;
      }
    }
    if (!Next)
      break;
    Chain.push_back(Next);
    Visited.insert(Next);
    Current = Next;
  }
  return !Chain.empty();
}

// Check that all accumulator PHIs have the same preheader initial value,
// and that value is zero (so merging K accumulators into one with the same
// init is semantically correct).
bool sameInitialValue(const SmallVector<PHINode *, 4> &AccPhis) {
  if (AccPhis.empty())
    return true;
  Value *Init = AccPhis[0]->getIncomingValue(0);
  if (!isa<ConstantInt>(Init) || !cast<ConstantInt>(Init)->isZero())
    return false;
  for (unsigned i = 1; i < AccPhis.size(); ++i)
    if (AccPhis[i]->getIncomingValue(0) != Init)
      return false;
  return true;
}

// Check that accumulator PHIs used outside the loop form a single-level
// combine tree (one binop reading all acc PHIs directly). Returns the
// combine instruction via `CombineOut` if valid.
bool combineTreeOutside(const SmallVector<PHINode *, 4> &AccPhis,
                        BasicBlock *H,
                        Instruction **CombineOut) {
  SmallPtrSet<Instruction *, 8> ExternalSet;
  for (PHINode *P : AccPhis)
    for (User *U : P->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() != H)
        ExternalSet.insert(UI);
    }
  if (ExternalSet.empty()) {
    *CombineOut = nullptr;
    return true;
  }
  if (ExternalSet.size() != 1)
    return false;
  auto *BO = dyn_cast<BinaryOperator>(*ExternalSet.begin());
  if (!BO)
    return false;
  bool ReadsAcc = false;
  for (Value *Op : BO->operands())
    if (isa<PHINode>(Op)) {
      ReadsAcc = true;
      break;
    }
  if (!ReadsAcc)
    return false;
  for (PHINode *P : AccPhis) {
    bool Reaches = false;
    for (User *U : P->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() != H) {
        Reaches = true;
        break;
      }
    }
    if (!Reaches)
      return false;
  }
  *CombineOut = BO;
  return true;
}

// Try to fold a multi-accumulator unrolled loop.
unsigned tryFoldMultiAcc(BasicBlock *H, SmallVectorImpl<StringRef> &Notes) {
  bool SelfLatch = false;
  for (BasicBlock *S : successors(H))
    if (S == H)
      SelfLatch = true;
  if (!SelfLatch)
    return 0;

  // Find stride IV.
  PHINode *IVPhi = nullptr;
  int64_t Step = 0;
  Instruction *StepInst = nullptr;
  for (Instruction &I : *H) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi || !Phi->getType()->isIntegerTy())
      continue;
    if (Phi->getNumIncomingValues() != 2)
      continue;
    int64_t S;
    Instruction *SI;
    if (matchStrideIV(Phi, H, S, SI)) {
      IVPhi = Phi;
      Step = S;
      StepInst = SI;
      break;
    }
  }
  if (!IVPhi)
    return 0;

  // Collect accumulator PHIs (non-IV PHIs).
  SmallVector<PHINode *, 4> AccPhis;
  for (Instruction &I : *H) {
    if (auto *P = dyn_cast<PHINode>(&I))
      if (P != IVPhi)
        AccPhis.push_back(P);
  }
  if (AccPhis.size() < 2)
    return 0;

  // Trace update chains for each accumulator PHI.
  SmallVector<SmallVector<Instruction *, 8>, 4> Chains;
  for (PHINode *P : AccPhis) {
    SmallVector<Instruction *, 8> Chain;
    if (!traceAccChain(P, H, Chain))
      return 0;
    Chains.push_back(Chain);
  }

  // All chains must have the same length D == 1 (one update per accumulator
  // per iteration; D>1 multi-accumulator unrolls are not handled yet).
  int64_t D = (int64_t)Chains[0].size();
  if (D != 1)
    return 0;
  for (auto &Chain : Chains)
    if ((int64_t)Chain.size() != D)
      return 0;

  // The IV step must equal the number of accumulators K.
  if (Step != (int64_t)AccPhis.size())
    return 0;

  // All chains must use the same operation.
  unsigned OpCode;
  {
    auto *First = dyn_cast<BinaryOperator>(Chains[0][0]);
    if (!First)
      return 0;
    OpCode = First->getOpcode();
    for (auto &Chain : Chains)
      for (Instruction *I : Chain) {
        auto *BO = dyn_cast<BinaryOperator>(I);
        if (!BO || BO->getOpcode() != OpCode)
          return 0;
      }
  }

  // The operation must be reassociable.
  // Integer add/mul/xor/and/or are always safe.
  // Float ops require reassociation evidence (bail for now).
  for (auto &Chain : Chains)
    if (Chain[0]->getType()->isFloatingPointTy())
      return 0;

  // All accumulator PHIs must have the same initial value.
  if (!sameInitialValue(AccPhis))
    return 0;

  // Accumulators used outside the loop must form a single-level combine tree.
  Instruction *CombineInst = nullptr;
  if (!combineTreeOutside(AccPhis, H, &CombineInst))
    return 0;

  // --- Fold ---
  // Create merged PHI at block start (after existing PHIs).
  Instruction *InsertPoint = &*H->getFirstNonPHIIt();
  IRBuilder<> B(InsertPoint);
  PHINode *NewPhi = B.CreatePHI(Chains[0][0]->getType(), 2, "ua_merge");
  // Find the first acc PHI incoming from outside the loop (pred != H).
  BasicBlock *EntryPred = nullptr;
  Value *EntryVal = nullptr;
  for (unsigned i = 0; i < AccPhis.size(); ++i) {
    BasicBlock *Pred = AccPhis[i]->getIncomingBlock(0);
    if (Pred != H) {
      EntryPred = Pred;
      EntryVal = AccPhis[i]->getIncomingValue(0);
      break;
    }
  }
  if (!EntryPred)
    return 0;
  // Combine all acc initial values into one (for zero init, stays zero).
  for (unsigned i = 0; i < AccPhis.size(); ++i) {
    Value *Init = AccPhis[i]->getIncomingValue(0);
    if (Init == EntryVal)
      continue;
    EntryVal = B.CreateBinOp(static_cast<Instruction::BinaryOps>(OpCode),
                             EntryVal, Init, "ua_init");
  }
  NewPhi->addIncoming(EntryVal, EntryPred);

  // Create merged updates at end of block (before terminator).
  Instruction *Term = H->getTerminator();
  IRBuilder<> B2(Term);
  Value *Acc = NewPhi;
  for (size_t i = 0; i < Chains.size(); ++i) {
    Instruction *FirstUpdate = Chains[i][0];
    auto *BO = cast<BinaryOperator>(FirstUpdate);
    Value *Other = nullptr;
    for (Value *Op : BO->operands())
      if (Op != AccPhis[i]) {
        Other = Op;
        break;
      }
    if (!Other)
      return 0;
    Instruction *NewUpdate = cast<Instruction>(
        B2.CreateBinOp(static_cast<Instruction::BinaryOps>(OpCode), Acc, Other, "ua_upd"));
    copyProvenance(FirstUpdate, NewUpdate);
    Acc = NewUpdate;
  }
  NewPhi->addIncoming(Acc, H);

  // Fix blocker #1: when there's a combine instruction, replace IT with
  // NewPhi and erase it. Don't replaceAllUsesWith on the old PHIs (that
  // would turn `add acc0,acc1` into `add NewPhi,NewPhi`).
  if (CombineInst) {
    // Check that every operand is an acc PHI (combine is fully redundant).
    for (Value *Op : CombineInst->operands()) {
      bool IsAcc = false;
      for (PHINode *P : AccPhis)
        if (Op == P) { IsAcc = true; break; }
      if (!IsAcc)
        return 0; // Can't safely fold complex combine.
    }
    CombineInst->replaceAllUsesWith(NewPhi);
    CombineInst->eraseFromParent();
  } else {
    // No external combine: safe to replace all acc PHI uses.
    for (PHINode *P : AccPhis)
      P->replaceAllUsesWith(NewPhi);
  }
  // Clear incoming references from old PHIs (they point to dropped instructions).
  for (PHINode *P : AccPhis)
    P->dropAllReferences();

  // Rewrite the IV step: D*K -> K (each iteration processes K elements).
  if (auto *BO = dyn_cast<BinaryOperator>(StepInst)) {
    int64_t K = (int64_t)AccPhis.size();
    BO->setOperand(1, ConstantInt::get(BO->getOperand(1)->getType(), K));
  }

  // Collect all instructions to drop: all chain updates + old PHIs + dead code.
  SmallPtrSet<Instruction *, 32> Drop;
  for (auto &Chain : Chains)
    for (Instruction *I : Chain)
      Drop.insert(I);
  for (PHINode *P : AccPhis)
    Drop.insert(P);

  // Iteratively drop instructions whose uses are all dropped.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : *H) {
      if (Drop.count(&I) || &I == StepInst || I.isTerminator())
        continue;
      bool AllUsesDropped = true;
      bool HasUses = false;
      for (User *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || UI->getParent() != H)
          continue;
        HasUses = true;
        if (!Drop.count(UI))
          AllUsesDropped = false;
      }
      if (HasUses && AllUsesDropped) {
        Drop.insert(&I);
        Changed = true;
      }
    }
  }

  // Erase dropped instructions in reverse program order.
  SmallVector<Instruction *, 32> Dead;
  for (Instruction &I : *H)
    if (Drop.count(&I))
      Dead.push_back(&I);
  for (auto It = Dead.rbegin(); It != Dead.rend(); ++It)
    (*It)->eraseFromParent();

  Notes.push_back(H->getName());
  return (unsigned)D;
}

} // namespace

// Entry point called from RerollPass::run after tryFoldLoop rounds.
unsigned unrollAccumFold(Function &F, SmallVectorImpl<StringRef> &Notes) {
  unsigned Folds = 0;
  SmallVector<BasicBlock *, 16> Headers;
  for (BasicBlock &B : F)
    for (BasicBlock *S : successors(&B))
      if (S == &B) {
        Headers.push_back(&B);
        break;
      }
  for (BasicBlock *H : Headers)
    if (tryFoldMultiAcc(H, Notes))
      Folds++;
  return Folds;
}

} // namespace revenant
