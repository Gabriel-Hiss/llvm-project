//===- UnvecMaps.cpp - inner-cascade element-wise map loops -------===//
//
// Part of the Revenant LLVM fork.
//
// Second-level map matcher. UnloopVec folds the outer dispatch of a
// vectorized element-wise loop (copy/fill/add); on AVX2/clang cascades
// an inner dispatch with its own vector tier remains:
//
//   outer done -> middle.pre -> middle.vec loop -> middle.exit
//             -> inner dispatch -> drain loop -> tail loop
//
// Two gaps keep UnloopVec's matchDispatch from folding the inner
// dispatch, both addressed here without touching UnloopVec.cpp:
//
//  1. Bypass edge: the inner S is also reachable from the outer
//     region (the middle preheader feeds S with the same 0 the
//     dispatch edge feeds). The single-entry check fails on that
//     pred. The bypass is restart-consistent: it carries the same
//     zero start, so folding D to S keeps the same scalar program.
//  2. S-phi-derived secondary start: the middle vector loop's IV
//     starts at a zext of an S phi the primary body proved is a
//     resume phi (D-incoming 0, R-incomings bound-derived). The
//     secondary start rule only accepts R-defined instructions.
//
// Both extensions reuse UnloopVec's own soundness argument: resume
// phis restart at 0 on the folded path.
//
// Scope: element-wise maps only (copy/fill/add: vector loads, splats,
// lane-wise integer adds, vector stores). No reductions, no masked
// memory ops, no FP. Anything else bails with no mutation.
//
//===----------------------------------------------------------------------===//

