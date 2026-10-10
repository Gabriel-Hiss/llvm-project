//===- UnloopVec.cpp - undo loop-vectorizer skeletons ------------------===//
//
// Part of the Revenant LLVM fork.
//
// See UnloopVec.h for the firing shape and the soundness argument.
//
// Pass structure: scan for dispatch candidates D (cond br where one
// successor S is the scalar tail -- either an entry block with a
// single-block scalar self-loop latch under it, or the loop header
// itself in the dispatch-to-header form the normalized lifter emits
// -- and the other side reaches a wide-loop region R that feeds S's
// resume phi from its own block, or where both D successors reach
// such a region). Validate R and the scalar tail structurally, then:
//   1. fold D to the scalar edge (keep S's incoming from D),
//   2. drop R-block incomings from non-S outsider phis,
//   3. erase dead R blocks (wide body, middle reduce/combine, epilogue
//      vector loop, memcheck chain, dispatch chain blocks).
// Step 3 leaves blocks referenced by llvm.loop metadata; metadata is
// not followed by the verifier, so erase order needs no metadata
// cleanup. Alive2-style reasoning for the non-obvious steps:
//   - D folding: D's condition is the vectorizer's own profitability /
//     aliasing guard. S is the vectorizer's complete fallback for the
//     range: it re-derives the remainder range from the trip count N
//     (small-N edge skips R entirely; the N==n.vec edge skips the tail;
//     the tiered tail below S dispatches on N congruences and remaining
//     counts, all satisfiable at resume 0, and every counted tier loop
//     covers exactly its checked range). So S-from-D-start computes the
//     same value as S-from-R-resume on every path that reaches it. In
//     the versioned/no-resume form (GCC) S is a full-trip copy of the
//     loop and no resume values flow in at all.
//   - middle.reduce erasure: combine + llvm.vector.reduce feeds only the
//     tail accumulator seed and the direct-exit value; both are dead
//     once the tail iterates from 0.
//   - memcheck erasure: pure ptrtoint/sub ICMP chain whose only true use
//     is routing into R; dead once D folds to S.
//   - dispatch-to-header: when S is the loop header, its phis carry
//     loop state directly; the IV phi must be a resume phi (start
//     edges feed 0, R incomings bound-derived), accumulator phis feed
//     nothing but the latch/exit and restart at their start value.
// Refinement-free because no kept instruction changes: kept blocks keep
// their instructions, operands, and flags; only branch targets and dead
// code change.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/UnloopVec/UnloopVec.h"
#include "UnloopVecCommon.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include <algorithm>
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace revenant {
using unloopvec::loopBound;
using unloopvec::matchScalarLoop;
using unloopvec::ScalarLoopInfo;
using unloopvec::stripCountDerivation;
namespace unvecmaps {
bool runUnvecMaps(llvm::Function &F);
} // namespace unvecmaps

namespace {

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
}

bool isVectorTyped(Type *T) { return isa<FixedVectorType>(T); }

// An instruction that may appear in an erased vector tier: no calls
// except LLVM intrinsics, no asm, nothing volatile/atomic, no
// terminators, no !revenant.opaque.
bool isTierErasable(const Instruction &I) {
  if (I.isTerminator())
    return false;
  if (I.isVolatile())
    return false;
  if (const auto *CB = dyn_cast<CallBase>(&I)) {
    const Function *Callee = CB->getCalledFunction();
    if (!Callee)
      return false;
    if (Callee->isIntrinsic())
      return true;
    return false;
  }
  if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I) ||
      isa<FenceInst>(&I))
    return false;
  if (I.getMetadata("revenant.opaque"))
    return false;
  return true;
}

// A forward reachable, non-cross-lane FP math op: only collapsible when
// the IR itself licenses reassociation.
bool fpNeedsReassoc(const Instruction &I) {
  if (!I.getType()->isFPOrFPVectorTy())
    return false;
  switch (I.getOpcode()) {
  case Instruction::FAdd:
  case Instruction::FMul:
  case Instruction::FSub:
  case Instruction::FDiv:
  case Instruction::FRem:
    return I.getOpcode() == Instruction::FAdd ||
           I.getOpcode() == Instruction::FMul;
  default:
    return false;
  }
}

