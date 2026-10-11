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
using unloopvec::TailCmp;

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

// Partition-coverage acceptance for outer tiers: the middle bound
// strips to the same trip-count root N as the tail bound, the tail
// spans to N, and the drain is a countdown remainder from the S
// resume. Then drain [resume, drain_end] plus tail [drain_end, N]
// cover [resume, N], a superset of any same-N middle range, so the
// middle tier is redundant. All roots compare by pointer after
// stripping derivations.
static bool partitionCoverageOK(Value *MidBound, ScalarLoopInfo &Drain,
                                ScalarLoopInfo &Tail, const Function &F) {
  Value *TailBound = loopBound(Tail);
  if (!TailBound || !MidBound)
    return false;
  if (stripCountDerivation(MidBound, F) !=
      stripCountDerivation(TailBound, F))
    return false;
  // Drain must be a countdown (latch against 0).
  Value *DrainBound = loopBound(Drain);
  auto *Zero = dyn_cast_or_null<ConstantInt>(DrainBound);
  if (!Zero || !Zero->isZero())
    return false;
  return true;
}

// Element-wise map body check for a middle-tier vector loop: one
// vector load, an optional splat add (fill/add) or none (copy), and
// one vector store of the same type. All memory ops simple
// (non-volatile, non-atomic, align 1).
static bool isMapBody(BasicBlock *B) {
  // Interleaved/unrolled tiers repeat the load/add/store group
  // several times per iteration (corpus AVX2 add: 4x <8 x i32>
  // loads, adds, stores). Collect every vector memory op and every
  // lane-wise add; all must share one element type.
  Type *Elem = nullptr;
  unsigned Loads = 0, Stores = 0;
  for (Instruction &I : *B) {
    if (isa<PHINode>(&I) || isa<BranchInst>(&I) ||
        isa<GetElementPtrInst>(&I) || isa<ICmpInst>(&I) ||
        isa<CastInst>(&I))
      continue;
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      auto *VT = dyn_cast<FixedVectorType>(LI->getType());
      if (!VT || LI->isVolatile() || !LI->isSimple())
        return false;
      if (!Elem)
        Elem = VT->getElementType();
      else if (Elem != VT->getElementType())
        return false;
      ++Loads;
      continue;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (isa<FixedVectorType>(BO->getType())) {
        // Lane-wise vector arithmetic: only add is a map.
        if (BO->getOpcode() != Instruction::Add)
          return false;
        auto *VT = cast<FixedVectorType>(BO->getType());
        if (!Elem)
          Elem = VT->getElementType();
        else if (Elem != VT->getElementType())
          return false;
      }
      // Scalar ALU (address math, IV steps, bounds) is fine.
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      auto *VT =
          dyn_cast<FixedVectorType>(SI->getValueOperand()->getType());
      if (!VT || SI->isVolatile() || !SI->isSimple())
        return false;
      if (!Elem)
        Elem = VT->getElementType();
      else if (Elem != VT->getElementType())
        return false;
      ++Stores;
      continue;
    }
    if (isa<InsertElementInst>(&I) || isa<ShuffleVectorInst>(&I))
      continue;
    return false;
  }
  return Elem && Loads >= 1 && Stores >= 1;
}
// Root pointer behind GEP/index chains: the base allocation both
// sides must share for lane correspondence.
static Value *ptrBase(Value *P) {
  while (auto *G = dyn_cast<GetElementPtrInst>(P))
    P = G->getPointerOperand();
  return P;
}

