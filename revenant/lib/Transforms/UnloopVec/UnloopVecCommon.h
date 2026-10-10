//===- UnloopVecCommon.h - shared loop-shape helpers ------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Helpers shared by UnloopVec.cpp and UnvecMaps.cpp: single-block
// counted-loop match, latch-bound extraction, and trip-count root
// stripping. Behavior-identical to the code formerly in UnloopVec.cpp's
// anonymous namespace; moved here so the two map matchers cannot drift.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNLOOPVEC_COMMON_H
#define REVENANT_TRANSFORMS_UNLOOPVEC_COMMON_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace revenant {
namespace unloopvec {

using namespace llvm;

struct ScalarLoopInfo {
  BasicBlock *Header = nullptr; // single-block self-loop
  PHINode *IV = nullptr;        // steps by constant
  ConstantInt *Step = nullptr;
  Value *TripCountN = nullptr; // exit bound derives from this
  SmallVector<PHINode *, 2> AccPhis;
};

// Match a single-block self-loop latch with a constant-step IV:
//   H: iv = phi [start, P], [iv.next, H]; ...; iv.next = add iv, C;
//      br cond, H, Exit (or inverted)
inline bool matchScalarLoop(BasicBlock *H, ScalarLoopInfo &Out) {
  auto *Term = dyn_cast<BranchInst>(H->getTerminator());
  if (!Term || !Term->isConditional())
    return false;
  bool SelfEdge = false;
  for (BasicBlock *Succ : successors(H))
    if (Succ == H)
      SelfEdge = true;
  if (!SelfEdge)
    return false;
  // Exactly two successors, one is self.
  if (Term->getSuccessor(0) == Term->getSuccessor(1))
    return false;
  // Find the IV: a phi with one incoming from H that is add(phi, const).
  for (PHINode &Phi : H->phis()) {
    Value *BackVal = nullptr;
    Value *StartVal = nullptr;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      if (Phi.getIncomingBlock(I) == H)
        BackVal = Phi.getIncomingValue(I);
      else
        StartVal = Phi.getIncomingValue(I);
    }
    if (!BackVal || !StartVal)
      continue;
    auto *Add = dyn_cast<BinaryOperator>(BackVal);
    if (!Add)
      continue;
    if (Add->getOpcode() != Instruction::Add &&
        Add->getOpcode() != Instruction::Sub)
      continue;
    if (Add->getParent() != H)
      continue;
    ConstantInt *C = nullptr;
    if (Add->getOperand(0) == &Phi &&
        (C = dyn_cast<ConstantInt>(Add->getOperand(1)))) {
      if (Add->getOpcode() == Instruction::Sub)
        continue; // iv - C is not a latch step
      Out.Header = H;
      Out.IV = &Phi;
      Out.Step = C;
      break;
    }
  }
  return Out.Header != nullptr;
}

// The latch exit condition compares something derived from the IV
// against a bound value; return that bound value. The IV chain is the
// IV phi plus its in-latch step add; anything else on either side of
// the icmp is the bound. Multi-IV tails compare a sibling count phi
// that also steps by a constant, so sibling constant-step phis count
// as IV chains too.
inline Value *loopBound(ScalarLoopInfo &L) {
  auto *Term = cast<BranchInst>(L.Header->getTerminator());
  Value *Cond = Term->getCondition();
  auto *Cmp = dyn_cast<ICmpInst>(Cond);
  if (!Cmp)
    return nullptr;
  auto IsCountedPhi = [&](Value *V) {
    auto *P = dyn_cast<PHINode>(V);
    if (!P || P->getParent() != L.Header)
      return false;
    Value *Back = nullptr;
    for (unsigned I = 0; I < P->getNumIncomingValues(); ++I)
      if (P->getIncomingBlock(I) == L.Header)
        Back = P->getIncomingValue(I);
    auto *Add = dyn_cast<BinaryOperator>(Back);
    if (!Add || Add->getParent() != L.Header)
      return false;
    // Same step rule as matchScalarLoop: phi + constant.
    if (Add->getOpcode() != Instruction::Add)
      return false;
    return Add->getOperand(0) == P &&
           isa<ConstantInt>(Add->getOperand(1));
  };
  auto IsIVChain = [&](Value *V) {
    if (V == L.IV || IsCountedPhi(V))
      return true;
    if (auto *BO = dyn_cast<BinaryOperator>(V)) {
      if (BO->getParent() != L.Header)
        return false;
      if (BO->getOpcode() != Instruction::Add &&
          BO->getOpcode() != Instruction::Sub)
        return false;
      return IsCountedPhi(BO->getOperand(0)) ||
             IsCountedPhi(BO->getOperand(1)) || BO->getOperand(0) == L.IV ||
             BO->getOperand(1) == L.IV;
    }
    return false;
  };
  bool Op0IV = IsIVChain(Cmp->getOperand(0));
  bool Op1IV = IsIVChain(Cmp->getOperand(1));
  if (Op0IV && !Op1IV)
    return Cmp->getOperand(1);
  if (Op1IV && !Op0IV)
    return Cmp->getOperand(0);
  return nullptr;
}

// Follow a value through pure in-block and single-def chains to find
// the trip count root: strips zext/sext/trunc, add/sub/and with
// constants, and single-use chains. Returns the outermost
// non-derivable value.
inline Value *stripCountDerivation(Value *V, const Function &F,
                                   unsigned Depth = 8) {
  (void)F;
  while (V && Depth-- > 0) {
    auto *I = dyn_cast<Instruction>(V);
    if (!I)
      break;
    switch (I->getOpcode()) {
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::Trunc:
      V = I->getOperand(0);
      continue;
    case Instruction::Shl:
    case Instruction::LShr: {
      // Byte-scaled forms (shl(zext(lshr(N, K)), M)) still derive
      // from the same trip-count root.
      auto *C1 = dyn_cast<ConstantInt>(I->getOperand(1));
      if (C1) {
        V = I->getOperand(0);
        continue;
      }
      return V;
    }
    case Instruction::And:
    case Instruction::Sub:
    case Instruction::Add: {
      // Keep the non-constant side when the other is constant.
      auto *C0 = dyn_cast<ConstantInt>(I->getOperand(0));
      auto *C1 = dyn_cast<ConstantInt>(I->getOperand(1));
      if (C1 && !C0) {
        V = I->getOperand(0);
        continue;
      }
      if (C0 && !C1 && I->getOpcode() == Instruction::Add) {
        V = I->getOperand(1);
        continue;
      }
      return V;
    }
    case Instruction::URem:
      // N mod K: trip count is operand 0.
      V = I->getOperand(0);
      continue;
    default:
      return V;
    }
  }
  return V;
}

} // namespace unloopvec
} // namespace revenant

#endif