bool isOrderedLaneReduction(const Instruction &I, const BasicBlock &B) {
  const auto *Target = dyn_cast<BinaryOperator>(&I);
  if (!Target || Target->getOpcode() != Instruction::FAdd ||
      !Target->getType()->isDoubleTy())
    return false;
  SmallVector<const LoadInst *, 8> Loads;
  SmallVector<const BinaryOperator *, 8> Adds;
  for (const Instruction &J : B) {
    if (auto *L = dyn_cast<LoadInst>(&J))
      if (L->getType()->isDoubleTy())
        Loads.push_back(L);
    if (auto *BO = dyn_cast<BinaryOperator>(&J))
      if (BO->getOpcode() == Instruction::FAdd)
        Adds.push_back(BO);
  }
  if (Loads.size() != Adds.size() ||
      (Loads.size() != 2 && Loads.size() != 4 && Loads.size() != 8))
    return false;
  const DataLayout &DL = B.getModule()->getDataLayout();
  SmallVector<int64_t, 8> Offsets;
  const Value *CommonBase = nullptr;
  for (const LoadInst *L : Loads) {
    const Value *Ptr = L->getPointerOperand();
    APInt Total(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
    while (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
      APInt Part(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
      if (!GEP->accumulateConstantOffset(DL, Part)) {
        const Value *Root = GEP->getPointerOperand();
        if (!CommonBase)
          CommonBase = Root;
        else if (CommonBase != Root)
          return false;
        break;
      }
      if (Part.getBitWidth() != Total.getBitWidth())
        return false;
      Total += Part;
      Ptr = GEP->getPointerOperand();
    }
    if (Total.getActiveBits() > 63)
      return false;
    Offsets.push_back(Total.getSExtValue());
  }
  llvm::sort(Offsets);
  int64_t Base = Offsets.front();
  for (unsigned N = 0; N != Offsets.size(); ++N)
    if (Offsets[N] != Base + static_cast<int64_t>(N * 8))
      return false;
  const Value *Accumulator = nullptr;
  for (unsigned N = 0; N != Adds.size(); ++N) {
    const BinaryOperator *Add = Adds[N];
    const LoadInst *Load = nullptr;
    const Value *Carry = nullptr;
    for (unsigned Op = 0; Op != 2; ++Op) {
      if (auto *L = dyn_cast<LoadInst>(Add->getOperand(Op)))
        Load = L;
      else
        Carry = Add->getOperand(Op);
    }
    if (!Load || !Carry)
      return false;
    if (N && Carry != Accumulator)
      return false;
    auto It = llvm::find(Loads, Load);
    if (It == Loads.end() || It != Loads.begin() + N)
      return false;
    Accumulator = Add;
  }
  return true;
}

bool fpReassocOK(const Instruction &I) {
  if (!fpNeedsReassoc(I))
    return true;
  if (const auto *FPMO = dyn_cast<FPMathOperator>(&I))
    return FPMO->hasAllowReassoc() || FPMO->isFast();
  return false;
}

// Structural phi congruence for tier routing: allow trivial
// coverage-only differences between the vector side and the scalar
// side by comparing the non-phi instruction sequence by opcode/type,
// so unrolled scalar tails (2x/4x copies) and constant-folded bounds
// still count as rerunnable from 0.
// Def-use following: operands may be phis of the compared blocks, the
// compared blocks' own non-phi defs at the same position, constants,
// function arguments, or values defined outside the compared region.
// GEP/load/store opcodes must match positionally (memory pattern must
// be the same); pure ALU operands recurse structurally. Depth cap keeps
// this linear.
struct TailCmp {
  const BasicBlock *A = nullptr; // vector-side tier block
  const BasicBlock *B = nullptr; // scalar tail block
  DenseMap<const Value *, const Value *> MapAB;
  DenseSet<std::pair<const Value *, const Value *>> InProgress;
  static constexpr unsigned MaxDepth = 16;

  const Instruction *nthNonPhi(const BasicBlock *BB, unsigned N) const {
    unsigned I = 0;
    for (const Instruction &J : *BB) {
      if (isa<PHINode>(&J))
        continue;
      if (I == N)
        return &J;
      ++I;
    }
    return nullptr;
  }

  bool valEq(const Value *X, const Value *Y, unsigned Depth) {
    if (X == Y)
      return true;
    if (Depth == 0)
      return false;
    if (isa<Constant>(X) && isa<Constant>(Y))
      return X == Y;
    if (isa<Argument>(X) && X == Y)
      return true;
    auto It = MapAB.find(X);
    if (It != MapAB.end())
      return It->second == Y;
    // Defs outside both blocks must be identical values.
    const auto *IX = dyn_cast<Instruction>(X);
    const auto *IY = dyn_cast<Instruction>(Y);
    if (!IX || !IY)
      return false;
    if (IX->getParent() != A || IY->getParent() != B)
      return false;
    if (IX->getOpcode() != IY->getOpcode())
      return false;
    if (IX->getType() != IY->getType())
      return false;
    if (!InProgress.insert({X, Y}).second)
      return true; // coinductive: already comparing, assume equal
    bool OK = IX->getNumOperands() == IY->getNumOperands();
    for (unsigned I = 0; OK && I < IX->getNumOperands(); ++I)
      OK = valEq(IX->getOperand(I), IY->getOperand(I), Depth - 1);
    if (!OK)
      InProgress.erase({X, Y});
    return OK;
  }

  // Position I of both blocks compares equal (non-phi). Memory opcodes
  // additionally require equal volatile/atomic flags (callers already
  // rejected volatile; check ordering flags stay equal).
  bool insnEq(const Instruction *X, const Instruction *Y, unsigned Depth) {
    if (X->getOpcode() != Y->getOpcode())
      return false;
    if (X->getType() != Y->getType())
      return false;
    if (X->getNumOperands() != Y->getNumOperands())
      return false;
    for (unsigned I = 0; I < X->getNumOperands(); ++I)
      if (!valEq(X->getOperand(I), Y->getOperand(I), Depth))
        return false;
    return true;
  }
};


// Scaled byte-count bound: the normalized lifter expresses the vector
// trip count in bytes as shl(zext(lshr(N, K)), M), i.e. (N >> K) whole
// groups of (1 << M) bytes (corpus fill_vec: (N >> 3) groups of 32
// bytes). Value facts used (all exact unsigned arithmetic, N >= 0 on
// the folded path where the dispatch already checked N >= 1):
//   - byteBound == (N >> K) * (1 << M), so the loop covers exactly
//     (N >> K) * ((1 << M) / ElemBytes) elements when (1 << M) is a
//     multiple of the tail element size ElemBytes;
//   - that element count equals the and-mask n.vec (N with low E bits
//     cleared) when E == K and ((1 << M) / ElemBytes) == (1 << K).
// Returns the covered element step (1 << K) * TailStep... actually
// the element step per vector iteration: ((1 << M) / ElemBytes).
// Fills ElemStepOut and KOut on success, else 0.
uint64_t matchScaledVecBound(Value *VecBound, Value *N, uint64_t ElemBytes,
                             uint64_t &ElemStepOut, unsigned &KOut) {
  ElemStepOut = 0;
  KOut = 0;
  if (ElemBytes == 0)
    return 0;
  auto *Shl = dyn_cast<BinaryOperator>(VecBound);
  if (!Shl || Shl->getOpcode() != Instruction::Shl)
    return 0;
  auto *M = dyn_cast<ConstantInt>(Shl->getOperand(1));
  if (!M)
    return 0;
  uint64_t MVal = M->getZExtValue();
  if (MVal >= 63)
    return 0;
  uint64_t Scale = 1ULL << MVal;
  // The byte scale must be an exact multiple of one tail element.
  if (Scale < ElemBytes || Scale % ElemBytes != 0)
    return 0;
  Value *Z = Shl->getOperand(0);
  // Peel a zext/sext (the lifter widens the group count to i64).
  if (auto *Cast = dyn_cast<CastInst>(Z)) {
    if (Cast->getOpcode() != Instruction::ZExt &&
        Cast->getOpcode() != Instruction::SExt)
      return 0;
    Z = Cast->getOperand(0);
  }
  auto *Lshr = dyn_cast<BinaryOperator>(Z);
  if (!Lshr || Lshr->getOpcode() != Instruction::LShr)
    return 0;
  auto *K = dyn_cast<ConstantInt>(Lshr->getOperand(1));
  if (!K)
    return 0;
  unsigned KVal = (unsigned)K->getZExtValue();
  if (KVal == 0 || KVal >= 63)
    return 0;
  const Function &F =
      *cast<Instruction>(VecBound)->getParent()->getParent();
  Value *RootN = stripCountDerivation(N, F);
  if (stripCountDerivation(Lshr->getOperand(0), F) != RootN)
    return 0;
  // Element step per vector iteration must itself clear exactly the
  // low KVal element bits: Scale / ElemBytes == (1 << KVal).
  uint64_t ElemStep = Scale / ElemBytes;
  if (ElemStep != (1ULL << KVal))
    return 0;
  ElemStepOut = ElemStep;
  KOut = KVal;
  return ElemStep;
}

// n.vec check: vector bound is N with low bits cleared (and N, Mask
// with K >= 1 low zero bits, e.g. the exact ~(step-1) mask or a
// wider-than-N mask like the i64 0x7FFFFFF8 clang emits for a
// zext trip count) or N minus (N mod K*VF). Returns the step K*VF
// when the shape matches, else 0. Only the low cleared bits matter:
// n.vec is a multiple of 2^K below N, so the erased vector loop ran
// whole steps and the tail restarted at 0 covers the full range.
uint64_t matchVecBound(Value *VecBound, Value *N, uint64_t &StepOut) {
  const Function &F =
      *cast<Instruction>(VecBound)->getParent()->getParent();
  Value *RootN = stripCountDerivation(N, F);
  Value *Shape = VecBound;
  while (auto *Cast = dyn_cast<CastInst>(Shape)) {
    if (Cast->getOpcode() != Instruction::ZExt &&
        Cast->getOpcode() != Instruction::SExt &&
        Cast->getOpcode() != Instruction::Trunc)
      break;
    Shape = Cast->getOperand(0);
  }
  if (auto *And = dyn_cast<BinaryOperator>(Shape)) {
    if (And->getOpcode() == Instruction::And) {
      if (auto *C = dyn_cast<ConstantInt>(And->getOperand(1))) {
        unsigned Width = C->getBitWidth();
        APInt Mask = C->getValue();
        // Step from the low cleared bits: bit K is the lowest set
        // bit, so and-ing clears exactly the low K bits.
        unsigned K = Mask.countTrailingZeros();
        if (K >= 1 && K < 63) {
          Value *Base = And->getOperand(0);
          if (stripCountDerivation(Base, *And->getParent()->getParent()) ==
              RootN) {
            StepOut = 1ULL << K;
            return StepOut;
          }
        }
      }
    }
    if (And->getOpcode() == Instruction::Sub) {
      // N - (N mod K): operand 1 is urem(N', K).
      if (auto *Rem = dyn_cast<Instruction>(And->getOperand(1))) {
        if (Rem->getOpcode() == Instruction::URem) {
          if (auto *K = dyn_cast<ConstantInt>(Rem->getOperand(1))) {
            uint64_t Step = K->getZExtValue();
            if (Step >= 2) {
              Value *Base0 = stripCountDerivation(And->getOperand(0),
                                                  *And->getParent()->getParent());
              Value *Base1 = stripCountDerivation(Rem->getOperand(0),
                                                  *And->getParent()->getParent());
              if (Base0 == RootN && Base1 == RootN) {
                StepOut = Step;
                return Step;
              }
            }
          }
        }
      }
    }
  }
  return 0;
}

// True when V's def chain is pure w.r.t. region R: every instruction
// is side-effect free and all its operands are constants, arguments,
// or defined outside R (or recursively pure). Used for the top body's
// exit bound and the resume phi feeds.
bool isPureChain(Value *V, const DenseSet<BasicBlock *> &R,
                 DenseSet<const Value *> &Seen, unsigned Depth = 12) {
  if (isa<Constant>(V) || isa<Argument>(V))
    return true;
  if (!Seen.insert(V).second)
    return true;
  if (Depth == 0)
    return false;
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;
  if (!isTierErasable(*I))
    return false;
  if (auto *P = dyn_cast<PHINode>(I)) {
    // Phis in R with outside-R start values are ok if the backedge
    // value is also pure; the caller checks latch structure.
    for (unsigned K = 0; K < P->getNumIncomingValues(); ++K)
      if (!isPureChain(P->getIncomingValue(K), R, Seen, Depth - 1))
        return false;
    return true;
  }
  for (Value *Op : I->operands())
    if (!isPureChain(Op, R, Seen, Depth - 1))
      return false;
  return true;
}

// Collect the forward closure from Entry stopping at any block in Stop
// (Stop blocks are not included). Function exits (ret/unreachable) are
// also boundaries: the vector region never contains them (the middle
// block's direct-to-exit edge and the loop-exit merge stay outside R;
// step 2 drops their R incomings). EH terminators still fail.
bool collectRegion(BasicBlock *Entry, const DenseSet<BasicBlock *> &Stop,
                   DenseSet<BasicBlock *> &R, Function &F) {
  (void)F;
  SmallVector<BasicBlock *, 16> Work{Entry};
  while (!Work.empty()) {
    BasicBlock *BB = Work.pop_back_val();
    if (Stop.count(BB) || R.count(BB))
      continue;
    if (isa<ReturnInst>(BB->getTerminator()) ||
        isa<UnreachableInst>(BB->getTerminator()))
      continue;
    if (isa<InvokeInst>(BB->getTerminator()) ||
        isa<CatchSwitchInst>(BB->getTerminator()) ||
        isa<CleanupReturnInst>(BB->getTerminator()))
      return false;
    R.insert(BB);
    for (BasicBlock *S : successors(BB))
      Work.push_back(S);
  }
  return true;
}

struct Candidate {
  BasicBlock *D = nullptr;      // dispatch block
  BasicBlock *S = nullptr;      // scalar tail entry
  BasicBlock *L = nullptr;      // scalar single-block loop
  ScalarLoopInfo TailLoop;      // scalar tail latch info
  DenseSet<BasicBlock *> R;     // vector region to erase
  DenseSet<BasicBlock *> Chain; // dispatch-chain blocks to erase (D excl)
  // S-routing phis the tail dispatch condition depends on (strict
  // mode records, the post check after the top-body match keeps only
  // resume phis) and the accepted resume set.
  SmallVector<const PHINode *, 4> CondPhis;
  DenseSet<BasicBlock *> Starts; // zero-start paths into the scalar tail
  bool Versioned = false;        // full scalar loop is a separate guard arm
  BasicBlock *VersionedBody = nullptr;
  DenseSet<const PHINode *> Resume;
};

// A resume phi restarts its range at 0 on the folded path: its D
// incoming is constant 0, every other incoming comes from R, and at
// least one R incoming is Bound-derived (the same n.vec, possibly
// through casts like the trunc in corpus copy_bytes; anything that
// strips to the same trip-count root is restart-consistent because it
// is a pure function of N). Accumulators (fed by reductions) never
// qualify: routing or bounds depending on them would differ after the
// restart, so they keep failing the S-condition and bound checks.
void collectResumePhis(BasicBlock *S,
                       const DenseSet<BasicBlock *> &Starts,
                       const DenseSet<BasicBlock *> &R, Value *Bound,
                       Value *IVNext, const Function &F,
                       DenseSet<const PHINode *> &Out) {
  Value *RootB = stripCountDerivation(Bound, F);
  for (PHINode &Phi : S->phis()) {
    bool DZero = false, OK = true, Fed = false;
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      BasicBlock *IB = Phi.getIncomingBlock(I);
      Value *Inc = Phi.getIncomingValue(I);
      // Header mode (S == L): the backedge incoming is the latched
      // value, not a start or a resume feed.
      if (IB == S)
        continue;
      if (R.count(IB)) {
        // R feeds: resume values (bound-derived, incl. the vector
        // body's own IV.next which equals n.vec on its exit edge) or
        // restarts at the same zero the start edges feed.
        if (Inc == Bound || Inc == IVNext ||
            stripCountDerivation(Inc, F) == RootB) {
          Fed = true;
        } else if (auto *CZ = dyn_cast<ConstantInt>(Inc)) {
          if (!CZ->isZero())
            OK = false;
        } else {
          OK = false;
        }
      } else if (Starts.count(IB)) {
        auto *CZ = dyn_cast<ConstantInt>(Inc);
        if (!CZ || !CZ->isZero())
          OK = false;
        else
          DZero = true;
      } else {
        OK = false;
      }
    }
    if (OK && DZero && Fed)
      Out.insert(&Phi);
  }
}

// Shared tail checks: S entered from D (and R in strict mode), S phis
// agree, S-routing S-phi deps are recorded for the resume post check,
// and the single-block scalar self-loop L sits directly under S (or IS
// S itself in the dispatch-to-header form) with preds exactly {S, L}
// plus the start edges.
//
// DFS from From to To without entering R blocks. Used to prove the
// non-S successor of an R pred of S (the continue edge of a resume
// guard, or the middle block's direct loop-exit edge) stays a live
// post-tail block once R is erased. Reachability through erased chain
// blocks also counts: if the only paths were dead, the target is dead
// too and its phis' R incomings are dropped by the same surgery.
bool reachableSkippingRegion(BasicBlock *From, BasicBlock *To,
                             const DenseSet<BasicBlock *> &R) {
  if (From == To)
    return true;
  SmallVector<BasicBlock *, 16> Work{From};
  DenseSet<BasicBlock *> Seen{From};
  while (!Work.empty()) {
    BasicBlock *BB = Work.pop_back_val();
    for (BasicBlock *Su : successors(BB)) {
      if (Su == To)
        return true;
      if (R.count(Su) || !Seen.insert(Su).second)
        continue;
      Work.push_back(Su);
    }
    if (Seen.size() > 512)
      return false;
  }
  return false;
}

// preds exactly {S, L} plus the start edges.
bool matchTailShape(BasicBlock *S, BasicBlock *D, Candidate &C,
                    const DenseSet<BasicBlock *> &R, bool StrictPreds,
                    bool AllowNoR = false) {
  // S preds are: S itself when the scalar tail is a dispatch-to-header
  // loop (S == L, the normalized lifter jumps straight into the loop
  // header), the primary dispatch D, dispatch-chain blocks that fall
  // out to the scalar side (GCC's alias-guard restart), R resume
  // blocks, and optionally a small-trip guard that also enters S with
  // a fresh start value.
  bool HasD = false, HasR = false, Header = false;
  for (BasicBlock *Su : successors(S))
    if (Su == S)
      Header = true;
  C.Starts.clear();
  C.Starts.insert(D);
  for (BasicBlock *P : predecessors(S)) {
    if (P == S) {
      if (!Header)
        return false;
      continue;
    }
    if (P == D) {
      HasD = true;
      continue;
    }
    if (R.count(P)) {
      // R preds feed resume values; the OTHER successor of their
      // terminator must stay a live block after R is erased: another
      // R block (resume chain), D or a chain block (both erased), or
      // a post-tail block still reachable without R (the middle
      // block's direct loop-exit edge).
      auto *PB = dyn_cast<BranchInst>(P->getTerminator());
      BasicBlock *Other = nullptr;
      if (PB && PB->isConditional()) {
        if (PB->getSuccessor(0) == S)
          Other = PB->getSuccessor(1);
        else if (PB->getSuccessor(1) == S)
          Other = PB->getSuccessor(0);
      }
      if (!Other)
        return false;
      if (!R.count(Other) && Other != D && !C.Chain.count(Other) &&
          !reachableSkippingRegion(S, Other, R))
        return false;
      HasR = true;
      continue;
    }
    if (C.Chain.count(P)) {
      // Dispatch-chain fall-out (an alias/memcheck guard that failed
      // inside the chain): restarts the tail exactly like D does.
      C.Starts.insert(P);
      continue;
    }
    if (StrictPreds) {
      auto *PB = dyn_cast<BranchInst>(P->getTerminator());
      bool StartsAtZero = PB && PB->isConditional() &&
                          ((PB->getSuccessor(0) == D &&
                            PB->getSuccessor(1) == S) ||
                           (PB->getSuccessor(1) == D &&
                            PB->getSuccessor(0) == S));
      if (!StartsAtZero)
        return false;
      C.Starts.insert(P);
    }
  }
  if (!HasD || (StrictPreds && !HasR && !AllowNoR))
    return false;
  // Every start-edge incoming to an S phi must carry the same value:
  // the restart is one fresh start, whether it arrives through D, a
  // chain fall-out, or a small-trip guard. R incomings (resume feeds)
  // and the backedge in header mode are not start edges.
  if (StrictPreds) {
    for (PHINode &Phi : S->phis()) {
      Value *V0 = nullptr;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
        BasicBlock *IB = Phi.getIncomingBlock(I);
        if (IB == S || R.count(IB))
          continue;
        if (!C.Starts.count(IB))
          return false;
        Value *Inc = Phi.getIncomingValue(I);
        if (!V0)
          V0 = Inc;
        else if (Inc != V0)
          return false;
      }
    }
  }
  // S terminator condition deps on S phis are recorded (strict only;
  // tier dispatches between vector levels legitimately route on
  // N-derived conditions, and R membership decides later).
  // Resume-ness needs the vector bound, known only after the top-body
  // match, so matchDispatch verifies the recorded phis against the
  // resume set afterwards.
  if (StrictPreds) {
    C.CondPhis.clear();
    if (auto *Br = dyn_cast<BranchInst>(S->getTerminator())) {
      if (Br->isConditional()) {
        SmallPtrSet<const Value *, 16> Seen2;
        SmallVector<const Value *, 8> W2{Br->getCondition()};
        while (!W2.empty()) {
          const Value *V = W2.pop_back_val();
          if (!Seen2.insert(V).second)
            continue;
          if (const auto *P = dyn_cast<PHINode>(V)) {
            if (P->getParent() == S)
              C.CondPhis.push_back(P);
            continue;
          }
          if (const auto *I = dyn_cast<Instruction>(V)) {
            if (I->getParent() == S) {
              for (const Value *Op : I->operands())
                W2.push_back(Op);
            }
          }
        }
      }
    }
  }
  // L: single successor latch of S that is a single-block self-loop
  // with preds exactly {S, L} (start edges aside); in header mode S is
  // itself that loop. In loose mode (R unknown) any successor may be
  // L; the strict pass re-validates after R is collected.
  for (BasicBlock *Succ : successors(S)) {
    if (R.count(Succ))
      continue;
    ScalarLoopInfo L;
    if (Succ == S) {
      if (!matchScalarLoop(S, L))
        continue;
      C.L = S;
      C.TailLoop = L;
      return true;
    }
    if (!matchScalarLoop(Succ, L))
      continue;
    bool PredsOK = true;
    for (BasicBlock *P : predecessors(Succ)) {
      if (P != S && P != Succ)
        PredsOK = false;
    }
    if (!PredsOK)
      continue;
    C.L = Succ;
    C.TailLoop = L;
    return true;
  }
  SmallVector<BasicBlock *, 8> Work;
  DenseSet<BasicBlock *> Seen;
  for (BasicBlock *Succ : successors(S))
    if (!R.count(Succ) && Seen.insert(Succ).second)
      Work.push_back(Succ);
  while (!Work.empty()) {
    BasicBlock *BB = Work.pop_back_val();
    if (R.count(BB))
      continue;
    ScalarLoopInfo Loop;
    if (matchScalarLoop(BB, Loop)) {
      C.L = BB;
      C.TailLoop = Loop;
      return true;
    }
    for (BasicBlock *Succ : successors(BB))
      if (!R.count(Succ) && Seen.insert(Succ).second)
        Work.push_back(Succ);
  }
  return false;
}
// Loose: find L under S before R is known (D edge only).
bool matchScalarTailLoose(BasicBlock *S, BasicBlock *D, Candidate &C) {
  DenseSet<BasicBlock *> Empty;
  return matchTailShape(S, D, C, Empty, false);
}

