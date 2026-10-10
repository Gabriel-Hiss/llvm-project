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

// Pass-local candidate: the inner dispatch D, its scalar entry S,
// the countdown drain L plus the counted tail T, the middle vector
// tier Mid with its preheader, and the region R to erase (middle
// preheader-through-exit plus the outer dispatch blocks that only
// route into it).
struct InnerCascade {
  BasicBlock *D = nullptr;
  BasicBlock *S = nullptr;
  BasicBlock *Mid = nullptr;
  BasicBlock *MidPre = nullptr;
  SmallVector<BasicBlock *, 2> MidBypass;
  ScalarLoopInfo MidLoop;
  BasicBlock *L = nullptr; // countdown drain
  ScalarLoopInfo DrainLoop;
  BasicBlock *T = nullptr; // counted tail
  ScalarLoopInfo TailLoop;
  DenseSet<BasicBlock *> R;
};
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
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (isa<FixedVectorType>(BO->getType())) {
        // Lane-wise vector arithmetic: only add is a map.
        if (BO->getOpcode() != Instruction::Add)
          return false;
      }
      // Scalar ALU (address math, IV steps, bounds) is fine.
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
    if (isa<InsertElementInst>(&I) || isa<ShuffleVectorInst>(&I))
      continue;
    return false;
  }
  if (!VL || !VS)
    return false;
  return VL->getType() == VS->getValueOperand()->getType();
}