// Lane-equivalence gate for one middle vector tier against one
// scalar loop (the countdown drain): every vector lane provably
// equals the scalar the drain computes, through the shared TailCmp
// proof (loads pre-seeded, splats projected, lane-wise ALU
// recursed, commutative retry). Same memory bases on both sides.
// Returns false (no fold) on anything unproven.
static bool midLanesEqualMidVsDrain(BasicBlock *Mid, BasicBlock *Drain) {
  // Every vector load/add/store in the tier; every lane group must
  // prove equal against the drain's scalar computation.
  SmallVector<LoadInst *, 4> VLoads;
  SmallVector<BinaryOperator *, 4> VAdds;
  SmallVector<StoreInst *, 4> VStores;
  for (Instruction &I : *Mid) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (isa<FixedVectorType>(LI->getType()))
        VLoads.push_back(LI);
    } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (isa<FixedVectorType>(BO->getType()) &&
          BO->getOpcode() == Instruction::Add)
        VAdds.push_back(BO);
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (isa<FixedVectorType>(SI->getValueOperand()->getType()))
        VStores.push_back(SI);
    }
  }
  LoadInst *SL = nullptr;
  BinaryOperator *SAdd = nullptr;
  StoreInst *SS = nullptr;
  for (Instruction &I : *Drain) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!isa<FixedVectorType>(LI->getType()) && !SL)
        SL = LI;
    } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (!isa<FixedVectorType>(BO->getType()) &&
          BO->getOpcode() == Instruction::Add && !SAdd)
        SAdd = BO;
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!isa<FixedVectorType>(SI->getValueOperand()->getType()) &&
          !SS)
        SS = SI;
    }
  }
  if (VLoads.empty() || VStores.empty() || !SL || !SS)
    return false;
  // Same memory bases on every memory op.
  for (LoadInst *VL : VLoads)
    if (ptrBase(VL->getPointerOperand()) != ptrBase(SL->getPointerOperand()))
      return false;
  for (StoreInst *VS : VStores)
    if (ptrBase(VS->getPointerOperand()) != ptrBase(SS->getPointerOperand()))
      return false;
  // Copy (no adds either side): every stored lane group equals the
  // loaded lanes.
  if (VAdds.empty() && !SAdd) {
    for (StoreInst *VS : VStores) {
      TailCmp TC;
      TC.A = Mid;
      TC.B = Drain;
      for (LoadInst *VL : VLoads)
        TC.MapAB[VL] = SL;
      if (!TC.laneEq(VS->getValueOperand(), SS->getValueOperand(),
                     TailCmp::MaxDepth))
        return false;
    }
    return true;
  }
  // Add: every lane-wise vector add must equal the scalar add. Each
  // vector add must consume one of the tier's loads.
  if (VAdds.empty() || !SAdd)
    return false;
  if (SAdd->getOperand(0) != SL && SAdd->getOperand(1) != SL)
    return false;
  for (BinaryOperator *VAdd : VAdds) {
    LoadInst *Used = nullptr;
    for (LoadInst *VL : VLoads)
      if (VAdd->getOperand(0) == VL || VAdd->getOperand(1) == VL)
        Used = VL;
    if (!Used)
      return false;
    TailCmp TC;
    TC.A = Mid;
    TC.B = Drain;
    for (LoadInst *VL : VLoads)
      TC.MapAB[VL] = SL;
    // Operand order may differ (splat-first vs load-first): retry
    // swapped for commutative add. laneEq itself stays positional.
    if (TC.laneEq(VAdd, SAdd, TailCmp::MaxDepth))
      continue;
    if (VAdd->getNumOperands() != 2 || SAdd->getNumOperands() != 2)
      return false;
    TailCmp TCSwap;
    TCSwap.A = Mid;
    TCSwap.B = Drain;
    for (LoadInst *VL : VLoads)
      TCSwap.MapAB[VL] = SL;
    if (!TCSwap.laneEq(VAdd->getOperand(0), SAdd->getOperand(1),
                       TailCmp::MaxDepth) ||
        !TCSwap.laneEq(VAdd->getOperand(1), SAdd->getOperand(0),
                       TailCmp::MaxDepth))
      return false;
  }
  return true;
}

// Match the inner cascade around dispatch D with scalar entry S:
// S routes to a countdown drain L and a counted tail T; the other
// arm reaches a middle vector tier Mid (single-block self-loop with
// a preheader) whose bound the countdown check accepts against the
// drain. Region R covers the middle preheader, tier, and exit plus
// any dispatch blocks between D and the preheader. Returns true with
// C filled on a full structural match; no mutation.