// Strict: S entered from D and R only, phis agree.
bool matchScalarTail(BasicBlock *S, BasicBlock *D,
                     const DenseSet<BasicBlock *> &R, Candidate &C,
                     bool AllowNoR = false) {
  return matchTailShape(S, D, C, R, true, AllowNoR);
}
// Top vector body: single-block self-loop, IV from constant 0,
// constant step K >= 2, exit == (IV.next == n.vec). n.vec must match
// the trip count N (masked or sub/urem form) and the same n.vec value
// must feed S's resume phi from an R block.
//
// Multi-tier cascades (AVX2 8-wide + 4-wide + scalar, corpus fill_vec):
// every extra vector tier is a secondary body: same checks, except
// the IV may start at an R-defined resume value instead of constant 0
// (it restarts inside the erased region, so no S resume feed is
// needed) and only the direct bound form is accepted. At least one
// primary body is required per candidate.
bool matchTierBound(BasicBlock *B, ScalarLoopInfo &V, Candidate &C,
                    Function &F, Value *Bound, uint64_t Step,
                    const DenseSet<const PHINode *> &Resume) {
  // Bound must be pure w.r.t. R.
  DenseSet<const Value *> Seen;
  if (!isPureChain(Bound, C.R, Seen))
    return false;
  // Tail bound must derive from the same N: directly, or as a
  // remainder (N minus a resume phi, the byte-tail shape in corpus
  // copy_bytes where the scalar latch counts the leftover).
  Value *TailBound = loopBound(C.TailLoop);
  if (!TailBound)
    return false;
  Value *NTail = stripCountDerivation(TailBound, F);
  uint64_t Found = 0;
  // Try bound directly, else through a pure chain root.
  if (!matchVecBound(Bound, NTail, Found)) {
    // Remainder form: sub(N, resume) or sub(resume, N) with the phi
    // side in the resume set.
    Value *NAlt = nullptr;
    if (auto *BO = dyn_cast<BinaryOperator>(NTail)) {
      if (BO->getOpcode() == Instruction::Sub) {
        Value *Op0 = stripCountDerivation(BO->getOperand(0), F);
        Value *Op1 = stripCountDerivation(BO->getOperand(1), F);
        bool Op0Res = isa<PHINode>(Op0) &&
                      Resume.count(cast<PHINode>(Op0));
        bool Op1Res = isa<PHINode>(Op1) &&
                      Resume.count(cast<PHINode>(Op1));
        if (Op0Res != Op1Res)
          NAlt = Op0Res ? Op1 : Op0;
      }
    }
    Value *NBase = NAlt ? NAlt : NTail;
    if (!matchVecBound(Bound, NBase, Found)) {
      // Scaled byte bound (normalized lifter): the vector trip count
      // in bytes, shl(zext(lshr(N, K)), M). The element count it
      // covers equals the and-mask n.vec when (1 << M) / ElemBytes
      // == (1 << K), i.e. one vector iteration strides exactly one
      // whole group of tail elements.
      const DataLayout &DL = F.getParent()->getDataLayout();
      uint64_t ElemBytes = 0;
      for (Instruction &I : *C.TailLoop.Header) {
        if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
          Type *ET = GEP->getSourceElementType();
          if (ET->isSized() && !ET->isAggregateType()) {
            uint64_t EB = DL.getTypeStoreSize(ET).getFixedValue();
            if (EB && (!ElemBytes || EB < ElemBytes))
              ElemBytes = EB;
          }
        }
      }
      uint64_t ElemStep = 0;
      unsigned KVal = 0;
      if (!ElemBytes ||
          !matchScaledVecBound(Bound, NBase, ElemBytes, ElemStep, KVal))
        return false;
      Found = ElemStep;
    }
  }
  // Interleaved groups: the latch step and the mask step must divide
  // each other (the mask step covers one group, the latch strides all
  // groups); anything else is not a vectorizer bound.
  if (Found != Step && Step % Found != 0 && Found % Step != 0)
    return false;
  return true;
}

