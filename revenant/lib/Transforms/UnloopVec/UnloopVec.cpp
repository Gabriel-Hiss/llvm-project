//===- UnloopVec.cpp - undo loop-vectorizer skeletons ------------------===//
//
// Part of the Revenant LLVM fork.
//
// See UnloopVec.h for the firing shape and the soundness argument.
//
// Pass structure: scan for dispatch candidates D (cond br where one
// successor S has a single-block scalar self-loop latch and the other
// side reaches a wide-loop region R that feeds S's resume phi from its
// own block, or where both D successors reach such a region). Validate
// R and the scalar tail structurally, then:
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
//     same value as S-from-R-resume on every path that reaches it.
//   - middle.reduce erasure: combine + llvm.vector.reduce feeds only the
//     tail accumulator seed and the direct-exit value; both are dead
//     once the tail iterates from 0.
//   - memcheck erasure: pure ptrtoint/sub ICMP chain whose only true use
//     is routing into R; dead once D folds to S.
// Refinement-free because no kept instruction changes: kept blocks keep
// their instructions, operands, and flags; only branch targets and dead
// code change.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/UnloopVec/UnloopVec.h"

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
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace revenant {
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
bool matchScalarLoop(BasicBlock *H, ScalarLoopInfo &Out) {
  auto *Term = dyn_cast<BranchInst>(H->getTerminator());
  if (!Term || !Term->isConditional())
    return false;
  // needed; the caller validates the IV and the preds.
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
    Value *Other = nullptr;
    if (Add->getOperand(0) == &Phi &&
        (C = dyn_cast<ConstantInt>(Add->getOperand(1)))) {
      Other = Add->getOperand(0);
      (void)Other;
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
// that also steps by a constant (corpus copy_vec latches on its
// 0-based count while indexing with the main IV), so sibling
// constant-step phis count as IV chains too.
Value *loopBound(ScalarLoopInfo &L) {
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
Value *stripCountDerivation(Value *V, const Function &F, unsigned Depth = 8) {
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

// n.vec check: vector bound is N with low bits cleared (and N, Mask
// with K >= 1 low zero bits, e.g. the exact ~(step-1) mask or a
// wider-than-N mask like the i64 0x7FFFFFF8 clang emits for a
// zext trip count) or N minus (N mod K*VF). Returns the step K*VF
// when the shape matches, else 0. Only the low cleared bits matter:
// n.vec is a multiple of 2^K below N, so the erased vector loop ran
// whole steps and the tail restarted at 0 covers the full range.
uint64_t matchVecBound(Value *VecBound, Value *N, uint64_t &StepOut) {
  Value *RootN = stripCountDerivation(N, *cast<Instruction>(VecBound)->getParent()->getParent());
  if (auto *And = dyn_cast<BinaryOperator>(VecBound)) {
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
};

// Shared tail checks: S's own condition never depends on an S phi,
// and the single-block scalar self-loop L sits directly under S with
// preds exactly {S, L}.
bool matchTailShape(BasicBlock *S, BasicBlock *D, Candidate &C,
                    const DenseSet<BasicBlock *> &R, bool StrictPreds) {
  // S preds: D plus (in strict mode) at least one R block and no
  // other outsiders. Loose mode only needs the D edge; the strict
  // check runs after R is collected.
  bool HasD = false, HasR = false;
  for (BasicBlock *P : predecessors(S)) {
    if (P == D)
      HasD = true;
    else if (R.count(P))
      HasR = true;
    else if (StrictPreds)
      return false;
  }
  if (!HasD || (StrictPreds && !HasR))
    return false;
  // S phis: every non-D incoming must be fed from an R block (strict
  // only; loose mode skips phi checks entirely).
  if (StrictPreds) {
    for (PHINode &Phi : S->phis()) {
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
        BasicBlock *IB = Phi.getIncomingBlock(I);
        if (IB == D)
          continue;
        if (!R.count(IB))
          return false;
      }
    }
  }
  // S terminator condition must not transitively use an S phi (strict
  // only; tier dispatches between vector levels legitimately route on
  // N-derived conditions, and R membership decides later).
  if (StrictPreds) {
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
              return false;
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
  // with preds exactly {S, L}. In loose mode (R unknown) any
  // successor may be L; the strict pass re-validates after R is
  // collected.
  for (BasicBlock *Succ : successors(S)) {
    if (R.count(Succ))
      continue;
    ScalarLoopInfo L;
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
  return false;
}

// Loose: find L under S before R is known (D edge only).
bool matchScalarTailLoose(BasicBlock *S, BasicBlock *D, Candidate &C) {
  DenseSet<BasicBlock *> Empty;
  return matchTailShape(S, D, C, Empty, false);
}

// Strict: S entered from D and R only, phis agree.
bool matchScalarTail(BasicBlock *S, BasicBlock *D,
                     const DenseSet<BasicBlock *> &R, Candidate &C) {
  return matchTailShape(S, D, C, R, true);
}
// Top vector body: single-block self-loop, IV from constant 0,
// constant step K >= 2, exit == (IV.next == n.vec). n.vec must match
// the trip count N (masked or sub/urem form) and the same n.vec value
// must feed S's resume phi from an R block.
bool matchVectorBody(BasicBlock *B, Candidate &C, Function &F) {
  ScalarLoopInfo V;
  if (!matchScalarLoop(B, V))
    return false;
  // IV start must be constant 0 from the non-self predecessor.
  for (unsigned I = 0; I < V.IV->getNumIncomingValues(); ++I) {
    if (V.IV->getIncomingBlock(I) == B)
      continue;
    auto *C0 = dyn_cast<ConstantInt>(V.IV->getIncomingValue(I));
    if (!C0 || !C0->isZero())
      return false;
  }
  uint64_t Step = V.Step->getZExtValue();
  if (Step < 2)
    return false;
  Value *Bound = loopBound(V);
  if (!Bound)
    return false;
  // Bound must be pure w.r.t. R.
  DenseSet<const Value *> Seen;
  if (!isPureChain(Bound, C.R, Seen))
    return false;
  // Tail bound must derive from the same N.
  Value *TailBound = loopBound(C.TailLoop);
  if (!TailBound)
    return false;
  Value *NTail = stripCountDerivation(TailBound, F);
  uint64_t Found = 0;
  // Try bound directly, else through a pure chain root.
  if (!matchVecBound(Bound, NTail, Found)) {
    // Bound may be (N mod form) chain; try the operands.
    return false;
  }
  // Interleaved groups: the latch step and the mask step must divide
  // each other (the mask step covers one group, the latch strides all
  // groups); anything else is not a vectorizer bound.
  if (Found != Step && Step % Found != 0 && Found % Step != 0)
    return false;
  // Same n.vec value must feed S's resume phi from an R block: the
  // resume phi's R-incoming value is Bound (or its IV.next source).
  bool ResumeOK = false;
  Value *IVNext = nullptr;
  for (unsigned I = 0; I < V.IV->getNumIncomingValues(); ++I)
    if (V.IV->getIncomingBlock(I) == B)
      IVNext = V.IV->getIncomingValue(I);
  for (PHINode &Phi : C.S->phis()) {
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
      BasicBlock *IB = Phi.getIncomingBlock(I);
      if (!C.R.count(IB))
        continue;
      Value *Inc = Phi.getIncomingValue(I);
      if (Inc == Bound || Inc == IVNext) {
        // The IV phi supplying the resume must start at constant 0
        // on the D edge.
        if (auto *RPhi = dyn_cast<PHINode>(&Phi)) {
          for (unsigned J = 0; J < RPhi->getNumIncomingValues(); ++J) {
            if (RPhi->getIncomingBlock(J) == C.D) {
              if (auto *CZ = dyn_cast<ConstantInt>(RPhi->getIncomingValue(J))) {
                if (CZ->isZero())
                  ResumeOK = true;
              }
            }
          }
        }
      }
    }
  }
  if (!ResumeOK)
    return false;
  return true;
}

// Full candidate match for one dispatch block D: S is the successor
// with a scalar self-loop latch directly under it (preds {S, L});
// R is everything reachable from the other successor without passing
// through S, L, or L's exit.
#ifndef NDEBUG
#define ULV_BAIL()                                                           \
  do {                                                                       \
    ::llvm::errs() << "unloopvec bail " << __LINE__ << " in "                \
                   << F.getName() << "\n";                                   \
  } while (0)
#else
#define ULV_BAIL() ((void)0)
#endif
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
    // Find the scalar tail loop L directly under S first, with no
    // region yet (empty R: the pred check below is loose, the real
    // check happens after R is collected).
    DenseSet<BasicBlock *> Empty;
    if (!matchScalarTailLoose(S, D, Cand))
      continue;
    BasicBlock *L = Cand.L;
    DenseSet<BasicBlock *> Stop{S, L};
    for (BasicBlock *ES : successors(L))
      if (ES != L)
        Stop.insert(ES);
    DenseSet<BasicBlock *> R;
    if (!collectRegion(V, Stop, R, F))
      continue;
    if (R.size() < 2)
      continue;
    Cand.R = R;
    Cand.L = nullptr;
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
    if (!matchScalarTail(S, D, Cand.R, Cand))
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
    // fails; self-loops must be vector-typed/counted loops.
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
    // Every self-loop in R must be vector-typed or a counted loop
    // from constant 0 (scalarized tiers are fine).
    for (BasicBlock *BB : Cand.R) {
      bool Self = false;
      for (BasicBlock *Su : successors(BB))
        if (Su == BB)
          Self = true;
      if (!Self)
        continue;
      bool HasVec = false;
      for (Instruction &I : *BB) {
        if (isa<PHINode>(&I))
          continue;
        if (isVectorTyped(I.getType())) {
          HasVec = true;
          break;
        }
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (const Function *CF = CB->getCalledFunction()) {
            StringRef N = CF->getName();
            if (N.starts_with("llvm.vector.reduce.") ||
                N.starts_with("llvm.masked.") || N.starts_with("llvm.vp.")) {
              HasVec = true;
              break;
            }
          }
        }
      }
      if (HasVec)
        continue;
      // Counted loop from constant 0: every phi's non-self incoming
      // is constant 0 or defined outside R.
      bool Counted = true;
      for (PHINode &Phi : BB->phis()) {
        for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
          if (Phi.getIncomingBlock(I) == BB)
            continue;
          Value *Inc = Phi.getIncomingValue(I);
          if (auto *CZ = dyn_cast<ConstantInt>(Inc)) {
            if (!CZ->isZero())
              Counted = false;
          } else if (auto *I2 = dyn_cast<Instruction>(Inc)) {
            if (Cand.R.count(I2->getParent()))
              Counted = false;
          }
        }
      }
      // Also require a constant-step IV.
      ScalarLoopInfo Tmp;
      if (!matchScalarLoop(BB, Tmp)) {
        ULV_BAIL();
        goto next_pick;
      }
      if (!Counted) {
        ULV_BAIL();
        goto next_pick;
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
        if (!fpReassocOK(I)) {
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
    // other tail phis start at S phis or outside-R values.
    {
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
        if (auto *P = dyn_cast<PHINode>(Inc)) {
          if (P->getParent() != S) {
            ULV_BAIL();
            goto next_pick;
          }
          bool Zero = false;
          for (unsigned J = 0; J < P->getNumIncomingValues(); ++J)
            if (P->getIncomingBlock(J) == D) {
              if (auto *CZ = dyn_cast<ConstantInt>(P->getIncomingValue(J)))
                if (CZ->isZero())
                  Zero = true;
            }
          if (Zero)
            IVOK = true;
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
    // Top vector body check.
    {
      bool Found = false;
      // The top body is an R block whose IV starts at constant 0
      // from D or the chain.
      for (BasicBlock *BB : Cand.R) {
        Candidate Probe = Cand;
        if (matchVectorBody(BB, Probe, F)) {
          Cand = Probe;
          Found = true;
          break;
        }
      }
      if (!Found) {
        ULV_BAIL();
        goto next_pick;
      }
    }
    Cand.Chain = Chain;
    C = Cand;
    return true;
  next_pick:;
  }
  return false;
}

bool runOnFunction(Function &F) {
  bool Changed = false;
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
  // Drop overlapping candidates (shared R blocks): keep the first.
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
    NewBr->setDebugLoc(Br->getDebugLoc());
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
  return Changed;
}

} // namespace

PreservedAnalyses UnloopVecPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  if (!runOnFunction(F))
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
