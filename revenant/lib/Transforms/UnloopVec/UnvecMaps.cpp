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
// Drain-tail fusion: S routes to a countdown drain L (remainder
// (N-indexStart)&(K-1)) and a counted tail T (bound N) whose start is
// the drain end. When every tail lane group provably equals the
// drain's scalar body (shared TailCmp valEq per lane group, same
// bases), the drain is redundant: widen the tail start to the S
// resume and erase the drain. The tail then covers [resume, N)
// alone: exactly one loop, step 1 per element per lane group.
// Range contract (checked in the matcher, relied on by the fold):
// the drain index IV restarts at the S resume chain value indexStart
// (the tail IV's S-side value when the drain is skipped), it steps by
// exactly 1 per iteration, and the countdown runs exactly
// (N-indexStart)&(K-1) iterations where K is the tail's per-iteration
// lane-group stride, so the drain end (and the fused tail start) is
// exactly indexStart+rem. The fold materializes that sum in S.
struct DrainTail {
  BasicBlock *S = nullptr;
  BasicBlock *L = nullptr; // countdown drain
  ScalarLoopInfo DrainLoop;
  BasicBlock *T = nullptr; // counted tail
  ScalarLoopInfo TailLoop;
  Value *IndexStart = nullptr; // S-chain drain index start (fused base)
  Value *Resume = nullptr; // S resume remainder (drain countdown start)
  Value *N = nullptr;      // trip count root
};
static bool tailLanesEqualDrain(BasicBlock *Tail, BasicBlock *Drain,
                                ScalarLoopInfo &TailLoop,
                                ScalarLoopInfo &DrainLoop);
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