bool matchVectorBody(BasicBlock *B, Candidate &C, Function &F,
                     bool Secondary) {
  ScalarLoopInfo V;
  if (!matchScalarLoop(B, V))
    return false;
  // IV start: constant 0 from the non-self predecessor (primary), or
  // additionally an R-defined resume value (secondary tiers restart
  // inside the erased region).
  for (unsigned I = 0; I < V.IV->getNumIncomingValues(); ++I) {
    if (V.IV->getIncomingBlock(I) == B)
      continue;
    Value *Start = V.IV->getIncomingValue(I);
    if (auto *C0 = dyn_cast<ConstantInt>(Start)) {
      if (!C0->isZero())
        return false;
      continue;
    }
    if (Secondary) {
      if (auto *I2 = dyn_cast<Instruction>(Start)) {
        if (C.R.count(I2->getParent()))
          continue;
      }
    }
    return false;
  }
  uint64_t Step = V.Step->getZExtValue();
  if (Step < 2)
    return false;
  Value *Bound = loopBound(V);
  if (!Bound)
    return false;
  Value *IVNext = nullptr;
  for (unsigned I = 0; I < V.IV->getNumIncomingValues(); ++I)
    if (V.IV->getIncomingBlock(I) == B)
      IVNext = V.IV->getIncomingValue(I);
  // The resume set is meaningful only for the primary body (it feeds
  // S); secondaries confirm their remainder against the candidate set
  // once the primary has filled it, else against their local set.
  DenseSet<const PHINode *> Local;
  const DenseSet<const PHINode *> *Resume = &C.Resume;
  if (!Secondary) {
    collectResumePhis(C.S, C.Starts, C.R, Bound, IVNext, F, C.Resume);
  } else if (C.Resume.empty()) {
    collectResumePhis(C.S, C.Starts, C.R, Bound, IVNext, F, Local);
    Resume = &Local;
  }
  if (matchTierBound(B, V, C, F, Bound, Step, *Resume))
    return true;
  if (!Secondary && C.Versioned) {
    for (Instruction &I : *B)
      if (isOrderedLaneReduction(I, *B))
        return true;
  }
  if (!Secondary && C.Versioned) {
    // the whole scalar path can replay N after deleting the vector tier.
    // Keep this separate from the usual resumed-tail bound proof.
    for (PHINode &Phi : C.S->phis()) {
      bool HasStart = false;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
        if (C.Starts.count(Phi.getIncomingBlock(I))) {
          HasStart = true;
          if (auto *D = dyn_cast<Instruction>(Phi.getIncomingValue(I)))
            if (C.R.count(D->getParent()))
              return false;
        }
      if (!HasStart)
        return false;
    }
    for (const PHINode *P : C.CondPhis)
      for (unsigned I = 0; I < P->getNumIncomingValues(); ++I)
        if (C.R.count(P->getIncomingBlock(I)))
          return false;
    for (BasicBlock *BB : C.R)
      for (Instruction &I : *BB)
        if (isa<CallBase>(&I) && !isa<DbgInfoIntrinsic>(&I))
          return false;
    return true;
  }
  return false;
}

// Full candidate match for one dispatch block D: S is the successor
// with a scalar self-loop latch directly under it (preds {S, L});
// R is everything reachable from the other successor without passing
// through S, L, or L's exit.

#define DEBUG_TYPE "unloopvec"
#define ULV_BAIL()                                                            \
  do {                                                                        \
    LLVM_DEBUG(::llvm::dbgs() << "unloopvec bail " << __LINE__ << " in "      \
                              << F.getName() << "\n");                        \
  } while (0)

Value *broadcastScalar(Value *V);
bool matchVersionedBody(BasicBlock *B, Candidate &C, Function &F);