// The counted tail must sit behind the corpus remainder guard. The
// tail-feed block G (the tail IV's non-latch predecessor) computes
//   %left = sub <start>, <N>
//   %c = icmp ugt %left, -T        (T >= the tail stride K)
//   br i1 %c, label <not-tail>, label <tail>
// where <start> is the tail-feed phi's outside-G incoming (the S
// chain value the drain restarts from). On the middle-tier path the
// drain ends exactly at N (middle bound N&~(K-1) plus the remainder
// N&(K-1)), so %left is 0 and the guard always skips the tail: the
// middle tier plus drain already covered [0, N). Without the guard
// the tail enters at N and writes [N, N+K) before its latch exits,
// and the fold (drain [0, rem) plus tail [rem, N)) would drop that
// range. On the zero-restart path the guard skips iff N < T, and
// the fold keeps the same instructions over the same inputs, so the
// decision is unchanged there.
static bool tailGuardOK(BasicBlock *Tail, BasicBlock *Drain, BasicBlock *S,
                        ScalarLoopInfo &L, Function &F) {
  Value *TailBound = loopBound(L);
  if (!TailBound)
    return false;
  Value *NRoot = stripCountDerivation(TailBound, F);
  uint64_t K = L.Step ? L.Step->getZExtValue() : 0;
  // Single tail-feed block G holding the tail IV's start.
  BasicBlock *G = nullptr;
  Value *Feed = nullptr;
  for (unsigned I = 0; I < L.IV->getNumIncomingValues(); ++I) {
    if (L.IV->getIncomingBlock(I) == Tail)
      continue;
    if (G && L.IV->getIncomingBlock(I) != G)
      return false;
    G = L.IV->getIncomingBlock(I);
    Feed = L.IV->getIncomingValue(I);
  }
  if (!G || G == Tail)
    return false;
  // The start value: the feed phi's S-chain incoming (the value the
  // drain restarts from on the zero path); the drain end is the
  // other incoming.
  Value *Start = Feed;
  if (auto *P = dyn_cast<PHINode>(Feed)) {
    if (P->getParent() != G)
      return false;
    Value *FromS = nullptr;
    for (unsigned I = 0; I < P->getNumIncomingValues(); ++I) {
      if (P->getIncomingBlock(I) != S)
        continue;
      if (FromS)
        return false; // ambiguous start
      FromS = P->getIncomingValue(I);
    }
    if (!FromS)
      return false;
    Start = FromS;
  }
  auto *Br = dyn_cast<BranchInst>(G->getTerminator());
  if (!Br || !Br->isConditional())
    return false;
  bool TailArm = false;
  for (unsigned I = 0; I < 2; ++I)
    if (Br->getSuccessor(I) == Tail)
      TailArm = true;
  if (!TailArm)
    return false;
  for (Instruction &I : *G) {
    auto *Sub = dyn_cast<BinaryOperator>(&I);
    if (!Sub || Sub->getOpcode() != Instruction::Sub)
      continue;
    if (Sub->getOperand(0) != Start && Sub->getOperand(0) != Feed)
      continue;
    if (stripCountDerivation(Sub->getOperand(1), F) != NRoot)
      continue;
    for (User *U : Sub->users()) {
      auto *Cmp = dyn_cast<ICmpInst>(U);
      if (!Cmp || Cmp->getParent() != G)
        continue;
      if (Cmp->getPredicate() != CmpInst::ICMP_UGT)
        continue;
      if (Cmp->getOperand(0) != Sub || Br->getCondition() != Cmp)
        continue;
      auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(1));
      if (!C || !C->isNegative())
        continue;
      // -T with T >= K: the skip region covers every possible
      // remainder, so the middle-tier path always skips.
      if (C->getSExtValue() > -(int64_t)K)
        continue;
      return true;
    }
  }
  return false;
}

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
    // BFS from V for the middle vector tier: the first vector
    // self-loop with a map body. Its preheader is its single
    // non-self predecessor (or the block the search stepped from).
    // The search threads through dispatch blocks (conditional or
    // not) without entering S/drain/tail.
    BasicBlock *MidPre = nullptr;
    BasicBlock *Mid = nullptr;
    ScalarLoopInfo MidLoop;
    {
      SmallVector<BasicBlock *, 16> Work{V};
      DenseSet<BasicBlock *> Seen{V};
      DenseMap<BasicBlock *, BasicBlock *> Pred;
      for (unsigned Steps = 0; !Work.empty() && !Mid && Steps < 64;
           ++Steps) {
        BasicBlock *BB = Work.pop_back_val();
        if (BB == S || BB == MaybeDrain || BB == TailBB)
          continue;
        ScalarLoopInfo Loop;
        if (BB != V && matchScalarLoop(BB, Loop) && isMapBody(BB)) {
          Mid = BB;
          MidLoop = Loop;
          MidPre = Pred.lookup(BB);
          break;
        }
        auto *T = dyn_cast<BranchInst>(BB->getTerminator());
        if (!T)
          continue;
        // Through single-successor blocks and dispatch arms alike;
        // S/drain/tail/exit arms are pruned, not followed.
        for (BasicBlock *Su : T->successors()) {
          if (Su == S || Su == MaybeDrain || Su == TailBB)
            continue;
          if (isa<ReturnInst>(Su->getTerminator()) ||
              isa<UnreachableInst>(Su->getTerminator()))
            continue;
          if (!Seen.insert(Su).second)
            continue;
          if (!Pred.count(Su))
            Pred[Su] = BB;
          Work.push_back(Su);
        }
      }
    }
    if (!Mid || !MidPre)
      continue;
    // The preheader must feed the tier directly.
    {
      bool Feeds = false;
      for (BasicBlock *Su : successors(MidPre))
        Feeds = Feeds || Su == Mid;
      if (!Feeds)
        continue;
    }
    uint64_t Step = MidLoop.Step->getZExtValue();
    if (Step < 2)
      continue;
    // Trip coverage, two forms: the countdown complement (middle
    // bound N&~(K-1) with drain remainder N&(K-1)) or the outer-tier
    // partition form (middle and tail bounds share the N root while
    // drain+tail span to N, so the middle is redundant).
    {
      Value *MidBound = loopBound(MidLoop);
      bool TripOK =
          countdownSecondaryOK(Mid, MidLoop, Drain, F, Step) ||
          partitionCoverageOK(MidBound, Drain, Tail, F);
      if (!TripOK)
        continue;
    }
    // The counted tail must sit behind the corpus remainder guard
    // (see tailGuardOK). Without it the middle-tier path enters the
    // tail at N and writes [N, N+K) before the latch exits, a range
    // the folded graph never writes.
    if (!tailGuardOK(TailBB, MaybeDrain, S, Tail, F))
      continue;
    // Lane equivalence (board bug 25, shared TailCmp gate): every
    // middle lane must equal the drain's scalar. The +100-vs-+1
    // mismatch fails here and bails.
    if (!midLanesEqualMidVsDrain(Mid, MaybeDrain))
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
  // Fixpoint rounds (bounded): each round re-scans the mutated
  // graph. Candidates run inner-first (reverse block order):
  // inner cascades sit later in layout, and folding them first
  // keeps the outer candidates' blocks valid. Stale candidates
  // (blocks erased by an earlier fold) are skipped by parent
  // check. Collect-then-rewrite within each round: matching never
  // mutates.
  for (unsigned Round = 0; Round < 4; ++Round) {
    SmallVector<InnerCascade, 4> Cands;
    SmallVector<BasicBlock *, 16> Order;
    for (BasicBlock &BB : F) {
      auto *Br = dyn_cast<BranchInst>(BB.getTerminator());
      if (!Br || !Br->isConditional())
        continue;
      Order.push_back(&BB);
    }
    for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
      BasicBlock *D = *It;
      if (D->getParent() != &F)
        continue;
      auto *Br = dyn_cast<BranchInst>(D->getTerminator());
      if (!Br || !Br->isConditional())
        continue;
      for (unsigned SIdx = 0; SIdx < 2; ++SIdx) {
        InnerCascade C;
        if (matchInnerCascade(D, Br->getSuccessor(SIdx), F, C))
          Cands.push_back(std::move(C));
      }
    }
    DenseSet<BasicBlock *> Taken;
    bool RoundChanged = false;
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
      // The lane-equivalence proof runs inside the matcher
      // (midLanesEqualMidVsDrain over the shared TailCmp); only
      // proved candidates reach this fold.
      RoundChanged |= foldInnerCascade(F, C);
    }
    Changed |= RoundChanged;
    if (!RoundChanged)
      break;
  }
  return Changed;
}
} // namespace unvecmaps
} // namespace revenant
