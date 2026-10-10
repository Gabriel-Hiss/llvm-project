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
  int64_t Step = SignedStep;
  if (BO->getOpcode() == Instruction::Sub)
    Step = -Step;
  int64_t Mag = Step >= 0 ? Step : -Step;
  if (Mag < 2 || Mag > 4096)
    return false;
  StepOut = (uint64_t)Mag;
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

// Check that all accumulator PHIs have the same preheader initial value.
bool sameInitialValue(const SmallVector<PHINode *, 4> &AccPhis) {
  if (AccPhis.empty())
    return true;
  Value *Init = AccPhis[0]->getIncomingValue(0);
  for (unsigned i = 1; i < AccPhis.size(); ++i)
    if (AccPhis[i]->getIncomingValue(0) != Init)
      return false;
  return true;
}

// Check that accumulator PHIs used outside the loop form a single combine
// tree (each PHI feeds exactly one external user, and those users form a
// connected DAG).
bool combineTreeOutside(const SmallVector<PHINode *, 4> &AccPhis,
                        BasicBlock *H) {
  SmallVector<Instruction *, 8> ExternalUsers;
  for (PHINode *P : AccPhis)
    for (User *U : P->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() != H)
        ExternalUsers.push_back(UI);
    }
  if (ExternalUsers.empty())
    return true;
  for (Instruction *I : ExternalUsers) {
    auto *BO = dyn_cast<BinaryOperator>(I);
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
  }
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
  return true;
}

// Collect all load addresses used by a value, unfolding through pure defs.
void collectLoadAddrs(Value *V, const DefLookup &DL,
                       SmallVector<GetElementPtrInst *, 8> &Addrs) {
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand()))
      Addrs.push_back(GEP);
    return;
  }
  Instruction *Def = DL.find(V);
  if (!Def)
    return;
  for (Use &U : Def->operands())
    collectLoadAddrs(U.get(), DL, Addrs);
}

// Verify that the non-chain operands of a chain's updates access memory
// with a uniform stride: each load address in window w differs from the
// corresponding address in window 0 by exactly w elements.
bool uniformAddressStride(const SmallVector<Instruction *, 8> &Chain,
                          PHINode *AccPhi, Value *IV, const DefLookup &DL) {
  int64_t D = (int64_t)Chain.size();
  if (D < 2)
    return false;

  // Collect the non-chain operand of each update.
  SmallVector<Value *, 8> Others;
  for (int64_t w = 0; w < D; ++w) {
    auto *BO = cast<BinaryOperator>(Chain[(size_t)w]);
    Value *Other = nullptr;
    for (Value *Op : BO->operands())
      if (Op != AccPhi && (w == 0 || Op != Chain[(size_t)w - 1])) {
        Other = Op;
        break;
      }
    if (!Other)
      return false;
    Others.push_back(Other);
  }

  // Collect load addresses for window 0.
  SmallVector<GetElementPtrInst *, 8> Addrs0;
  collectLoadAddrs(Others[0], DL, Addrs0);
  if (Addrs0.empty())
    return false;

  // For each subsequent window, collect load addresses and verify stride.
  for (int64_t w = 1; w < D; ++w) {
    SmallVector<GetElementPtrInst *, 8> AddrsW;
    collectLoadAddrs(Others[(size_t)w], DL, AddrsW);
    if (AddrsW.size() != Addrs0.size())
      return false;
    for (size_t i = 0; i < Addrs0.size(); ++i) {
      auto *GEP0 = Addrs0[i];
      auto *GEPW = AddrsW[i];
      if (GEP0->getPointerOperand() != GEPW->getPointerOperand())
        return false;
      if (GEP0->getNumIndices() != GEPW->getNumIndices())
        return false;
      for (unsigned j = 0; j < GEP0->getNumIndices(); ++j) {
        auto *Idx0 = dyn_cast<ConstantInt>(GEP0->idx_begin()[j]);
        auto *IdxW = dyn_cast<ConstantInt>(GEPW->idx_begin()[j]);
        if (!Idx0 || !IdxW)
          return false;
        int64_t Diff = IdxW->getSExtValue() - Idx0->getSExtValue();
        if (Diff != w)
          return false;
      }
    }
  }
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

  // Build DefLookup for affine analysis.
  DefLookup DL;
  for (Instruction &DI : *H)
    if (!DI.getType()->isVoidTy())
      DL.Map[&DI] = &DI;

  // Trace update chains for each accumulator PHI.
  SmallVector<SmallVector<Instruction *, 8>, 4> Chains;
  for (PHINode *P : AccPhis) {
    SmallVector<Instruction *, 8> Chain;
    if (!traceAccChain(P, H, Chain))
      return 0;
    Chains.push_back(Chain);
  }

  // All chains must have the same length D >= 2.
  int64_t D = (int64_t)Chains[0].size();
  if (D < 2)
    return 0;
  for (auto &Chain : Chains)
    if ((int64_t)Chain.size() != D)
      return 0;

  // The IV step must equal the fold factor D.
  if (Step != D)
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

  // Accumulators used outside the loop must form a combine tree.
  if (!combineTreeOutside(AccPhis, H))
    return 0;

  // Verify uniform address stride for each chain.
  for (auto &Chain : Chains)
    if (!uniformAddressStride(Chain, AccPhis[&Chain - &Chains[0]], IVPhi, DL))
      return 0;

  // --- Fold ---
  // Drop updates 1..D-1 from each chain.
  SmallPtrSet<Instruction *, 16> Drop;
  for (auto &Chain : Chains)
    for (size_t i = 1; i < Chain.size(); ++i)
      Drop.insert(Chain[i]);

  // Iteratively drop instructions whose uses are all dropped.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : *H) {
      if (Drop.count(&I) || isa<PHINode>(&I) || &I == StepInst ||
          I.isTerminator())
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

  // Merge accumulators into one PHI.
  IRBuilder<> B(H->begin());
  PHINode *NewPhi = B.CreatePHI(Chains[0][0]->getType(), 2, "ua_merge");
  for (unsigned i = 0; i < AccPhis.size(); ++i)
    NewPhi->addIncoming(AccPhis[i]->getIncomingValue(0),
                        AccPhis[i]->getIncomingBlock(0));

  // Combine the first updates into a single chain at the end of the block.
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
    Instruction *NewUpdate =
        B2.CreateBinOp(OpCode, Acc, Other, "ua_upd");
    copyProvenance(FirstUpdate, NewUpdate);
    Acc = NewUpdate;
  }
  NewPhi->addIncoming(Acc, H);

  // Drop the original first updates (replaced by the merged chain).
  for (auto &Chain : Chains)
    Drop.insert(Chain[0]);

  // Re-run dead-code elimination.
  Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : *H) {
      if (Drop.count(&I) || isa<PHINode>(&I) || &I == StepInst ||
          I.isTerminator())
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

  // Rewire all uses of old accumulator PHIs to the new PHI.
  for (PHINode *P : AccPhis) {
    P->replaceAllUsesWith(NewPhi);
    Drop.insert(P);
  }

  // Rewrite the IV step: D -> 1.
  if (auto *BO = dyn_cast<BinaryOperator>(StepInst)) {
    int64_t Old = cast<ConstantInt>(BO->getOperand(1))->getSExtValue();
    int64_t Sign = Old >= 0 ? 1 : -1;
    BO->setOperand(1, ConstantInt::get(BO->getOperand(1)->getType(), Sign));
  }

  // Erase dropped instructions in reverse program order.
  SmallVector<Instruction *, 16> Dead;
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