bool isZeroStartValue(Value *V, BasicBlock *S,
                      const DenseSet<BasicBlock *> &Starts,
                      DenseSet<Value *> &Seen) {
  if (!Seen.insert(V).second)
    return true;
  if (auto *C = dyn_cast<ConstantInt>(V))
    return C->isZero();
  if (auto *P = dyn_cast<PHINode>(V)) {
    if (P->getParent() != S)
      return false;
    bool Found = false;
    for (unsigned I = 0; I < P->getNumIncomingValues(); ++I)
      if (Starts.count(P->getIncomingBlock(I))) {
        Found = true;
        auto *C = dyn_cast<ConstantInt>(P->getIncomingValue(I));
        if (!C || !C->isZero())
          return false;
      }
    return Found;
  }
  if (auto *Cast = dyn_cast<CastInst>(V))
    return isZeroStartValue(Cast->getOperand(0), S, Starts, Seen);
  if (auto *BO = dyn_cast<BinaryOperator>(V)) {
    if (auto *C = dyn_cast<ConstantInt>(BO->getOperand(1)))
      return C->isZero() &&
             isZeroStartValue(BO->getOperand(0), S, Starts, Seen);
  }
  return false;
}
bool matchDispatch(BasicBlock *D, Function &F, Candidate &C) {
  auto *Br = dyn_cast<BranchInst>(D->getTerminator());
  if (!Br || !Br->isConditional())
    return false;
  BasicBlock *S0 = Br->getSuccessor(0);
  BasicBlock *S1 = Br->getSuccessor(1);
  for (int Pick = 0; Pick < 2; ++Pick) {
    BasicBlock *S = Pick == 0 ? S0 : S1;
    BasicBlock *V = Pick == 0 ? S1 : S0;
    Candidate Cand;
    Cand.D = D;
    Cand.S = S;
    if (!matchScalarTailLoose(S, D, Cand)) {
      continue;
    }
    BasicBlock *L = Cand.L;
    DenseSet<BasicBlock *> Stop{S, L};
    for (BasicBlock *ES : successors(L))
      if (ES != L)
        Stop.insert(ES);
    for (BasicBlock &BB : F)
      if (isa<ReturnInst>(BB.getTerminator()))
        Stop.insert(&BB);
    DenseSet<BasicBlock *> R;
    if (!collectRegion(V, Stop, R, F)) {
      continue;
    }
    if (R.size() < 2) {
      continue;
    }
    Cand.R = R;
    Cand.L = nullptr;
    bool HasRegionPredecessor = false;
    for (BasicBlock *P : predecessors(S)) {
      if (P == S)
        continue; // header mode: the backedge is not a region entry
      if (!Cand.R.count(P))
        continue;
      auto *PB = dyn_cast<BranchInst>(P->getTerminator());
      bool Guard = PB && PB->isConditional() &&
                   (PB->getSuccessor(0) == S || PB->getSuccessor(1) == S);
      HasRegionPredecessor |= !Guard;
    }
    Cand.Versioned = !HasRegionPredecessor;
    // Compute the dispatch chain BEFORE the strict tail check so
    // the escape rule can see chain membership: chain blocks are
    // reachable from D without passing S/L/exits.
    DenseSet<BasicBlock *> Chain;
    {
      SmallVector<BasicBlock *, 8> Work{D};
      DenseSet<BasicBlock *> Seen;
      Seen.insert(S);
      // L unknown yet; use the loose L for the walk.
      if (Cand.L)
        Seen.insert(Cand.L);
      while (!Work.empty()) {
        BasicBlock *BB = Work.pop_back_val();
        for (BasicBlock *Su : successors(BB)) {
          if (Su == S || Seen.count(Su) || Cand.R.count(Su))
            continue;
          Seen.insert(Su);
          bool Bad = false;
          for (BasicBlock *SS : successors(Su)) {
            if (Seen.count(SS)) {
              Bad = true;
              break;
            }
          }
          if (Bad)
            continue;
          Chain.insert(Su);
          Work.push_back(Su);
        }
      }
    }
    Cand.Chain = Chain;
    if (!matchScalarTail(S, D, Cand.R, Cand, Cand.Versioned))
      continue;
    // Refresh the chain now that L is known (exclude L and its
    // exit from the walk).
    {
      Chain.clear();
      SmallVector<BasicBlock *, 8> Work{D};
      DenseSet<BasicBlock *> Seen;
      Seen.insert(S);
      Seen.insert(Cand.L);
      for (BasicBlock *ES : successors(Cand.L))
        if (ES != Cand.L)
          Seen.insert(ES);
      while (!Work.empty()) {
        BasicBlock *BB = Work.pop_back_val();
        for (BasicBlock *Su : successors(BB)) {
          if (Su == S || Seen.count(Su) || Cand.R.count(Su))
            continue;
          Seen.insert(Su);
          bool Bad = false;
          for (BasicBlock *SS : successors(Su)) {
            if (Seen.count(SS)) {
              Bad = true;
              break;
            }
          }
          if (Bad)
            continue;
          Chain.insert(Su);
          Work.push_back(Su);
        }
      }
      Cand.Chain = Chain;
    }
    for (BasicBlock *BB : Cand.R) {
      for (BasicBlock *P : predecessors(BB)) {
        if (Cand.R.count(P) || P == D || Chain.count(P))
          continue;
        ULV_BAIL();
        goto next_pick;
      }
    }
    // R cycles: every cycle is a single-block self-loop. Check via
    // SCC-lite: any block with a back edge from a different R block
    // fails. What each self-loop is (primary vector body, secondary
    // tier, or scalarized tier) is settled by the body classification
    // after the escape checks.
    for (BasicBlock *BB : Cand.R) {
      for (BasicBlock *Su : successors(BB)) {
        if (Su == BB)
          continue;
        if (Cand.R.count(Su)) {
          // Forward edge inside R is fine only if it cannot form a
          // multi-block cycle: check reachability back.
          SmallVector<BasicBlock *, 8> W{Su};
          DenseSet<BasicBlock *> V8;
          bool Cycles = false;
          while (!W.empty()) {
            BasicBlock *X = W.pop_back_val();
            if (X == BB) {
              Cycles = true;
              break;
            }
            if (!V8.insert(X).second)
              continue;
            if (!Cand.R.count(X))
              continue;
            for (BasicBlock *XS : successors(X))
              W.push_back(XS);
          }
          if (Cycles) {
            ULV_BAIL();
            goto next_pick;
          }
        }
      }
    }
    // No calls except intrinsics, no asm/volatile/atomic, no
    // ret/unreachable/trap, br-only terminators; FP binops in R need
    // reassoc flags. Loop metadata (!llvm.loop) on R terminators is
    // dropped at rewrite time (step 2b strips it before folding, so
    // no metadata graph keeps an erased block alive).
    for (BasicBlock *BB : Cand.R) {
      for (Instruction &I : *BB) {
        if (I.isTerminator()) {
          if (!isa<BranchInst>(&I)) {
            ULV_BAIL();
            goto next_pick;
          }
          continue;
        }
        if (!isTierErasable(I)) {
          ULV_BAIL();
          goto next_pick;
        }
        if (!fpReassocOK(I) && !isOrderedLaneReduction(I, *BB)) {
          ULV_BAIL();
          goto next_pick;
        }
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (isa<DbgInfoIntrinsic>(&I))
            continue;
          const Function *CF = CB->getCalledFunction();
          if (!CF || !CF->isIntrinsic()) {
            ULV_BAIL();
            goto next_pick;
          }
        }
        if (isa<CallInst>(&I) && cast<CallInst>(&I)->isInlineAsm()) {
          ULV_BAIL();
          goto next_pick;
        }
      }
    }
    // Escape: every R def must be dead after the fold, with phis as
    // the only outsiders. S phis, debug intrinsics, and outsider
    // phis fed only through R-block edges are fine (steps 2/2b drop
    // those edges). A NON-phi use outside R is fine only when its
    // user block is itself dead with R: n.vec feeding the middle
    // cmp.n and the latch cmp is dead once both blocks are erased.
    // Anything reaching a kept block (L's exit, loop-exit merges
    // through non-R edges) bails.
    for (BasicBlock *BB : Cand.R) {
      for (Instruction &I : *BB) {
        if (I.getType()->isVoidTy())
          continue;
        bool VecDef = isVectorTyped(I.getType());
        if (!VecDef) {
          if (auto *CB = dyn_cast<CallBase>(&I)) {
            if (const Function *CF = CB->getCalledFunction()) {
              StringRef N = CF->getName();
              if (N.starts_with("llvm.vector.reduce.") ||
                  N.starts_with("llvm.masked.") || N.starts_with("llvm.vp."))
                VecDef = true;
            }
          }
        }
        for (Use &U : I.uses()) {
          auto *UI = cast<Instruction>(U.getUser());
          BasicBlock *UB = UI->getParent();
          if (Cand.R.count(UB))
            continue;
          if (isa<DbgInfoIntrinsic>(UI))
            continue;
          // Vector-typed defs (wide phis, wide loads, bin.rdx) must
          // never feed kept computation. Two outsiders are fine:
          // S phis (handled below), and non-S phis that keep a
          // non-R incoming after the R edges drop (the loop-exit
          // LCSSA merge keeps its L edge; step 2 drops the R edge).
          // Anything else (non-phi kept users) bails.
          if (!isa<PHINode>(UI)) {
            // Non-phi user in a dead chain block (vector.memcheck
            // chain feeding the dispatch): dead with R, fine.
            if (Cand.Chain.count(UB))
              continue;
            ULV_BAIL();
            goto next_pick;
          }
          if (UB == S)
            continue;
          if (VecDef) {
            // R-edge incomings drop in step 2; fine when the phi
            // keeps a non-R incoming (loop-exit LCSSA merge keeps
            // its L edge).
            auto *P = cast<PHINode>(UI);
            bool ValFromNonR = false;
            bool KeepsNonR = false;
            for (unsigned K = 0; K < P->getNumIncomingValues(); ++K) {
              if (P->getIncomingValue(K) != &I)
                continue;
              if (!Cand.R.count(P->getIncomingBlock(K)))
                ValFromNonR = true;
            }
            for (unsigned K = 0; K < P->getNumIncomingValues(); ++K) {
              if (!Cand.R.count(P->getIncomingBlock(K)))
                KeepsNonR = true;
            }
            if (!ValFromNonR && KeepsNonR)
              continue;
            ULV_BAIL();
            goto next_pick;
          }
          bool AllR = true;
          for (unsigned K = 0; K < cast<PHINode>(UI)->getNumIncomingValues(); ++K) {
            auto *P = cast<PHINode>(UI);
            if (P->getIncomingValue(K) != &I)
              continue;
            if (!Cand.R.count(P->getIncomingBlock(K)))
              AllR = false;
          }
          if (AllR)
            continue;
          ULV_BAIL();
          goto next_pick;
        }
      }
    }
    // Tail IV starts at an S phi whose D-incoming is constant 0;
    // other tail phis start at S phis or outside-R values. In header
    // mode (S == L) the S-phi rules above plus the resume post check
    // after the body match cover this, so this block is skipped.
    if (Cand.L != Cand.S && !Cand.Versioned) {
      bool IVOK = false;
      for (unsigned I = 0; I < Cand.TailLoop.IV->getNumIncomingValues(); ++I) {
        BasicBlock *IB = Cand.TailLoop.IV->getIncomingBlock(I);
        Value *Inc = Cand.TailLoop.IV->getIncomingValue(I);
        if (IB == Cand.L)
          continue;
        if (IB != S) {
          ULV_BAIL();
          goto next_pick;
        }
        if (auto *CZ = dyn_cast<ConstantInt>(Inc)) {
          if (CZ->isZero())
            IVOK = true;
        } else {
          DenseSet<Value *> Seen;
          IVOK = isZeroStartValue(Inc, S, Cand.Starts, Seen);
        }
      }
      if (!IVOK) {
        ULV_BAIL();
        goto next_pick;
      }
      for (PHINode &Phi : Cand.L->phis()) {
        if (&Phi == Cand.TailLoop.IV)
          continue;
        for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
          if (Phi.getIncomingBlock(I) == Cand.L)
            continue;
          Value *Inc = Phi.getIncomingValue(I);
          if (Phi.getIncomingBlock(I) != S) {
            ULV_BAIL();
            goto next_pick;
          }
          if (auto *P = dyn_cast<PHINode>(Inc)) {
            if (P->getParent() != S) {
              ULV_BAIL();
              goto next_pick;
            }
          } else if (auto *I2 = dyn_cast<Instruction>(Inc)) {
            if (Cand.R.count(I2->getParent())) {
              ULV_BAIL();
              goto next_pick;
            }
          }
        }
      }
    }
    // Top vector body check: at least one primary body (IV from
    // constant 0, S resume feed), plus any secondary tiers (IV from an
    // R-defined resume, same trip count). Every R self-loop must be
    // one or the other; other R blocks must not be self-loops (the
    // single-entry and cycle checks already confined cycles to
    // self-loops).
    {
      bool Found = false;
      // The top body is an R block whose IV starts at constant 0
      // from D or the chain.
      for (BasicBlock *BB : Cand.R) {
        Candidate Probe = Cand;
        bool VM = Cand.Versioned && matchVersionedBody(BB, Probe, F);
        if (matchVectorBody(BB, Probe, F, false) || VM) {
          if (Cand.Versioned)
            Probe.VersionedBody = BB;
          Cand = Probe;
          Found = true;
          break;
        }
      }
      if (!Found) {
        ULV_BAIL();
        goto next_pick;
      }
      for (BasicBlock *BB : Cand.R) {
        bool Self = false;
        for (BasicBlock *Su : successors(BB))
          if (Su == BB)
            Self = true;
        if (!Self)
          continue;
        Candidate Probe = Cand;
        if ((Cand.Versioned && BB == Cand.VersionedBody) ||
            matchVectorBody(BB, Probe, F, false) ||
            matchVectorBody(BB, Probe, F, true))
          continue;
        ULV_BAIL();
        goto next_pick;
      }
    }
    // Header mode (S == L): the loop IV is itself an S phi, so it
    // must be a proven resume phi -- start edges feed 0 and every R
    // incoming is bound-derived -- otherwise the restart at element 0
    // is unproven and we bail.
    if (Cand.L == Cand.S && !Cand.Resume.count(Cand.TailLoop.IV)) {
      ULV_BAIL();
      goto next_pick;
    }

    // S-routing post check: every S phi the tail dispatch routes on
    // must be a resume phi (D-incoming 0, R-incomings Bound-derived,
    // so the routing is a pure function of N on the folded path).
    // Anything else (notably accumulators) still bails.
    for (const PHINode *P : Cand.CondPhis) {
      if (!Cand.Resume.count(P)) {
        ULV_BAIL();
        goto next_pick;
      }
    }
    C = Cand;
    return true;
  next_pick:;
  }
  return false;
}
// Prove the GCC-style guarded loop versioning shape before discarding its
// vector arm. The scalar fallback and vector loop must share memory bases,
// the vector lanes must implement scalar IVs 4*i through 4*i+3, the mask
// must cover exactly floor(N/4)*4 elements, and all three guarded scalar
// epilogue lanes must continue that range to N.

Value *broadcastScalar(Value *V);