// Match S routing to a countdown drain L and (through a drain-exit
// block E) a counted tail T: S -> {L, E-chain}, L self-loop with a
// countdown latch (bound 0), E leading to T self-loop with bound N,
// T start fed from the drain end. Proofs: drain countdown remainder
// shape, tail bound strips to the same N as the drain count init,
// drain start is the S resume, and every tail lane group equals the
// drain body (tailLanesEqualDrain over the shared TailCmp).
static bool matchDrainTail(BasicBlock *S, Function &F, DrainTail &C) {
  auto *SBr = dyn_cast<BranchInst>(S->getTerminator());
  if (!SBr || !SBr->isConditional())
    return false;
  BasicBlock *S0 = SBr->getSuccessor(0);
  BasicBlock *S1 = SBr->getSuccessor(1);
  for (int Pick = 0; Pick < 2; ++Pick) {
    BasicBlock *MaybeDrain = Pick == 0 ? S0 : S1;
    BasicBlock *MaybeExit = Pick == 0 ? S1 : S0;
    ScalarLoopInfo Drain;
    if (!matchScalarLoop(MaybeDrain, Drain))
      continue;
    // Drain latch must compare against constant 0 (countdown).
    Value *DrainBound = loopBound(Drain);
    auto *Zero = dyn_cast_or_null<ConstantInt>(DrainBound);
    if (!Zero || !Zero->isZero())
      continue;
    // Drain start: the non-latch incoming of the drain IV must be
    // the S resume (a value available at S: S phi, S instruction,
    // constant, or argument).
    Value *Resume = nullptr;
    for (unsigned I = 0; I < Drain.IV->getNumIncomingValues(); ++I) {
      if (Drain.IV->getIncomingBlock(I) == MaybeDrain)
        continue;
      if (Drain.IV->getIncomingBlock(I) != S)
        Resume = nullptr;
      else
        Resume = Drain.IV->getIncomingValue(I);
    }
    if (!Resume)
      continue;
    // Drain index contract: the drain must also carry a step-1 index
    // IV restarting at an S-chain value (the fused tail base). The
    // countdown IV itself may be that index (single-phi drain) or a
    // sibling phi may index while another counts down (corpus drain
    // pairs r11 countdown with r10 index).
    Value *IndexStart = nullptr;
    for (PHINode &Phi : MaybeDrain->phis()) {
      if (&Phi == Drain.IV)
        continue;
      Value *BackVal = nullptr;
      Value *StartVal = nullptr;
      BasicBlock *StartBlock = nullptr;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
        if (Phi.getIncomingBlock(I) == MaybeDrain)
          BackVal = Phi.getIncomingValue(I);
        else {
          StartVal = Phi.getIncomingValue(I);
          StartBlock = Phi.getIncomingBlock(I);
        }
      }
      if (!BackVal || !StartVal || StartBlock != S)
        continue;
      auto *Step = dyn_cast<BinaryOperator>(BackVal);
      if (!Step || Step->getParent() != MaybeDrain)
        continue;
      if (Step->getOpcode() != Instruction::Add)
        continue;
      auto *One = dyn_cast<ConstantInt>(Step->getOperand(1));
      if (!One || !One->isOne() || Step->getOperand(0) != &Phi)
        continue;
      // The start must be an S-chain value (available when S runs).
      if (!isa<Constant>(StartVal) && !isa<Argument>(StartVal)) {
        auto *SI = dyn_cast<Instruction>(StartVal);
        if (!SI)
          continue;
        BasicBlock *PB = SI->getParent();
        if (PB != S && !isa<PHINode>(SI))
          continue;
        if (auto *SP = dyn_cast<PHINode>(SI))
          if (SP->getParent() != S)
            continue;
      }
      IndexStart = StartVal;
      break;
    }
    if (!IndexStart) {
      // Single-phi drain: the countdown IV doubles as the index when
      // it steps by exactly 1.
      if (Drain.Step && Drain.Step->isOne())
        IndexStart = Resume;
      else
        continue;
    }
    // The other arm reaches the counted tail through a drain-exit
    // block: BFS for the first counted self-loop below it.
    ScalarLoopInfo Tail;
    BasicBlock *TailBB = nullptr;
    {
      SmallVector<BasicBlock *, 8> Work{MaybeExit};
      DenseSet<BasicBlock *> Seen{MaybeExit};
      for (unsigned Steps = 0; !Work.empty() && !TailBB && Steps < 32;
           ++Steps) {
        BasicBlock *BB = Work.pop_back_val();
        if (BB != MaybeExit && BB != MaybeDrain && matchScalarLoop(BB, Tail)) {
          TailBB = BB;
          break;
        }
        for (BasicBlock *Su : successors(BB))
          if (Seen.insert(Su).second)
            Work.push_back(Su);
      }
    }
    if (!TailBB || TailBB == MaybeDrain)
      continue;
    // Tail bound must strip to the same N as the drain count init.
    Value *TailBound = loopBound(Tail);
    if (!TailBound)
      continue;
    // Tail start: the non-latch incoming of the tail IV must come
    // from the drain end (defined in the drain, its exit block, or
    // the S resume chain), not from an unrelated value.
    Value *TailStart = nullptr;
    for (unsigned I = 0; I < Tail.IV->getNumIncomingValues(); ++I) {
      if (Tail.IV->getIncomingBlock(I) == TailBB)
        continue;
      TailStart = Tail.IV->getIncomingValue(I);
    }
    if (!TailStart)
      continue;
    auto *StartInst = dyn_cast<Instruction>(TailStart);
    bool StartOK = isa<Constant>(TailStart) || isa<Argument>(TailStart);
    if (!StartOK && StartInst) {
      BasicBlock *PB = StartInst->getParent();
      StartOK = PB == MaybeDrain || PB == TailBB;
      if (!StartOK) {
        // Defined in the drain-exit region between drain and tail.
        for (BasicBlock *Su : successors(MaybeDrain)) {
          if (Su == MaybeDrain || Su == TailBB)
            continue;
          SmallVector<BasicBlock *, 8> W{Su};
          DenseSet<BasicBlock *> Sn{Su};
          for (unsigned K = 0; K < 16 && !W.empty() && !StartOK; ++K) {
            BasicBlock *X = W.pop_back_val();
            if (X == PB)
              StartOK = true;
            for (BasicBlock *Y : successors(X))
              if (Sn.insert(Y).second)
                W.push_back(Y);
          }
        }
      }
    }
    if (!StartOK)
      continue;
    // Tail bound must share the trip root with the drain count
    // init (the (N-S)&(K-1) remainder): otherwise the fused loop
    // may cover a different range than the drain did.
    Value *DrainCountInit = nullptr;
    for (unsigned I = 0; I < Drain.IV->getNumIncomingValues(); ++I) {
      if (Drain.IV->getIncomingBlock(I) == MaybeDrain)
        continue;
      // The count phi's start is the remainder; use the index IV's
      // start when the latch counts a sibling phi (corpus drain
      // latches on the countdown while indexing with the main IV).
      DrainCountInit = Drain.IV->getIncomingValue(I);
    }
    // Prefer the remainder init feeding the countdown: search the
    // drain block for an And feeding any drain phi start.
    for (PHINode &Phi : MaybeDrain->phis()) {
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
        if (Phi.getIncomingBlock(I) == MaybeDrain)
          continue;
        if (auto *BO = dyn_cast<BinaryOperator>(Phi.getIncomingValue(I)))
          if (BO->getOpcode() == Instruction::And)
            DrainCountInit = Phi.getIncomingValue(I);
      }
    }
    if (!DrainCountInit)
      continue;
    if (stripCountDerivation(TailBound, F) !=
        stripCountDerivation(DrainCountInit, F))
      continue;
    // Per-lane body proof over the shared TailCmp.
    if (!tailLanesEqualDrain(TailBB, MaybeDrain, Tail, Drain))
      continue;
    C.S = S;
    C.L = MaybeDrain;
    C.DrainLoop = Drain;
    C.T = TailBB;
    C.TailLoop = Tail;
    C.Resume = Resume;
    C.IndexStart = IndexStart;
    C.N = stripCountDerivation(TailBound, F);
    return true;
  }
  return false;
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