// Match the inner cascade around dispatch D with scalar entry S:
// S routes to a countdown drain L and a counted tail T; the other
// arm reaches a middle vector tier Mid (single-block self-loop with
// a preheader) whose bound the countdown check accepts against the
// drain. Region R covers the middle preheader, tier, and exit plus
// any dispatch blocks between D and the preheader. Returns true with
// C filled on a full structural match; no mutation.
static bool matchInnerCascade(BasicBlock *D, BasicBlock *S, Function &F,
                              InnerCascade &C) {
  // S must route to exactly the drain and the tail entries.
  auto *SBr = dyn_cast<BranchInst>(S->getTerminator());
  if (!SBr || !SBr->isConditional())
    return false;
  BasicBlock *S0 = SBr->getSuccessor(0);
  BasicBlock *S1 = SBr->getSuccessor(1);
  // One successor is the countdown drain (single-block self-loop
  // whose latch compares against 0); the other reaches the counted
  // tail. Try both orders.
  for (int Pick = 0; Pick < 2; ++Pick) {
    BasicBlock *MaybeDrain = Pick == 0 ? S0 : S1;
    BasicBlock *MaybeTailEntry = Pick == 0 ? S1 : S0;
    ScalarLoopInfo Drain;
    if (!matchScalarLoop(MaybeDrain, Drain))
      continue;
    // Drain latch must compare against constant 0 (countdown form).
    Value *DrainBound = loopBound(Drain);
    auto *Zero = dyn_cast_or_null<ConstantInt>(DrainBound);
    if (!Zero || !Zero->isZero())
      continue;
    // The tail entry is either the counted tail itself or a block
    // leading to it; find the single-block counted self-loop below.
    ScalarLoopInfo Tail;
    BasicBlock *TailBB = nullptr;
    if (matchScalarLoop(MaybeTailEntry, Tail)) {
      TailBB = MaybeTailEntry;
    } else {
      SmallVector<BasicBlock *, 4> Work{MaybeTailEntry};
      DenseSet<BasicBlock *> Seen{MaybeTailEntry};
      while (!Work.empty() && !TailBB) {
        BasicBlock *BB = Work.pop_back_val();
        if (BB != MaybeTailEntry && matchScalarLoop(BB, Tail)) {
          TailBB = BB;
          break;
        }
        for (BasicBlock *Su : successors(BB))
          if (Seen.insert(Su).second)
            Work.push_back(Su);
        if (Seen.size() > 32)
          break;
      }
    }
    if (!TailBB)
      continue;
    // The middle tier: the non-S arm of D reaches a preheader that
    // feeds a single-block vector self-loop.
    auto *DBr = dyn_cast<BranchInst>(D->getTerminator());
    if (!DBr || !DBr->isConditional())
      continue;
    BasicBlock *V = DBr->getSuccessor(0) == S ? DBr->getSuccessor(1)
                                              : DBr->getSuccessor(0);
    // Walk from V to the middle preheader: a block whose single
    // successor is a vector self-loop.
    BasicBlock *MidPre = V;
    BasicBlock *Mid = nullptr;
    ScalarLoopInfo MidLoop;
    for (unsigned Walk = 0; Walk < 8 && MidPre && !Mid; ++Walk) {
      for (BasicBlock *Su : successors(MidPre)) {
        if (Su == S || Su == MaybeDrain || Su == TailBB)
          continue;
        ScalarLoopInfo Loop;
        if (matchScalarLoop(Su, Loop) && isMapBody(Su)) {
          Mid = Su;
          MidLoop = Loop;
          break;
        }
      }
      if (Mid)
        break;
      // Step through unconditional single-successor blocks.
      auto *WBr = dyn_cast<BranchInst>(MidPre->getTerminator());
      if (!WBr || WBr->isConditional())
        break;
      MidPre = WBr->getSuccessor(0);
      if (MidPre == S || MidPre == MaybeDrain || MidPre == TailBB)
        break;
    }
    if (!Mid)
      continue;
    // The middle preheader may have outer-region preds (the outer
    // dispatch's small-N arm) when each feeds constant 0 to its
    // phis: the bypass restart, same zero the dispatch edge feeds.
    // Recorded for re-routing in the fold.
    SmallVector<BasicBlock *, 2> MidBypass;
    for (BasicBlock *P : predecessors(MidPre)) {
      if (P == D)
        continue;
      bool ZeroFeed = true;
      for (PHINode &Phi : MidPre->phis()) {
        int Idx = Phi.getBasicBlockIndex(P);
        if (Idx < 0)
          continue;
        auto *C0 = dyn_cast<ConstantInt>(Phi.getIncomingValue(Idx));
        if (!C0 || !C0->isZero())
          ZeroFeed = false;
      }
      if (!ZeroFeed)
        return false;
      MidBypass.push_back(P);
    }
    C.D = D;
    C.S = S;
    C.Mid = Mid;
    C.MidPre = MidPre;
    C.MidBypass.assign(MidBypass.begin(), MidBypass.end());
    C.MidLoop = MidLoop;
    C.L = MaybeDrain;
    C.DrainLoop = Drain;
    C.T = TailBB;
    C.TailLoop = Tail;
    return true;
  }
  return false;
}
// Fold an inner cascade: point D directly at S, drop the middle
// region blocks, and let the drain plus counted tail cover the full
// range from the zero restart. The middle tier's element range is
// exactly what the drain remainder plus tail re-cover: the countdown
// check proved middle bound N&~(K-1) with drain remainder N&(K-1).
// Only branch targets change; kept instructions are untouched.
static bool foldInnerCascade(Function &F, const InnerCascade &C) {
  // Collect the erase region: from D's vector arm forward to the
  // middle preheader, tier, and exit, stopping at S/L/T/returns.
  DenseSet<BasicBlock *> Stop{C.S, C.L, C.T};
  for (BasicBlock &BB : F)
    if (isa<ReturnInst>(BB.getTerminator()))
      Stop.insert(&BB);
  for (BasicBlock *ES : successors(C.L))
    if (ES != C.L)
      Stop.insert(ES);
  for (BasicBlock *ES : successors(C.T))
    if (ES != C.T)
      Stop.insert(ES);
  auto *DBr = cast<BranchInst>(C.D->getTerminator());
  BasicBlock *V = DBr->getSuccessor(0) == C.S ? DBr->getSuccessor(1)
                                              : DBr->getSuccessor(0);
  DenseSet<BasicBlock *> R;
  SmallVector<BasicBlock *, 16> Work{V};
  while (!Work.empty()) {
    BasicBlock *BB = Work.pop_back_val();
    if (Stop.count(BB) || R.count(BB))
      continue;
    if (isa<ReturnInst>(BB->getTerminator()) ||
        isa<UnreachableInst>(BB->getTerminator()))
      continue;
    R.insert(BB);
    for (BasicBlock *Su : successors(BB))
      Work.push_back(Su);
  }
  if (!R.count(C.Mid) || !R.count(C.MidPre))
    return false;
  // Single entry: every R pred is in R, D, or a recorded MidPre
  // bypass (re-routed to S below).
  DenseSet<BasicBlock *> Bypass(C.MidBypass.begin(), C.MidBypass.end());
  for (BasicBlock *BB : R)
    for (BasicBlock *P : predecessors(BB))
      if (!R.count(P) && P != C.D && !(BB == C.MidPre && Bypass.count(P)))
        return false;
  // Escape: no R def may feed kept code except S phis through
  // R edges (dropped below) and dead-in-R users.
  for (BasicBlock *BB : R)
    for (Instruction &I : *BB) {
      if (I.getType()->isVoidTy())
        continue;
      for (Use &U : I.uses()) {
        auto *UI = cast<Instruction>(U.getUser());
        BasicBlock *UB = UI->getParent();
        if (R.count(UB))
          continue;
        if (auto *P = dyn_cast<PHINode>(UI)) {
          if (P->getParent() == C.S)
            continue;
          bool AllR = true;
          for (unsigned K = 0; K < P->getNumIncomingValues(); ++K)
            if (P->getIncomingValue(K) == &I &&
                !R.count(P->getIncomingBlock(K)))
              AllR = false;
          if (AllR)
            continue;
        }
        return false;
      }
    }
  // Rewrite: D jumps to S; each MidPre bypass jumps to S too (its
  // zero restart is exactly the folded-path start). S drops R
  // incomings. No kept instruction changes.
  BranchInst *NewBr = BranchInst::Create(C.S, DBr);
  if (MDNode *MD = DBr->getMetadata("revenant.va"))
    NewBr->setMetadata("revenant.va", MD);
  NewBr->setDebugLoc(DBr->getDebugLoc());
  DBr->eraseFromParent();
  for (BasicBlock *BP : C.MidBypass) {
    auto *BT = dyn_cast<BranchInst>(BP->getTerminator());
    if (!BT)
      return false;
    bool ToMidPre = false;
    for (BasicBlock *Su : BT->successors())
      ToMidPre = ToMidPre || Su == C.MidPre;
    if (!ToMidPre)
      return false;
    // The bypass edge must carry no live values into S: S phis
    // either lack a bypass incoming or carry the same zero the
    // dispatch edge feeds (proven at match time).
    for (PHINode &Phi : C.S->phis()) {
      int Idx = Phi.getBasicBlockIndex(BP);
      if (Idx < 0)
        continue;
      auto *C0 = dyn_cast<ConstantInt>(Phi.getIncomingValue(Idx));
      if (!C0 || !C0->isZero())
        return false;
    }
    for (PHINode &Phi : C.S->phis()) {
      if (Phi.getBasicBlockIndex(BP) >= 0)
        continue;
      // No bypass incoming: the path entered the middle at 0
      // (MidPre 0-feed proven at match time), so the scalar tail
      // restarts at 0 on this path too.
      Phi.addIncoming(ConstantInt::getNullValue(Phi.getType()), BP);
    }
    BT->replaceSuccessorWith(C.MidPre, C.S);
  }
  for (PHINode &Phi : C.S->phis()) {
    SmallVector<BasicBlock *, 4> Kill;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
      if (R.count(Phi.getIncomingBlock(I)))
        Kill.push_back(Phi.getIncomingBlock(I));
    if (Kill.size() == Phi.getNumIncomingValues())
      return false;
    for (BasicBlock *KB : Kill)
      Phi.removeIncomingValue(KB);
  }
  for (BasicBlock *BB : R)
    BB->getTerminator()->setMetadata(LLVMContext::MD_loop, nullptr);
  SmallVector<BasicBlock *, 16> Dead(R.begin(), R.end());
  DenseSet<BasicBlock *> DeadSet(Dead.begin(), Dead.end());
  for (BasicBlock *BB : Dead) {
    Instruction *OldT = BB->getTerminator();
    auto *Unr = new UnreachableInst(BB->getContext());
    Unr->insertInto(BB, BB->end());
    OldT->eraseFromParent();
  }
  for (BasicBlock &BB : F) {
    if (DeadSet.count(&BB))
      continue;
    for (PHINode &Phi : BB.phis()) {
      SmallVector<BasicBlock *, 4> Kill;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
        if (DeadSet.count(Phi.getIncomingBlock(I)))
          Kill.push_back(Phi.getIncomingBlock(I));
      if (Kill.size() == Phi.getNumIncomingValues())
        continue;
      for (BasicBlock *KB : Kill)
        Phi.removeIncomingValue(KB);
    }
  }
  for (BasicBlock *BB : Dead)
    BB->dropAllReferences();
  for (BasicBlock *BB : Dead)
    BB->eraseFromParent();
  return true;
}
// Runs after UnloopVec in the same library: folds inner-cascade map
// dispatches UnloopVec leaves behind.
bool runUnvecMaps(llvm::Function &F) {
  bool Changed = false;
  // Collect-then-rewrite: matching never mutates.
  SmallVector<InnerCascade, 4> Cands;
  for (BasicBlock &BB : F) {
    auto *Br = dyn_cast<BranchInst>(BB.getTerminator());
    if (!Br || !Br->isConditional())
      continue;
    for (unsigned SIdx = 0; SIdx < 2; ++SIdx) {
      InnerCascade C;
      if (matchInnerCascade(&BB, Br->getSuccessor(SIdx), F, C))
        Cands.push_back(std::move(C));
    }
  }
  DenseSet<BasicBlock *> Taken;
  for (InnerCascade &C : Cands) {
    if (Taken.count(C.D) || Taken.count(C.S) || Taken.count(C.Mid) ||
        Taken.count(C.L) || Taken.count(C.T))
      continue;
    bool Overlap = false;
    for (BasicBlock *BB : C.R)
      Overlap = Overlap || Taken.count(BB);
    // An earlier fold may have erased blocks this candidate
    // references. Erased blocks have no parent; skip the stale
    // candidate instead of touching freed memory.
    for (BasicBlock *BB : {C.D, C.S, C.Mid, C.MidPre, C.L, C.T})
      if (!BB || BB->getParent() != &F)
        Overlap = true;
    if (Overlap)
      continue;
    Taken.insert(C.D);
    Taken.insert(C.S);
    Taken.insert(C.Mid);
    Taken.insert(C.MidPre);
    Taken.insert(C.L);
    Taken.insert(C.T);
    for (BasicBlock *BB : C.R)
      Taken.insert(BB);
    Changed |= foldInnerCascade(F, C);
  }
  return Changed;
}

} // namespace unvecmaps
} // namespace revenant