bool matchVersionedBody(BasicBlock *B, Candidate &C, Function &F) {
  ScalarLoopInfo Scalar;
  if (!matchScalarLoop(C.L, Scalar)) {
    return false;
  }
  auto *ScalarCmp = dyn_cast<ICmpInst>(
      cast<BranchInst>(C.L->getTerminator())->getCondition());
  Value *ScalarBound = loopBound(Scalar);
  if (!ScalarCmp || !ScalarBound)
    return false;
  Value *Start = nullptr;
  for (unsigned I = 0; I < Scalar.IV->getNumIncomingValues(); ++I)
    if (Scalar.IV->getIncomingBlock(I) != C.L)
      Start = Scalar.IV->getIncomingValue(I);
  auto *StartZero = dyn_cast_or_null<ConstantInt>(Start);
  if (!StartZero || !StartZero->isZero())
    return false;

  ScalarLoopInfo Vector;
  if (!matchScalarLoop(B, Vector)) {
    return false;
  }
  auto *VectorStep = dyn_cast<ConstantInt>(Vector.Step);
  Value *VectorBound = loopBound(Vector);
  Value *VectorStart = nullptr;
  for (unsigned I = 0; I < Vector.IV->getNumIncomingValues(); ++I)
    if (Vector.IV->getIncomingBlock(I) != B)
      VectorStart = Vector.IV->getIncomingValue(I);
  auto *VectorStartZero = dyn_cast_or_null<ConstantInt>(VectorStart);
  auto *ScalarStep = dyn_cast<ConstantInt>(Scalar.Step);
  if (!VectorStartZero || !VectorStartZero->isZero() || !ScalarStep ||
      !ScalarStep->isOne() || !VectorStep ||
      VectorStep->getZExtValue() != 4 || !VectorBound)
    return false;

  SmallVector<LoadInst *, 2> ScalarLoads, VectorLoads;
  SmallVector<StoreInst *, 2> ScalarStores, VectorStores;
  SmallVector<BinaryOperator *, 2> ScalarAdds, VectorAdds;
  for (Instruction &I : *C.L) {
    if (auto *L = dyn_cast<LoadInst>(&I))
      ScalarLoads.push_back(L);
    else if (auto *St = dyn_cast<StoreInst>(&I))
      ScalarStores.push_back(St);
    else if (auto *BO = dyn_cast<BinaryOperator>(&I))
      if (BO->getOpcode() == Instruction::Add)
        ScalarAdds.push_back(BO);
  }
  for (Instruction &I : *B) {
    if (auto *L = dyn_cast<LoadInst>(&I))
      VectorLoads.push_back(L);
    else if (auto *St = dyn_cast<StoreInst>(&I))
      VectorStores.push_back(St);
    else if (auto *BO = dyn_cast<BinaryOperator>(&I))
      if (BO->getOpcode() == Instruction::Add)
        VectorAdds.push_back(BO);
  }
  ScalarAdds.clear();
  VectorAdds.clear();
  if (!ScalarStores.empty())
    if (auto *BO = dyn_cast<BinaryOperator>(
            ScalarStores.front()->getValueOperand()))
      if (BO->getOpcode() == Instruction::Add)
        ScalarAdds.push_back(BO);
  if (!VectorStores.empty())
    if (auto *BO = dyn_cast<BinaryOperator>(
            VectorStores.front()->getValueOperand()))
      if (BO->getOpcode() == Instruction::Add)
        VectorAdds.push_back(BO);
  if (ScalarLoads.size() != 1 || ScalarStores.size() != 1 ||
      ScalarAdds.size() != 1 || VectorLoads.size() != 1 ||
      VectorStores.size() != 1 || VectorAdds.size() != 1) {
    return false;
  }
  auto *VT = dyn_cast<FixedVectorType>(VectorLoads[0]->getType());
  if (!VT || VT->getNumElements() != 4 ||
      VT->getElementType() != ScalarLoads[0]->getType() ||
      VectorStores[0]->getValueOperand() != VectorAdds[0]) {
    return false;
  }
  if (VectorAdds[0]->getOperand(0) != VectorLoads[0] &&
      VectorAdds[0]->getOperand(1) != VectorLoads[0])
    return false;
  Value *VectorOther = VectorAdds[0]->getOperand(
      VectorAdds[0]->getOperand(0) == VectorLoads[0] ? 1 : 0);
  Value *ScalarOther = ScalarAdds[0]->getOperand(
      ScalarAdds[0]->getOperand(0) == ScalarLoads[0] ? 1 : 0);
  if (broadcastScalar(VectorOther) != ScalarOther ||
      (ScalarAdds[0]->getOperand(0) != ScalarLoads[0] &&
       ScalarAdds[0]->getOperand(1) != ScalarLoads[0]))
    return false;
  auto *SOverflow = dyn_cast<OverflowingBinaryOperator>(ScalarAdds[0]);
  auto *VOverflow = dyn_cast<OverflowingBinaryOperator>(VectorAdds[0]);
  if (ScalarLoads[0]->isVolatile() || ScalarLoads[0]->isAtomic() ||
      VectorLoads[0]->isVolatile() || VectorLoads[0]->isAtomic() ||
      ScalarStores[0]->isVolatile() || ScalarStores[0]->isAtomic() ||
      VectorStores[0]->isVolatile() || VectorStores[0]->isAtomic() ||
      !SOverflow || !VOverflow ||
      SOverflow->hasNoSignedWrap() != VOverflow->hasNoSignedWrap() ||
      SOverflow->hasNoUnsignedWrap() != VOverflow->hasNoUnsignedWrap())
    return false;
  auto *VStorePtr = dyn_cast<GetElementPtrInst>(
      VectorStores[0]->getPointerOperand());
  auto *VLoadPtr = dyn_cast<GetElementPtrInst>(
      VectorLoads[0]->getPointerOperand());
  auto *SStorePtr = dyn_cast<GetElementPtrInst>(
      ScalarStores[0]->getPointerOperand());
  auto *SLoadPtr = dyn_cast<GetElementPtrInst>(
      ScalarLoads[0]->getPointerOperand());
  if (!VStorePtr || !VLoadPtr || !SStorePtr || !SLoadPtr)
    return false;
  if (VLoadPtr->getSourceElementType() != VT->getElementType() ||
      VStorePtr->getSourceElementType() != VT->getElementType() ||
      SLoadPtr->getSourceElementType() != ScalarLoads[0]->getType() ||
      SStorePtr->getSourceElementType() != ScalarLoads[0]->getType())
    return false;
  auto *VectorIndex = dyn_cast<PHINode>(VLoadPtr->getOperand(1));
  auto *ScalarIndex = dyn_cast<PHINode>(SLoadPtr->getOperand(1));
  auto *VectorStoreIndex = dyn_cast<PHINode>(VStorePtr->getOperand(1));
  auto *ScalarStoreIndex = dyn_cast<PHINode>(SStorePtr->getOperand(1));
  auto *Mask = dyn_cast<BinaryOperator>(VectorBound);
  auto *MaskValue = Mask && Mask->getOpcode() == Instruction::And
                        ? dyn_cast<ConstantInt>(Mask->getOperand(1))
                        : nullptr;
  if (!MaskValue || MaskValue->getSExtValue() != -4 ||
      Mask->getOperand(0) != ScalarBound ||
      VectorIndex != Vector.IV || VectorStoreIndex != Vector.IV ||
      ScalarIndex != Scalar.IV || ScalarStoreIndex != Scalar.IV)
    return false;
  BasicBlock *AliasGuard = nullptr;
  for (BasicBlock *BB : C.R) {
    auto *Br = dyn_cast<BranchInst>(BB->getTerminator());
    if (!Br || !Br->isConditional() ||
        (Br->getSuccessor(0) != C.S && Br->getSuccessor(1) != C.S))
      continue;
    BasicBlock *Other = Br->getSuccessor(0) == C.S
                            ? Br->getSuccessor(1)
                            : Br->getSuccessor(0);
    if (C.R.count(Other)) {
      AliasGuard = BB;
      break;
    }
  }
  if (!AliasGuard && C.D != C.S)
    AliasGuard = C.D;
  auto *DispatchBr = AliasGuard
                         ? dyn_cast<BranchInst>(AliasGuard->getTerminator())
                         : nullptr;
  auto *AliasCmp = DispatchBr
                       ? dyn_cast<ICmpInst>(DispatchBr->getCondition())
                       : nullptr;
  auto *Difference =
      AliasCmp ? dyn_cast<BinaryOperator>(AliasCmp->getOperand(0)) : nullptr;
  auto *DestAddr = Difference
                       ? dyn_cast<PtrToIntInst>(Difference->getOperand(0))
                       : nullptr;
  auto *SourceOffset =
      Difference ? dyn_cast<BinaryOperator>(Difference->getOperand(1))
                 : nullptr;
  auto *SourceAddr = SourceOffset
                         ? dyn_cast<PtrToIntInst>(SourceOffset->getOperand(0))
                         : nullptr;
  auto *Offset = SourceOffset
                     ? dyn_cast<ConstantInt>(SourceOffset->getOperand(1))
                     : nullptr;
  auto *Limit = AliasCmp ? dyn_cast<ConstantInt>(AliasCmp->getOperand(1))
                         : nullptr;
  const DataLayout &DL = F.getParent()->getDataLayout();
  uint64_t ElemBytes = DL.getTypeStoreSize(VT->getElementType()).getFixedValue();
  if (!DispatchBr || !DispatchBr->isConditional() ||
      !C.R.count(DispatchBr->getSuccessor(0)) || !AliasCmp ||
      AliasCmp->getPredicate() != ICmpInst::ICMP_UGT || !Difference ||
      Difference->getOpcode() != Instruction::Sub || !DestAddr ||
      DestAddr->getPointerOperand() != VStorePtr->getPointerOperand() ||
      !SourceAddr ||
      SourceAddr->getPointerOperand() != VLoadPtr->getPointerOperand() ||
      !Offset || !Limit ||
      Offset->getZExtValue() + Limit->getZExtValue() !=
          VT->getNumElements() * ElemBytes - ElemBytes)
    return false;
  SmallVector<LoadInst *, 4> TailLoads;
  SmallVector<StoreInst *, 4> TailStores;
  for (BasicBlock *BB : C.R) {
    if (BB == B)
      continue;
    for (Instruction &I : *BB) {
      if (auto *L = dyn_cast<LoadInst>(&I)) {
        if (L->isVolatile() || L->isAtomic())
          return false;
        TailLoads.push_back(L);
      } else if (auto *St = dyn_cast<StoreInst>(&I)) {
        if (St->isVolatile() || St->isAtomic())
          return false;
        TailStores.push_back(St);
      } else if (!I.isTerminator() && !isa<PHINode>(&I) &&
                 !isa<GetElementPtrInst>(&I) && !isa<ICmpInst>(&I) &&
                 !isa<BinaryOperator>(&I) && !isa<InsertElementInst>(&I) &&
                 !isa<ShuffleVectorInst>(&I))
        return false;
    }
  }
  if (TailLoads.size() != 3 || TailStores.size() != 3)
    return false;
  auto MatchTailBranch = [&](BasicBlock *BB, Value *Count,
                             BasicBlock *Continue, BasicBlock *Exit) {
    auto *Br = dyn_cast<BranchInst>(BB->getTerminator());
    if (!Br || !Br->isConditional())
      return false;
    auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
    bool CountSide =
        Cmp && Cmp->getPredicate() == ICmpInst::ICMP_ULT &&
        ((Cmp->getOperand(0) == Count &&
          Cmp->getOperand(1) == ScalarBound) ||
         (Cmp->getOperand(1) == Count &&
          Cmp->getOperand(0) == ScalarBound));
    return CountSide && Br->getSuccessor(0) == Continue &&
           Br->getSuccessor(1) == Exit;
  };
  SmallVector<StoreInst *, 3> TailOrder;
  SmallVector<Value *, 2> TailNext;
  Value *TailIndex = VectorBound;
  for (unsigned Lane = 0; Lane != 3; ++Lane) {
    LoadInst *TailLoad = nullptr;
    for (LoadInst *L : TailLoads) {
      auto *G = dyn_cast<GetElementPtrInst>(L->getPointerOperand());
      if (G && G->getOperand(1) == TailIndex &&
          G->getPointerOperand() == SLoadPtr->getPointerOperand())
        TailLoad = L;
    }
    if (!TailLoad)
      return false;
    StoreInst *TailStore = nullptr;
    for (StoreInst *St : TailStores) {
      auto *G = dyn_cast<GetElementPtrInst>(St->getPointerOperand());
      auto *Add = dyn_cast<BinaryOperator>(St->getValueOperand());
      if (!G || G->getOperand(1) != TailIndex ||
          G->getPointerOperand() != SStorePtr->getPointerOperand() ||
          !Add || Add->getOpcode() != Instruction::Add)
        continue;
      if ((Add->getOperand(0) == TailLoad &&
           Add->getOperand(1) == ScalarOther) ||
          (Add->getOperand(1) == TailLoad &&
           Add->getOperand(0) == ScalarOther))
        TailStore = St;
    }
    if (!TailStore)
      return false;
    TailOrder.push_back(TailStore);
    TailLoads.erase(std::find(TailLoads.begin(), TailLoads.end(), TailLoad));
    TailStores.erase(std::find(TailStores.begin(), TailStores.end(), TailStore));
    if (Lane != 2) {
      BinaryOperator *Next = nullptr;
      for (Instruction &I : *TailStore->getParent())
        if (auto *BO = dyn_cast<BinaryOperator>(&I))
          if (BO->getOpcode() == Instruction::Add &&
              ((BO->getOperand(0) == TailIndex &&
                isa<ConstantInt>(BO->getOperand(1)) &&
                cast<ConstantInt>(BO->getOperand(1))->isOne()) ||
               (BO->getOperand(1) == TailIndex &&
                isa<ConstantInt>(BO->getOperand(0)) &&
                cast<ConstantInt>(BO->getOperand(0))->isOne())))
            Next = BO;
      if (!Next)
        return false;
      TailNext.push_back(Next);
      TailIndex = Next;
    }
  }
  BasicBlock *VectorExit = nullptr;
  for (BasicBlock *Succ : successors(B))
    if (Succ != B)
      VectorExit = Succ;
  BasicBlock *ScalarExit = nullptr;
  for (BasicBlock *Succ : successors(C.L))
    if (Succ != C.L)
      ScalarExit = Succ;
  if (!VectorExit || !ScalarExit ||
      !MatchTailBranch(VectorExit, VectorBound,
                       TailOrder.front()->getParent(), ScalarExit))
    return false;
  for (unsigned I = 0; I < TailNext.size(); ++I)
    if (!MatchTailBranch(TailOrder[I]->getParent(), TailNext[I],
                         TailOrder[I + 1]->getParent(), ScalarExit))
      return false;
  auto *Last = dyn_cast<BranchInst>(
      TailOrder.back()->getParent()->getTerminator());
  return Last && !Last->isConditional() &&
         Last->getSuccessor(0) == ScalarExit;
}