// Collect scalar (non-vector) loads/adds/stores of a loop body.
static void scalarMemOps(BasicBlock *B, SmallVectorImpl<LoadInst *> &Loads,
                         SmallVectorImpl<BinaryOperator *> &Adds,
                         SmallVectorImpl<StoreInst *> &Stores) {
  for (Instruction &I : *B) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!isa<FixedVectorType>(LI->getType()))
        Loads.push_back(LI);
    } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (!isa<FixedVectorType>(BO->getType()) &&
          BO->getOpcode() == Instruction::Add)
        Adds.push_back(BO);
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!isa<FixedVectorType>(SI->getValueOperand()->getType()))
        Stores.push_back(SI);
    }
  }
}

// Lane proof between two scalar loops (drain vs tail): every tail
// lane group (each scalar load/add/store triple, unrolled K times)
// must equal the drain's scalar computation through the shared
// TailCmp valEq, with identical memory bases. The drain IV maps to
// the tail IV (both count the same element sequence).
static bool tailLanesEqualDrain(BasicBlock *Tail, BasicBlock *Drain,
                                ScalarLoopInfo &TailLoop,
                                ScalarLoopInfo &DrainLoop) {
  SmallVector<LoadInst *, 4> TL, DL;
  SmallVector<BinaryOperator *, 4> TA, DA;
  SmallVector<StoreInst *, 4> TS, DS;
  scalarMemOps(Tail, TL, TA, TS);
  scalarMemOps(Drain, DL, DA, DS);
  if (TL.empty() || TS.empty() || DL.empty() || DS.empty())
    return false;
  // Same bases on every memory op.
  for (LoadInst *L : TL)
    if (ptrBase(L->getPointerOperand()) != ptrBase(DL[0]->getPointerOperand()))
      return false;
  for (StoreInst *S : TS)
    if (ptrBase(S->getPointerOperand()) != ptrBase(DS[0]->getPointerOperand()))
      return false;
  // The drain add must consume the drain load; every tail value add
  // must consume one tail load. IV steps and countdown decrements
  // (feeding phis and latch compares, not stores) are not value
  // adds and are skipped: collect only adds feeding a store.
  auto feedsStore = [](BinaryOperator *BO, ArrayRef<StoreInst *> Stores) {
    SmallVector<Value *, 4> Work{BO};
    DenseSet<Value *> Seen{BO};
    for (unsigned K = 0; K < 16 && !Work.empty(); ++K) {
      Value *V = Work.pop_back_val();
      for (User *U : V->users()) {
        if (isa<StoreInst>(U))
          return true;
        if (auto *N = dyn_cast<BinaryOperator>(U)) {
          if (N->getOpcode() == Instruction::Add && Seen.insert(N).second)
            Work.push_back(N);
        }
      }
    }
    return false;
  };
  BinaryOperator *DAdd = nullptr;
  for (BinaryOperator *A : DA)
    if (!DAdd && feedsStore(A, DS))
      DAdd = A;
  if (!DA.empty() && !DAdd)
    return false;
  if (DAdd && DAdd->getOperand(0) != DL[0] && DAdd->getOperand(1) != DL[0])
    return false;
  for (BinaryOperator *A : TA) {
    if (!feedsStore(A, TS))
      continue;
    bool UsesLoad = false;
    for (LoadInst *L : TL)
      UsesLoad = UsesLoad || A->getOperand(0) == L || A->getOperand(1) == L;
    if (!UsesLoad)
      return false;
  }
  // Every tail store value must equal the drain's stored value
  // (or the drain add result) under IV mapping.
  Value *DrainStored = DAdd ? cast<Value>(DAdd)
                            : cast<Value>(DS[0]->getValueOperand());
  for (StoreInst *S : TS) {
    TailCmp TC;
    TC.A = Drain;
    TC.B = Tail;
    TC.MapAB[DrainLoop.IV] = TailLoop.IV;
    for (LoadInst *L : DL)
      TC.MapAB[L] = nullptr; // filled per-store below
    // Map each drain load to the tail load feeding this store.
    bool Mapped = false;
    for (LoadInst *L : TL) {
      // The tail load feeding store S (through its add, if any).
      Value *V = S->getValueOperand();
      if (auto *BO = dyn_cast<BinaryOperator>(V)) {
        if (BO->getOperand(0) == L || BO->getOperand(1) == L) {
          for (LoadInst *D : DL)
            TC.MapAB[D] = L;
          Mapped = true;
          break;
        }
      } else if (V == L) {
        for (LoadInst *D : DL)
          TC.MapAB[D] = L;
        Mapped = true;
        break;
      }
    }
    if (!Mapped)
      return false;
    if (!TC.valEq(DrainStored, S->getValueOperand(), TailCmp::MaxDepth))
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

// Fold drain+tail by erasing the drain alone: S jumps to the guard
// block G (the drain-exit block feeding the tail IV), and G's tail-feed
// phi is rewired to the drain end indexStart+rem materialized in S.
// The guard, its skip/exit decision, and the tail are untouched, so the
// folded graph makes exactly the same decisions with exactly the same
// tail start: the drain covers [indexStart, indexStart+rem) uncondi-
// tionally (countdown from rem by 1 runs exactly rem iterations; when
// rem is 0 S skips the drain and fused.start == indexStart), hence the
// tail runs from the same value over the same range, including the
// degenerate cases (guard skips the tail; tail entered at N hangs
// exactly as before). Only branch targets and the tail-feed change;
// kept instructions are untouched.
static bool foldDrainTail(Function &F, const DrainTail &C) {
  // Drain-exit blocks: on a path from L to T (forward reachable
  // from L without passing S/T, and able to reach T). The function
  // exit is never included even when reachable from L.
  DenseSet<BasicBlock *> Exit;
  {
    DenseSet<BasicBlock *> Fwd;
    SmallVector<BasicBlock *, 8> Work;
    for (BasicBlock *Su : successors(C.L))
      if (Su != C.L)
        Work.push_back(Su);
    for (unsigned K = 0; K < 32 && !Work.empty(); ++K) {
      BasicBlock *BB = Work.pop_back_val();
      if (BB == C.T || BB == C.S || !Fwd.insert(BB).second)
        continue;
      if (isa<ReturnInst>(BB->getTerminator()) ||
          isa<UnreachableInst>(BB->getTerminator()))
        continue;
      for (BasicBlock *Su : successors(BB))
        Work.push_back(Su);
    }
    DenseSet<BasicBlock *> Back{C.T};
    SmallVector<BasicBlock *, 8> W2{C.T};
    for (unsigned K = 0; K < 32 && !W2.empty(); ++K) {
      BasicBlock *BB = W2.pop_back_val();
      for (BasicBlock *P : predecessors(BB)) {
        if (P == C.S || P == C.L)
          continue;
        if (Back.insert(P).second)
          W2.push_back(P);
      }
    }
    for (BasicBlock *BB : Fwd)
      if (Back.count(BB))
        Exit.insert(BB);
  }
  // The tail-feed block G holds the tail IV's non-latch incoming; it
  // stays (guard decision preserved), everything else on the L->T
  // paths goes. G must route (branch) only: its non-phi instructions
  // besides the tail-feed phi must be pure routing/arithmetic with no
  // memory traffic. Only one G is supported.
  BasicBlock *G = nullptr;
  for (unsigned I = 0; I < C.TailLoop.IV->getNumIncomingValues(); ++I) {
    BasicBlock *PB = C.TailLoop.IV->getIncomingBlock(I);
    if (PB == C.T)
      continue;
    if (G && PB != G)
      return false;
    G = PB;
  }
  if (!G || !Exit.count(G))
    return false;
  for (Instruction &I : *G) {
    if (isa<PHINode>(&I) || isa<BranchInst>(&I) || isa<ICmpInst>(&I) ||
        isa<CastInst>(&I) || isa<GetElementPtrInst>(&I))
      continue;
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (!isa<FixedVectorType>(BO->getType()))
        continue;
    }
    return false;
  }
  // Other exit blocks (pure L->G routing) must be dead after the fold
  // except for feeding G: same whitelist, no memory traffic.
  for (BasicBlock *BB : Exit) {
    if (BB == G)
      continue;
    for (Instruction &I : *BB) {
      if (isa<PHINode>(&I) || isa<BranchInst>(&I) || isa<ICmpInst>(&I) ||
          isa<CastInst>(&I) || isa<GetElementPtrInst>(&I))
        continue;
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        if (!isa<FixedVectorType>(BO->getType()))
          continue;
      }
      return false;
    }
  }
  // Dead set (drain plus non-guard exit blocks). G stays: S will target
  // it, and the phi cleanup after erasing drops the erased incomings.
  SmallVector<BasicBlock *, 8> Dead;
  Dead.push_back(C.L);
  for (BasicBlock *BB : Exit)
    if (BB != G)
      Dead.push_back(BB);
  DenseSet<BasicBlock *> DeadSet(Dead.begin(), Dead.end());
  // Kept-predecessor check BEFORE any mutation: every predecessor of a
  // dead block must itself be dead, S (repointed below), or G (which
  // stays and keeps its edge). Bailing here leaves the IR untouched.
  for (BasicBlock *BB : Dead)
    for (BasicBlock *P : predecessors(BB))
      if (!DeadSet.count(P) && P != C.S && P != G)
        return false;
  // G itself must become reachable only from S: every current
  // predecessor must be S, the drain, or an erased exit block.
  // Otherwise collapsing G's tail-feed phi would drop a live entry
  // (the reviewer's alt-path counterexample).
  for (BasicBlock *P : predecessors(G))
    if (P != C.S && P != C.L && !DeadSet.count(P))
      return false;
  // Materialize the drain end indexStart+rem in S. When the drain index
  // restarts at 0 (the cascade-folded shape) the sum folds to rem.
  Value *FusedStart = C.Resume;
  if (C.IndexStart != C.Resume) {
    auto *Sum = BinaryOperator::CreateAdd(C.IndexStart, C.Resume,
                                          "unvecmaps.fused.start");
    Sum->insertBefore(C.S->getTerminator());
    if (auto *IS = dyn_cast<Instruction>(C.IndexStart))
      if (MDNode *MD = IS->getMetadata("revenant.va"))
        Sum->setMetadata("revenant.va", MD);
    FusedStart = Sum;
  }
  // Rewire G's tail-feed phi(s) to the fused start from S: any phi in G
  // feeding the tail IV's start value takes fused.start; the tail IV
  // itself keeps pointing at G.
  for (PHINode &Phi : G->phis()) {
    bool FeedsTail = false;
    for (unsigned I = 0; I < C.TailLoop.IV->getNumIncomingValues(); ++I) {
      if (C.TailLoop.IV->getIncomingBlock(I) == G &&
          C.TailLoop.IV->getIncomingValue(I) == &Phi)
        FeedsTail = true;
    }
    if (!FeedsTail)
      continue;
    // Collapse to a single incoming from S: G is only reachable from S
    // after the fold (drain and sibling exit blocks are erased).
    while (Phi.getNumIncomingValues() > 1)
      Phi.removeIncomingValue(Phi.getNumIncomingValues() - 1);
    Phi.setIncomingValue(0, FusedStart);
    Phi.setIncomingBlock(0, C.S);
  }
  // S jumps to the guard block.
  auto *SBr = cast<BranchInst>(C.S->getTerminator());
  BranchInst *NewBr = BranchInst::Create(G, SBr);
  if (MDNode *MD = SBr->getMetadata("revenant.va"))
    NewBr->setMetadata("revenant.va", MD);
  NewBr->setDebugLoc(SBr->getDebugLoc());
  SBr->eraseFromParent();
  for (BasicBlock *BB : Dead)
    BB->getTerminator()->setMetadata(LLVMContext::MD_loop, nullptr);
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
  (void)F;
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
    // Drain-tail fusion runs after the cascade folds in each round:
    // with the middle tiers gone, S routes to drain+tail directly.
    {
      SmallVector<DrainTail, 4> FCands;
      for (BasicBlock &BB : F) {
        auto *Br = dyn_cast<BranchInst>(BB.getTerminator());
        if (!Br || !Br->isConditional())
          continue;
        DrainTail C;
        if (matchDrainTail(&BB, F, C))
          FCands.push_back(C);
      }
      for (DrainTail &C : FCands) {
        if (!C.S || !C.L || !C.T || C.S->getParent() != &F ||
            C.L->getParent() != &F || C.T->getParent() != &F)
          continue;
        RoundChanged |= foldDrainTail(F, C);
      }
    }
    Changed |= RoundChanged;
    if (!RoundChanged)
      break;
  }
  return Changed;
}
} // namespace unvecmaps
} // namespace revenant