#include "UnloopVecCommon.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace revenant {
namespace unvecmaps {

using unloopvec::loopBound;
using unloopvec::matchScalarLoop;
using unloopvec::ScalarLoopInfo;
using unloopvec::stripCountDerivation;

// True when V is a cast chain rooted at one of the resume phis: the
// value restarts at 0 on the folded path exactly like the phi.
static bool isResumeDerived(Value *V,
                            const DenseSet<const PHINode *> &Resume,
                            unsigned Depth = 6) {
  while (V && Depth-- > 0) {
    if (auto *P = dyn_cast<PHINode>(V))
      return Resume.count(P) != 0;
    auto *C = dyn_cast<CastInst>(V);
    if (!C)
      return false;
    if (C->getOpcode() != Instruction::ZExt &&
        C->getOpcode() != Instruction::SExt &&
        C->getOpcode() != Instruction::Trunc)
      return false;
    V = C->getOperand(0);
  }
  return false;
}

// Inner-cascade S-pred check: every non-self pred of S is D, an
// R resume block, or a bypass block (outside R) whose S incoming is
// constant 0. Bypass blocks are typically the middle preheader:
// reachable from the outer region, feeding the same zero start.
// Returns false when any other pred shape appears.
static bool innerPredsOK(BasicBlock *S, BasicBlock *D,
                         const DenseSet<BasicBlock *> &R,
                         SmallVectorImpl<BasicBlock *> &Bypass) {
  for (BasicBlock *P : predecessors(S)) {
    if (P == S)
      continue;
    if (P == D || R.count(P))
      continue;
    bool ZeroFeed = false;
    for (PHINode &Phi : S->phis()) {
      int Idx = Phi.getBasicBlockIndex(P);
      if (Idx < 0)
        continue;
      auto *C = dyn_cast<ConstantInt>(Phi.getIncomingValue(Idx));
      if (C && C->isZero())
        ZeroFeed = true;
      else
        return false;
    }
    // A bypass with no phis to check is still a restart at 0 only
    // when S has no phis at all; otherwise the missing feed is
    // unproven.
    if (!ZeroFeed && !S->phis().empty())
      return false;
    Bypass.push_back(P);
  }
  return true;
}

// Countdown-drain-aware secondary acceptance. When the scalar tail L
// is a countdown drain (latch compares a decrementing count against
// 0, so loopBound yields the 0 constant), the middle tier bound is
// accepted when it is an and-mask n.vec of the same trip count N
// whose mask step equals the drain remainder width:
//   drain init = N & (K-1), middle bound = N & ~(K-1), step K.
// On the folded path the resume is 0, so the drain covers exactly
// N - (N & ~(K-1)) = N & (K-1) elements and the middle tier covers
// the rest. All arithmetic is exact unsigned; N >= 0 holds on the
// folded path (the dispatch already checked N >= 1).
static bool countdownSecondaryOK(BasicBlock *Mid, ScalarLoopInfo &V,
                                 ScalarLoopInfo &Drain, const Function &F,
                                 uint64_t Step) {
  Value *MidBound = loopBound(V);
  if (!MidBound)
    return false;
  // Middle bound must be N with low bits cleared; step from the
  // lowest set mask bit must equal the latch step.
  uint64_t Found = 0;
  Value *N = stripCountDerivation(MidBound, F);
  {
    Value *Shape = MidBound;
    while (auto *Cast = dyn_cast<CastInst>(Shape)) {
      if (Cast->getOpcode() != Instruction::ZExt &&
          Cast->getOpcode() != Instruction::SExt &&
          Cast->getOpcode() != Instruction::Trunc)
        break;
      Shape = Cast->getOperand(0);
    }
    auto *And = dyn_cast<BinaryOperator>(Shape);
    if (!And || And->getOpcode() != Instruction::And)
      return false;
    auto *C = dyn_cast<ConstantInt>(And->getOperand(1));
    if (!C)
      return false;
    unsigned K = C->getValue().countTrailingZeros();
    if (K < 1 || K >= 63)
      return false;
    if (stripCountDerivation(And->getOperand(0), F) != N)
      return false;
    Found = 1ULL << K;
  }
  if (Found != Step)
    return false;
  // Drain must latch on a countdown comparison against 0 and start
  // from (N & (Found-1)): the exact remainder the middle tier leaves.
  auto *Term = dyn_cast<BranchInst>(Drain.Header->getTerminator());
  if (!Term || !Term->isConditional())
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(Term->getCondition());
  if (!Cmp || !Cmp->isEquality())
    return false;
  bool ZeroSide = isa<ConstantInt>(Cmp->getOperand(1)) &&
                  cast<ConstantInt>(Cmp->getOperand(1))->isZero();
  if (!ZeroSide)
    return false;
  // The compared value must be the decremented count (add with -1).
  auto *Dec = dyn_cast<BinaryOperator>(Cmp->getOperand(0));
  if (!Dec || Dec->getOpcode() != Instruction::Add)
    return false;
  auto *NegOne = dyn_cast<ConstantInt>(Dec->getOperand(1));
  if (!NegOne || !NegOne->isMinusOne())
    return false;
  auto *Count = dyn_cast<PHINode>(Dec->getOperand(0));
  if (!Count || Count->getParent() != Drain.Header)
    return false;
  // The count's start value must be N & (Found-1).
  Value *Init = nullptr;
  for (unsigned I = 0; I < Count->getNumIncomingValues(); ++I) {
    if (Count->getIncomingBlock(I) == Drain.Header)
      continue;
    Init = Count->getIncomingValue(I);
  }
  if (!Init)
    return false;
  auto *InitAnd = dyn_cast<BinaryOperator>(Init);
  if (!InitAnd || InitAnd->getOpcode() != Instruction::And)
    return false;
  auto *Mask = dyn_cast<ConstantInt>(InitAnd->getOperand(1));
  if (!Mask || Mask->getZExtValue() != Found - 1)
    return false;
  return stripCountDerivation(InitAnd->getOperand(0), F) == N;
}
static bool secondaryStartOK(Value *Start, const BasicBlock *B,
                             const DenseSet<BasicBlock *> &R,
                             const DenseSet<const PHINode *> &Resume) {
  if (auto *C0 = dyn_cast<ConstantInt>(Start))
    return C0->isZero();
  if (auto *I = dyn_cast<Instruction>(Start)) {
    if (R.count(I->getParent()))
      return true;
    if (I->getParent() != B && isResumeDerived(Start, Resume))
      return true;
  }
  return false;
}

// Element-wise map body check for a middle-tier vector loop: one
// vector load, an optional splat add (fill/add) or none (copy), and
// one vector store of the same type. All memory ops simple
// (non-volatile, non-atomic, align 1).
static bool isMapBody(BasicBlock *B) {
  LoadInst *VL = nullptr;
  StoreInst *VS = nullptr;
  for (Instruction &I : *B) {
    if (isa<PHINode>(&I) || isa<BranchInst>(&I) ||
        isa<GetElementPtrInst>(&I) || isa<ICmpInst>(&I) ||
        isa<CastInst>(&I))
      continue;
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!isa<FixedVectorType>(LI->getType()) || LI->isVolatile() ||
          !LI->isSimple())
        return false;
      if (VL)
        return false;
      VL = LI;
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!isa<FixedVectorType>(SI->getValueOperand()->getType()) ||
          SI->isVolatile() || !SI->isSimple())
        return false;
      if (VS)
        return false;
      VS = SI;
      continue;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (BO->getOpcode() != Instruction::Add)
        return false;
      if (!isa<FixedVectorType>(BO->getType()))
        return false;
      continue;
    }
    if (isa<InsertElementInst>(&I) || isa<ShuffleVectorInst>(&I))
      continue;
    return false;
  }
  if (!VL || !VS)
    return false;
  return VL->getType() == VS->getValueOperand()->getType();
}

// Runs after UnloopVec in the same library: folds inner-cascade map
// dispatches UnloopVec leaves behind. Declared here; registered as
// `revenant-unvecmaps` in Pipeline/Register.cpp.
bool runUnvecMaps(llvm::Function &F);

} // namespace unvecmaps
} // namespace revenant