// Counted loops can lower wide copies to i128 load/store pairs, or repeated
// fills to vector splats. Split only these proven memory shapes. Keep all
// copy loads before their stores so each chunk retains read-before-write
// behavior. Arithmetic vector loops remain unchanged until their lane
// semantics can be established.
Value *broadcastScalar(Value *V) {
  auto *Shuffle = dyn_cast<ShuffleVectorInst>(V);
  if (!Shuffle)
    return nullptr;
  auto *Insert = dyn_cast<InsertElementInst>(Shuffle->getOperand(0));
  if (!Insert)
    return nullptr;
  auto *Index = dyn_cast<ConstantInt>(Insert->getOperand(2));
  if (!Index || !Index->isZero())
    return nullptr;
  for (int I = 0; I != static_cast<int>(Shuffle->getShuffleMask().size()); ++I)
    if (Shuffle->getMaskValue(I) != 0)
      return nullptr;
  return Insert->getOperand(1);
}

bool lowerUniformStoreUnroll(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  bool Changed = false;
  for (BasicBlock &BB : F) {
    ScalarLoopInfo Loop;
    if (!matchScalarLoop(&BB, Loop))
      continue;
    auto *Step = dyn_cast<ConstantInt>(Loop.Step);
    if (!Step || Step->isZero() || Step->getValue().getActiveBits() > 63)
      continue;
    uint64_t ByteStep = Step->getZExtValue();
    PHINode *PtrPhi = nullptr;
    GetElementPtrInst *PtrNext = nullptr;
    for (PHINode &Phi : BB.phis()) {
      if (!Phi.getType()->isPointerTy())
        continue;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
        if (Phi.getIncomingBlock(I) != &BB)
          continue;
        auto *GEP = dyn_cast<GetElementPtrInst>(Phi.getIncomingValue(I));
        if (!GEP || GEP->getPointerOperand() != &Phi)
          continue;
        APInt Offset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
        if (!GEP->accumulateConstantOffset(DL, Offset) ||
            Offset.getActiveBits() > 63 || Offset.getZExtValue() != ByteStep)
          continue;
        PtrPhi = &Phi;
        PtrNext = GEP;
        break;
      }
      if (PtrPhi)
        break;
    }
    if (!PtrPhi)
      continue;

    SmallVector<std::pair<StoreInst *, int64_t>, 8> Stores;
    Value *StoredValue = nullptr;
    uint64_t ElemBytes = 0;
    bool SafeBody = true;
    for (Instruction &I : BB) {
      if (auto *Store = dyn_cast<StoreInst>(&I)) {
        if (Store->isVolatile() || Store->isAtomic() ||
            !Store->getValueOperand()->getType()->isSized()) {
          SafeBody = false;
          break;
        }
        TypeSize Size = DL.getTypeStoreSize(Store->getValueOperand()->getType());
        if (Size.isScalable()) {
          SafeBody = false;
          break;
        }
        uint64_t EB = Size.getFixedValue();
        if (!EB || (ElemBytes && ElemBytes != EB) ||
            (StoredValue && StoredValue != Store->getValueOperand())) {
          SafeBody = false;
          break;
        }
        ElemBytes = EB;
        StoredValue = Store->getValueOperand();
        Value *Ptr = Store->getPointerOperand();
        APInt Total(DL.getIndexTypeSizeInBits(PtrPhi->getType()), 0);
        while (Ptr != PtrPhi) {
          auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
          if (!GEP) {
            SafeBody = false;
            break;
          }
          APInt Part(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
          if (!GEP->accumulateConstantOffset(DL, Part) ||
              Part.getBitWidth() != Total.getBitWidth()) {
            SafeBody = false;
            break;
          }
          Total += Part;
          Ptr = GEP->getPointerOperand();
        }
        if (!SafeBody || Total.getActiveBits() > 63) {
          SafeBody = false;
          break;
        }
        Stores.push_back({Store, Total.getSExtValue()});
        continue;
      }
      if (isa<PHINode>(&I) || isa<BranchInst>(&I) ||
          isa<GetElementPtrInst>(&I) || isa<ICmpInst>(&I) ||
          isa<CastInst>(&I))
        continue;
      auto *BO = dyn_cast<BinaryOperator>(&I);
      if (!BO || BO->getOpcode() != Instruction::Add) {
        SafeBody = false;
        break;
      }
    }
    if (!SafeBody || Stores.size() < 2 || !ElemBytes ||
        ByteStep % ElemBytes || Stores.size() != ByteStep / ElemBytes)
      continue;
    std::sort(Stores.begin(), Stores.end(),
              [](const auto &A, const auto &B) {
                return A.second < B.second;
              });
    bool LanesMatch = true;
    for (unsigned I = 0; I < Stores.size(); ++I)
      if (Stores[I].second != static_cast<int64_t>(I * ElemBytes))
        LanesMatch = false;
    if (!LanesMatch)
      continue;

    auto *StepUpdate =
        dyn_cast<BinaryOperator>(Loop.IV->getIncomingValueForBlock(&BB));
    if (!StepUpdate)
      continue;
    unsigned StepOperand = StepUpdate->getOperand(0) == Step ? 0 : 1;
    if (StepUpdate->getOperand(StepOperand) != Step)
      continue;
    for (unsigned I = 1; I < Stores.size(); ++I)
      Stores[I].first->eraseFromParent();
    StepUpdate->setOperand(
        StepOperand,
        ConstantInt::get(Step->getType(), ByteStep / Stores.size()));
    APInt NextOffset(DL.getIndexTypeSizeInBits(PtrNext->getType()),
                     ByteStep / Stores.size());
    PtrNext->setOperand(
        PtrNext->getNumOperands() - 1,
        ConstantInt::get(F.getContext(), NextOffset));
    Changed = true;
  }
  return Changed;
}

bool scalarizeCountedWideMemory(Function &F) {
  SmallVector<StoreInst *, 8> Stores;
  SmallVector<std::pair<LoadInst *, StoreInst *>, 8> Copies;
  for (BasicBlock &BB : F) {
    auto *Br = dyn_cast<BranchInst>(BB.getTerminator());
    ScalarLoopInfo Loop;
    bool CountedLoop = Br && Br->isConditional() &&
                       (Br->getSuccessor(0) == &BB ||
                        Br->getSuccessor(1) == &BB) &&
                       matchScalarLoop(&BB, Loop);
    for (Instruction &I : BB) {
      auto *Store = dyn_cast<StoreInst>(&I);
      if (!Store || Store->isVolatile() || Store->isAtomic())
        continue;
      if (isa<FixedVectorType>(Store->getValueOperand()->getType()) &&
          broadcastScalar(Store->getValueOperand())) {
        Stores.push_back(Store);
        continue;
      }
      if (!CountedLoop)
        continue;
      auto *Load = dyn_cast<LoadInst>(Store->getValueOperand());
      if (Load && Load->hasOneUse() &&
          Load->getType()->isIntegerTy(128) &&
          !Load->isVolatile() && !Load->isAtomic())
        Copies.push_back({Load, Store});
    }
  }
  bool Changed = false;
  for (StoreInst *Store : Stores) {
    auto *VT = cast<FixedVectorType>(Store->getValueOperand()->getType());
    Value *Scalar = broadcastScalar(Store->getValueOperand());
    Type *ElementTy = VT->getElementType();
    IRBuilder<> B(Store);
    for (unsigned I = 0; I < VT->getNumElements(); ++I) {
      Value *Ptr = Store->getPointerOperand();
      if (I) {
        Ptr = B.CreateGEP(ElementTy, Ptr, B.getInt32(I));
        copyVA(Store, cast<Instruction>(Ptr));
      }
      auto *ScalarStore = B.CreateStore(Scalar, Ptr);
      ScalarStore->setAlignment(Align(1));
      copyVA(Store, ScalarStore);
    }
    Value *Vector = Store->getValueOperand();
    auto *Shuffle = cast<Instruction>(Vector);
    auto *Insert = cast<Instruction>(Shuffle->getOperand(0));
    Store->eraseFromParent();
    if (Shuffle->use_empty())
      Shuffle->eraseFromParent();
    if (Insert->use_empty())
      Insert->eraseFromParent();
    Changed = true;
  }
  for (auto [Load, Store] : Copies) {
    Type *I64 = Type::getInt64Ty(F.getContext());
    SmallVector<Value *, 2> Values;
    IRBuilder<> LB(Load);
    for (unsigned I = 0; I != 2; ++I) {
      Value *Ptr = LB.CreateGEP(I64, Load->getPointerOperand(),
                                LB.getInt32(I));
      if (auto *GEP = dyn_cast<Instruction>(Ptr))
        copyVA(Load, GEP);
      auto *Lane = LB.CreateLoad(I64, Ptr);
      Lane->setAlignment(Align(1));
      copyVA(Load, Lane);
      Values.push_back(Lane);
    }
    IRBuilder<> SB(Store);
    for (unsigned I = 0; I != 2; ++I) {
      Value *Ptr = SB.CreateGEP(I64, Store->getPointerOperand(),
                                SB.getInt32(I));
      if (auto *GEP = dyn_cast<Instruction>(Ptr))
        copyVA(Store, GEP);
      auto *LaneStore = SB.CreateStore(Values[I], Ptr);
      LaneStore->setAlignment(Align(1));
      copyVA(Store, LaneStore);
    }
    Store->eraseFromParent();
    Load->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

bool eraseDeadValues(Function &F) {
  bool Changed = false;
  SmallVector<Instruction *, 16> Dead;
  do {
    Dead.clear();
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if ((isVectorTyped(I.getType()) || isa<GetElementPtrInst>(&I)) &&
            I.use_empty() && !I.mayHaveSideEffects() &&
            !I.getMetadata("revenant.opaque") &&
            !I.getMetadata("revenant.region"))
          Dead.push_back(&I);
    for (Instruction *I : Dead)
      I->eraseFromParent();
    Changed |= !Dead.empty();
  } while (!Dead.empty());
  return Changed;
}

bool runOnFunction(Function &F) {
  bool Changed = scalarizeCountedWideMemory(F);
  Changed |= lowerUniformStoreUnroll(F);
  SmallVector<Candidate, 4> Cands;
  // Collect-then-rewrite: matching never mutates.
  for (BasicBlock &BB : F) {
    if (!isa<BranchInst>(BB.getTerminator()))
      continue;
    auto *Br = cast<BranchInst>(BB.getTerminator());
    if (!Br->isConditional())
      continue;
    Candidate C;
    if (matchDispatch(&BB, F, C))
      Cands.push_back(std::move(C));
  }
  DenseSet<BasicBlock *> Taken;
  SmallVector<Candidate, 4> Work;
  for (Candidate &C : Cands) {
    bool Overlap = Taken.count(C.D) || Taken.count(C.S) || Taken.count(C.L);
    for (BasicBlock *BB : C.R)
      Overlap = Overlap || Taken.count(BB);
    if (Overlap)
      continue;
    Taken.insert(C.D);
    Taken.insert(C.S);
    Taken.insert(C.L);
    for (BasicBlock *BB : C.R)
      Taken.insert(BB);
    Work.push_back(std::move(C));
  }
  for (Candidate &C : Work) {
    // Pre-check: every kept outsider phi that uses an R def through
    // an R edge must keep a non-R incoming after the drop (the
    // loop-exit LCSSA merge keeps its L edge). Otherwise skip this
    // candidate without mutating anything.
    {
      bool OK = true;
      for (BasicBlock &BB : F) {
        if (C.R.count(&BB) || &BB == C.S)
          continue;
        for (PHINode &Phi : BB.phis()) {
          unsigned NonR = 0;
          for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
            if (!C.R.count(Phi.getIncomingBlock(I)))
              ++NonR;
          if (NonR == 0) {
            // Would drop every incoming: does this phi use an R def?
            for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
              if (C.R.count(Phi.getIncomingBlock(I)))
                OK = false;
          }
        }
      }
      if (!OK)
        continue;
    }
    // 1. Fold D to the scalar edge: replace the conditional branch
    // with an unconditional one at S. No kept instruction changes.
    auto *Br = cast<BranchInst>(C.D->getTerminator());
    BranchInst *NewBr = BranchInst::Create(C.S, Br);
    copyVA(Br, NewBr);
    // Preserve the debug location of the old terminator.
    Br->eraseFromParent();
    for (PHINode &Phi : C.S->phis()) {
      SmallVector<BasicBlock *, 4> Kill;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
        if (C.R.count(Phi.getIncomingBlock(I)))
          Kill.push_back(Phi.getIncomingBlock(I));
      for (BasicBlock *KB : Kill)
        Phi.removeIncomingValue(KB);
    }
    // Non-S outsider phis: drop R-block incomings, including the
    // middle->exit edge of the loop-exit LCSSA merge. The middle
    // block's own terminator is rewritten in step 2b so the CFG edge
    // disappears with it.
    for (BasicBlock &BB : F) {
      if (C.R.count(&BB) || &BB == C.S)
        continue;
      for (PHINode &Phi : BB.phis()) {
        SmallVector<BasicBlock *, 4> Kill;
        for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
          if (C.R.count(Phi.getIncomingBlock(I)))
            Kill.push_back(Phi.getIncomingBlock(I));
        // Only drop when a non-R incoming remains.
        if (Kill.size() == Phi.getNumIncomingValues())
          continue;
        for (BasicBlock *KB : Kill)
          Phi.removeIncomingValue(KB);
      }
    }
    // 2b. Strip !llvm.loop from every dead terminator first (the
    // metadata graph references blocks; erasing a referenced block
    // trips the verifier), then fold middle blocks that branch to a
    // kept outsider at their in-R/S successor.
    for (BasicBlock *BB : C.R)
      BB->getTerminator()->setMetadata(LLVMContext::MD_loop, nullptr);
    for (BasicBlock *BB : C.Chain)
      if (auto *MD = BB->getTerminator()->getMetadata(LLVMContext::MD_loop))
        (void)MD;
    for (BasicBlock *BB : C.R) {
      auto *T = dyn_cast<BranchInst>(BB->getTerminator());
      if (!T || !T->isConditional())
        continue;
      BasicBlock *K0 = T->getSuccessor(0);
      BasicBlock *K1 = T->getSuccessor(1);
      bool K0Out = !C.R.count(K0) && K0 != C.S && K0 != C.L;
      bool K1Out = !C.R.count(K1) && K1 != C.S && K1 != C.L;
      if (K0Out == K1Out)
        continue;
      // Only fold when the kept-outside edge carries no live values:
      // every phi in the outside successor fed through this edge
      // must have been dropped already (step 2) or be dead with R.
      // The loop-exit LCSSA merge does NOT qualify while it keeps
      // its middle edge, so it is never folded; the candidate then
      // needs the exit-phi path (which bails at match time).
      BasicBlock *Out = K0Out ? K0 : K1;
      bool HasLivePhi = false;
      for (PHINode &Phi : Out->phis()) {
        int Idx = Phi.getBasicBlockIndex(BB);
        if (Idx >= 0)
          HasLivePhi = true;
      }
      if (HasLivePhi)
        continue;
      BasicBlock *Keep = K0Out ? K1 : K0;
      BranchInst *Fold = BranchInst::Create(Keep, T);
      copyVA(T, Fold);
      Fold->setDebugLoc(T->getDebugLoc());
      T->eraseFromParent();
    }
    // Erase dead blocks, terminators first: replacing every dead
    // terminator with an unreachable drops successor block refs AND
    // the terminator's value uses (latch cmp, middle cmp.n) before
    // any value is destroyed, so the block-operand rule can never
    // trip. replaceSuccessorWith is avoided entirely: the
    // terminator swap updates succ/pred lists, then step 2's phi
    // surgery (already done) plus a leftover sweep below removes
    // dead pred edges from kept phis. Values follow: erase dead
    // blocks whose users are all dead-or-gone, preds likewise.
    SmallVector<BasicBlock *, 16> Dead;
    for (BasicBlock *BB : C.R)
      Dead.push_back(BB);
    for (BasicBlock *BB : C.Chain)
      Dead.push_back(BB);
    DenseSet<BasicBlock *> DeadSet(Dead.begin(), Dead.end());
    for (BasicBlock *BB : Dead) {
      Instruction *OldT = BB->getTerminator();
      auto *Unr = new UnreachableInst(BB->getContext());
      Unr->insertInto(BB, BB->end());
      OldT->eraseFromParent();
    }
    // Kept phis may still list dead preds (the terminator swap does
    // not touch phis): sweep all kept blocks once.
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
    // Erase dead blocks: drop all references inside the dead set
    // first so an erased def (n.vec) cannot outlive a dead user
    // (middle cmp.n, latch cmp) still sitting in another dead block.
    // No dead block has a kept predecessor (escape check) and no kept
    // block branches to a dead one (terminators swapped to
    // unreachable), so erase order inside the set is free.
    for (BasicBlock *BB : Dead)
      BB->dropAllReferences();
    for (BasicBlock *BB : Dead)
      BB->eraseFromParent();
    Changed = true;
  }
  if (Changed)
    Changed |= eraseDeadValues(F);
  return Changed;
}

} // namespace

PreservedAnalyses UnloopVecPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  bool Changed = runOnFunction(F);
  Changed |= unvecmaps::runUnvecMaps(F);
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-unloopvec` name in Pipeline/Register.cpp, so the entry point
// is compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
// Windows needs the explicit dllexport; without it opt fails with
// "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN
#undef DEBUG_TYPE // PassBuilder.h headers re-#define it per header
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

static void registerUnloopVecCallbacks(::llvm::PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](::llvm::StringRef Name, ::llvm::FunctionPassManager &PM,
         ::llvm::ArrayRef<::llvm::PassBuilder::PipelineElement>) {
        if (Name == "revenant-unloopvec") {
          PM.addPass(revenant::UnloopVecPass());
          return true;
        }
        return false;
      });
}

::llvm::PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnloopVec", LLVM_VERSION_STRING,
          registerUnloopVecCallbacks};
}

extern "C" __declspec(dllexport) ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
#endif
