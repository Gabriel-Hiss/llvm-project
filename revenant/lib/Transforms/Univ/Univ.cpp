//===- Univ.cpp - undo IV rewriting and strength reduction -----------===//
//
// Part of the Revenant LLVM fork.
//
// Undoes `indvars` IV widening plus `loop-reduce` strength reduction on the
// two loop shapes the corpus actually shows:
//
// 1. Widened integer-IV scalar tails: the loop walks an `i64` IV PHI from 0
//    stepping by 1, exits on `icmp eq %next, %wide.trip.count`, and reads
//    one element per iteration through a typed `getelementptr <elem>` (the
//    O2 `sum_vec` scalar tail after the vector main loop, `fill_vec`/`add`
//    tails, `sum_vec_d` tails; c-clang-o2/o3 ir-post).
// 2. Pointer-increment byte loops: the loop threads one (copy: two) `ptr`
//    PHI stepped by `getelementptr i8, %p, 1` and counts iterations with a
//    kept `i32` counter PHI tested by `icmp eq %count, %trip` (O2/O3
//    `copy_bytes` scalar tail and 8x-unrolled remainder; ir-pre shows the
//    same pointers next to an `i32` index tested with `icmp slt i32 %i,
//    %n`, which is the shape we rebuild).
//
// Both rewrite to the ir-pre shape: a single `i32` index PHI from 0,
// `add i32 %i, 1` in the latch, `icmp slt i32 %i, %n` in the header, one
// `getelementptr <elem>, %base, i32 %i` per touched element, and the pointer
// PHIs/counters replaced by the equivalent address expression. rustc
// iterator loops (`for &x in a`, filter/map folds) do not survive as loops
// in `rust-o3` ir-post: small cases fold to constants or a single vector
// reduction, so there is no IV left to invert there; the pointer half below
// covers the shape they would leave if present (`icmp eq %ptr, %end` +
// 1-element GEP step), and the measurement section reports that finding.
//
// Corpus:
// - `c-clang-o2` `sum_vec` (ir-pre `icmp slt i32` + `sext` + `getelementptr
//   i32`; ir-post scalar tail `phi i64` + `icmp eq %36, %5` + typed GEP).
// - `c-clang-o2`/`c-clang-o3` `copy_bytes` scalar tail (`phi ptr` pair +
//   `i32` counter + `icmp eq %46, %35`) and 8x remainder (`phi i32` +
//   `phi ptr` pair, exit `icmp eq %89, %2`).
//
// Safety: single-IV loops only. The IV PHI must start at constant 0, step
// by constant 1, and every use must be the step, the exit compare, or one of
// the recognized addresses. The new exit `icmp slt %i, %n` computes the
// same trip count (0..n-1) when the bound `%n` dominates and the old bound
// provably equals its widening. `!revenant.va` moves to each rebuilt
// instruction; `!revenant.opaque` is copied over, never dropped.
//
// Alive2-style argument for the integer half: with start 0 and step 1 the
// old exit fires at the first `%iv == %wide`. When `%wide == zext %n` the
// new exit fires at the first `%i >= %n` (signed), the same iteration for
// `%n >= 0`; for `%n <= 0` both trip zero times because the preheader guard
// (`icmp sgt %n, 0`) skips the loop. The rebuilt GEP `base + %i * sizeof`
// addresses the same bytes as the old typed GEP `base + %iv * sizeof` at
// equal iteration values, and both PHIs step together so the values stay
// equal.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Univ/Univ.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace {

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (!Src || !Dst)
    return;
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
  if (MDNode *MD = Src->getMetadata("revenant.opaque"))
    Dst->setMetadata("revenant.opaque", MD);
}

// Find the latch of Header: a distinct predecessor branching back, or the
// header itself for a single-block loop (self edge). The latch must be
// reachable from the header (a true back edge); plain entries are not.
static bool reachable(BasicBlock *From, BasicBlock *To) {
  SmallPtrSet<BasicBlock *, 16> Seen;
  SmallVector<BasicBlock *, 16> Stack{From};
  while (!Stack.empty()) {
    BasicBlock *B = Stack.pop_back_val();
    if (B == To)
      return true;
    if (!Seen.insert(B).second)
      continue;
    auto *T = dyn_cast<BranchInst>(B->getTerminator());
    if (!T)
      continue;
    for (BasicBlock *S : T->successors())
      Stack.push_back(S);
  }
  return false;
}

static BasicBlock *findLatch(BasicBlock *Header) {
  BasicBlock *Latch = nullptr;
  for (BasicBlock *P : predecessors(Header)) {
    auto *T = dyn_cast<BranchInst>(P->getTerminator());
    if (!T)
      continue;
    bool Back = false;
    for (BasicBlock *S : T->successors())
      if (S == Header)
        Back = true;
    if (!Back)
      continue;
    if (P == Header) {
      if (Latch)
        return nullptr;
      Latch = P;
      continue;
    }
    // A plain entry also branches to the header; only a block reachable
    // from the header is a true back edge.
    if (!reachable(Header, P))
      continue;
    if (Latch)
      return nullptr;
    Latch = P;
  }
  return Latch;
}

// A value defined outside the loop (or not an instruction at all).
static bool isLoopInvariant(Value *V, BasicBlock *Header, BasicBlock *Latch) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return true;
  return I->getParent() != Header && I->getParent() != Latch;
}

// Integer PHI in Header stepping by constant StepC in Latch. Returns the
// step instruction, which must live in Latch and consume the PHI, plus
// the loop-entry start value in StartV. The start must be constant 0 or a
// value defined outside the loop (vector-tail remainders start at the
// main loop's end count).
static Instruction *matchStep(PHINode *Phi, BasicBlock *Header,
                              BasicBlock *Latch, uint64_t &StepC,
                              Value *&StartV) {
  if (Phi->getNumIncomingValues() != 2)
    return nullptr;
  Value *Back = nullptr;
  Value *Start = nullptr;
  for (unsigned i = 0, e = Phi->getNumIncomingValues(); i != e; ++i) {
    if (Phi->getIncomingBlock(i) == Latch) {
      if (Back)
        return nullptr;
      Back = Phi->getIncomingValue(i);
    } else {
      if (Start)
        return nullptr;
      Start = Phi->getIncomingValue(i);
    }
  }
  if (!Back || !Start)
    return nullptr;
  // Start: constant 0, or loop-invariant (defined outside header/latch).
  if (auto *C = dyn_cast<ConstantInt>(Start)) {
    if (!C->isZero())
      return nullptr;
  } else if (!isLoopInvariant(Start, Header, Latch)) {
    return nullptr;
  }
  auto *Step = Back ? dyn_cast<Instruction>(Back) : nullptr;
  if (!Step || Step->getParent() != Latch)
    return nullptr;
  if (Step->getOpcode() != Instruction::Add)
    return nullptr;
  Value *Other = Step->getOperand(0) == Phi   ? Step->getOperand(1)
                 : Step->getOperand(1) == Phi ? Step->getOperand(0)
                                              : nullptr;
  auto *C = Other ? dyn_cast<ConstantInt>(Other) : nullptr;
  if (!C)
    return nullptr;
  StepC = C->getZExtValue();
  StartV = Start;
  return Step;
}

// Peel one `zext`/`sext` to the narrow bound value.
static Value *peelWidening(Value *V) {
  if (auto *Ext = dyn_cast<ZExtInst>(V))
    return Ext->getOperand(0);
  if (auto *Ext = dyn_cast<SExtInst>(V))
    return Ext->getOperand(0);
  return nullptr;
}
// One element touch inside the loop: the memory op and the pointer it
// touches (a GEP to rebuild, or a PHI stepped by a 1-element GEP).
struct Touch {
  Instruction *MemOp = nullptr; // load or store
  GetElementPtrInst *GEP = nullptr;
  PHINode *PtrPhi = nullptr;
};

// The recognized loop: header/latch, the IV (integer PHI or ptr PHI), its
// step, the exit compare + bound, and every memory touch to rebuild.
struct LoopShape {
  BasicBlock *Header = nullptr;
  BasicBlock *Latch = nullptr;
  PHINode *IV = nullptr;      // integer IV, or the counter of a ptr loop
  Instruction *Step = nullptr; // add (integer) or 1-elem GEP (pointer)
  Value *Bound = nullptr; // narrow `%n` (integer) or trip `%rem` (ptr)
  ICmpInst *ExitCmp = nullptr;
  Value *IVStart = nullptr; // entry value of the integer IV (0 or invariant)
  bool IsPtr = false;
  SmallVector<PHINode *, 2> Ptrs;
  SmallVector<GetElementPtrInst *, 2> PtrSteps;
  SmallVector<Touch, 4> Touches;
  Type *ElemTy = nullptr;
};

// Integer half: `phi i64 [0, pre], [%step, latch]`, `%step = add %iv, 1`,
// `icmp eq %step-or-iv, %wide` where `%wide` peels to an `i32` bound, one
// typed GEP per memory address, no other IV uses.
static bool matchIntLoop(BasicBlock *Header, BasicBlock *Latch, LoopShape &S) {
  S = LoopShape();
  PHINode *IV = nullptr;
  Instruction *Step = nullptr;
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isIntegerTy())
      continue;
    uint64_t StepC = 0;
    Value *StartV = nullptr;
    Instruction *St = matchStep(PN, Header, Latch, StepC, StartV);
    if (!St || StepC != 1)
      continue;
    if (IV)
      return false;
    IV = PN;
    Step = St;
    S.IVStart = StartV;
  }
  if (!IV)
    return false;
  // Other integer PHIs must be accumulators (no step in the latch), not
  // coupled IVs.
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (PN != IV && PN->getType()->isIntegerTy()) {
      uint64_t StepC = 0;
      Value *Dummy = nullptr;
      if (matchStep(PN, Header, Latch, StepC, Dummy))
        return false;
    }
  }
  // No pointer PHIs either: those are the pointer half's loops.
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (PN->getType()->isPointerTy())
      return false;
  }

  // Exit compare in the header or latch: `ne`/`eq` on the IV or its step.
  // A single-block loop has Header == Latch; scan once so the exit
  // compare is not seen twice. Multi-block loops test in the latch (the
  // header holds only addresses); body-block compares belong to a
  // different loop shape (early exit) and stay put.
  ICmpInst *Cmp = nullptr;
  Value *Wide = nullptr;
  auto inLoop = [&](BasicBlock *BB) {
    return BB == Header || BB == Latch ||
           (reachable(Header, BB) && reachable(BB, Latch));
  };
  SmallVector<BasicBlock *, 2> CmpBlocks{Header};
  if (Latch != Header)
    CmpBlocks.push_back(Latch);
  for (BasicBlock *BB : CmpBlocks) {
    for (Instruction &I : *BB) {
      auto *C = dyn_cast<ICmpInst>(&I);
      if (!C || (C->getPredicate() != ICmpInst::ICMP_EQ &&
                 C->getPredicate() != ICmpInst::ICMP_NE))
        continue;
      Value *A = C->getOperand(0), *B = C->getOperand(1);
      bool AIs = A == IV || A == Step;
      bool BIs = B == IV || B == Step;
      if (AIs == BIs)
        continue;
      if (Cmp)
        return false;
      Cmp = C;
      Wide = AIs ? B : A;
    }
  }
  if (!Cmp || !Wide || !isLoopInvariant(Wide, Header, Latch))
    return false;
  Value *Narrow = peelWidening(Wide);
  // counts 0..n-1 under the preheader's `n > 0` guard. (dyn_cast is not
  // null-safe: guard Narrow first.)
  if (Narrow) {
    if (auto *II = dyn_cast<IntrinsicInst>(Narrow)) {
      if (II->getIntrinsicID() == Intrinsic::smax &&
          isa<ConstantInt>(II->getOperand(1)) &&
          cast<ConstantInt>(II->getOperand(1))->isZero())
        Narrow = II->getOperand(0);
    }
  }
  if (!Narrow || !Narrow->getType()->isIntegerTy(32) ||
      !isLoopInvariant(Narrow, Header, Latch))
    return false;

  // The compare's exit edge must leave the loop: `ne` continues on true,
  // `eq` on false. Folded tails test in the loop block itself; split loops
  // test in the latch and continue into the header. The continue edge must
  // reach the latch; the exit edge must not be the header or the latch.
  auto *Br = dyn_cast<BranchInst>(Cmp->getParent()->getTerminator());
  if (!Br || !Br->isConditional() || Br->getCondition() != Cmp)
    return false;
  if (Cmp->getParent() != Header && Cmp->getParent() != Latch)
    return false;
  bool ContTrue = Cmp->getPredicate() == ICmpInst::ICMP_NE;
  BasicBlock *Cont = ContTrue ? Br->getSuccessor(0) : Br->getSuccessor(1);
  BasicBlock *Exit = ContTrue ? Br->getSuccessor(1) : Br->getSuccessor(0);
  if (Exit == Header || Exit == Latch)
    return false;
  if (Cont != Header && Cont != Latch && !reachable(Cont, Latch))
    return false;
  // Use discipline: IV feeds only step/compare/typed-GEP index; the step
  // feeds only the PHI and the compare. Addresses may live in the header,
  // the latch, or a body block between them.
  SmallVector<GetElementPtrInst *, 4> GEPs;
  for (User *U : IV->users()) {
    Instruction *I = dyn_cast<Instruction>(U);
    if (!I)
      return false;
    if (I == Step || I == Cmp)
      continue;
    auto *GI = dyn_cast<GetElementPtrInst>(I);
    if (!GI || !inLoop(GI->getParent()))
      return false;
    if (GI->getNumIndices() != 1 || GI->getOperand(1) != IV)
      return false;
    if (GI->getSourceElementType()->isIntegerTy(8))
      return false; // unscaled byte GEP: unknown stride
    // Lifting may materialize an invariant base pointer as an inttoptr in
    // the loop header. Its integer source must still be loop-invariant.
    Value *Base = GI->getPointerOperand();
    auto *BaseCast = dyn_cast<IntToPtrInst>(Base);
    if (!isLoopInvariant(Base, Header, Latch) &&
        (!BaseCast ||
         !isLoopInvariant(BaseCast->getOperand(0), Header, Latch)))
      return false;
    GEPs.push_back(GI);
  }
  for (User *U : Step->users()) {
    if (U == IV || U == Cmp)
      continue;
    return false;
  }
  if (GEPs.empty())
    return false;
  // One base, one element type across all touches.
  Value *Base = GEPs[0]->getPointerOperand();
  Type *ElemTy = GEPs[0]->getSourceElementType();
  for (auto *GI : GEPs) {
    if (GI->getPointerOperand() != Base ||
        GI->getSourceElementType() != ElemTy)
      return false;
    // Address users must be loads/stores in the loop.
    for (User *U : GI->users()) {
      auto *MI = dyn_cast<Instruction>(U);
      if (!MI || (!isa<LoadInst>(MI) && !isa<StoreInst>(MI)))
        return false;
      if (!inLoop(MI->getParent()))
        return false;
    }
  }

  S.Header = Header;
  S.Latch = Latch;
  S.IV = IV;
  S.Step = Step;
  S.ExitCmp = Cmp;
  S.Bound = Narrow;
  S.IsPtr = false;
  S.ElemTy = ElemTy;
  for (auto *GI : GEPs)
    S.Touches.push_back({nullptr, GI, nullptr});
  return true;
}
// Pointer half: one or two `ptr` PHIs stepped by a 1-element GEP in the
// latch, plus an `i32` counter PHI from 0 stepping by 1, exited by
// `icmp eq %counter-step, %trip`. Loads/stores touch the ptr PHI directly
// (byte loops) or through constant-offset GEPs only when the GEP is the
// step itself; unrolled remainders with extra constant GEPs do not match.
static bool matchPtrLoop(BasicBlock *Header, BasicBlock *Latch, LoopShape &S) {
  S = LoopShape();
  SmallVector<PHINode *, 2> Ptrs;
  SmallVector<GetElementPtrInst *, 2> Steps;
  Value *Start0 = nullptr;
  Type *ElemTy = nullptr;
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isPointerTy())
      continue;
    Value *Back = nullptr;
    Value *Start = nullptr;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      if (PN->getIncomingBlock(i) == Latch) {
        if (Back)
          return false;
        Back = PN->getIncomingValue(i);
      } else {
        Start = PN->getIncomingValue(i);
      }
    }
    auto *Step = Back ? dyn_cast<GetElementPtrInst>(Back) : nullptr;
    if (!Step || Step->getParent() != Latch)
      return false;
    if (Step->getPointerOperand() != PN || Step->getNumIndices() != 1)
      return false;
    auto *C = dyn_cast<ConstantInt>(Step->getOperand(1));
    if (!C || !C->isOne())
      return false;
    if (!isLoopInvariant(Start, Header, Latch))
      return false;
    if (Ptrs.size() == 2)
      return false;
    Ptrs.push_back(PN);
    Steps.push_back(Step);
    if (!Start0)
      Start0 = Start;
    if (!ElemTy)
      ElemTy = Step->getSourceElementType();
    else if (ElemTy != Step->getSourceElementType())
      return false;
  }
  if (Ptrs.empty())
    return false;

  // The counter: an i32 PHI from 0 stepping by 1 in the latch.
  PHINode *Counter = nullptr;
  Instruction *CountStep = nullptr;
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isIntegerTy(32))
      continue;
    uint64_t StepC = 0;
    Value *StartV = nullptr;
    Instruction *St = matchStep(PN, Header, Latch, StepC, StartV);
    if (!St || StepC != 1)
      continue;
    if (Counter)
      return false;
    Counter = PN;
    CountStep = St;
  }
  if (!Counter)
    return false;
  // No other integer PHIs.
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (PN->getType()->isIntegerTy() && PN != Counter)
      return false;
  }

  // Exit: `icmp eq %count-step, %trip`, false edge leaves the loop. A
  // single-block loop has Header == Latch; scan once so the exit compare
  // is not seen twice.
  ICmpInst *Cmp = nullptr;
  Value *Trip = nullptr;
  SmallVector<BasicBlock *, 2> CmpBlocks{Header};
  if (Latch != Header)
    CmpBlocks.push_back(Latch);
  for (BasicBlock *BB : CmpBlocks) {
    for (Instruction &I : *BB) {
      auto *C = dyn_cast<ICmpInst>(&I);
      if (!C || C->getPredicate() != ICmpInst::ICMP_EQ)
        continue;
      Value *A = C->getOperand(0), *B = C->getOperand(1);
      bool AIs = A == Counter || A == CountStep;
      bool BIs = B == Counter || B == CountStep;
      if (AIs == BIs)
        continue;
      if (Cmp)
        return false;
      Cmp = C;
      Trip = AIs ? B : A;
    }
  }
  if (!Cmp || !isLoopInvariant(Trip, Header, Latch))
    return false;
  auto *Br = dyn_cast<BranchInst>(Cmp->getParent()->getTerminator());
  if (!Br || !Br->isConditional() || Br->getCondition() != Cmp)
    return false;
  if (Br->getSuccessor(0) == Header || Br->getSuccessor(0) == Latch)
    return false;

  // Pointer uses: the step, plus loads/stores directly on the PHI. Any
  // extra constant-offset GEP (unrolled remainder) bails.
  SmallVector<Touch, 4> Touches;
  for (unsigned p = 0; p < Ptrs.size(); ++p) {
    for (User *U : Ptrs[p]->users()) {
      Instruction *I = dyn_cast<Instruction>(U);
      if (!I)
        return false;
      if (I == Steps[p] || I == Cmp)
        continue;
      if ((isa<LoadInst>(I) || isa<StoreInst>(I)) &&
          (I->getParent() == Header || I->getParent() == Latch)) {
        Touches.push_back({I, nullptr, Ptrs[p]});
        continue;
      }
      return false;
    }
    for (User *U : Steps[p]->users()) {
      if (U == Ptrs[p])
        continue;
      return false;
    }
  }
  // The counter feeds only its step and the compare.
  for (User *U : Counter->users()) {
    if (U == CountStep || U == Cmp)
      continue;
    return false;
  }
  if (Touches.empty())
    return false;

  S.Header = Header;
  S.Latch = Latch;
  S.IV = Counter;
  S.Step = CountStep;
  S.ExitCmp = Cmp;
  S.Bound = Trip;
  S.IsPtr = true;
  S.Ptrs = std::move(Ptrs);
  S.PtrSteps = std::move(Steps);
  S.Touches = std::move(Touches);
  S.ElemTy = ElemTy;
  return true;
}

// Copy metadata from Old to New, then move it: the old instruction dies.
static void moveVA(Instruction *Old, Instruction *New) {
  copyVA(Old, New);
}

// Rewrite an integer-IV loop: new i32 index, slt exit, typed GEPs.
static bool rewriteIntLoop(LoopShape &S) {
  LLVMContext &Ctx = S.Header->getContext();
  Type *I32 = Type::getInt32Ty(Ctx);

  BasicBlock *Pre = nullptr;
  for (BasicBlock *P : predecessors(S.Header)) {
    if (P == S.Latch || P == S.Header)
      continue;
    if (Pre)
      return false;
    Pre = P;
  }
  if (!Pre)
    return false;

  IRBuilder<> HB(&S.Header->front());
  PHINode *NewIV = HB.CreatePHI(I32, 2, S.IV->getName() + ".idx");
  moveVA(S.IV, NewIV);
  // Start the new index at the narrowed entry value: constant 0, or
  // `trunc` of a wider invariant start (vector-tail remainders).
  Value *NewStart = nullptr;
  if (!S.IVStart)
    return false;
  if (auto *C = dyn_cast<ConstantInt>(S.IVStart)) {
    NewStart = ConstantInt::get(I32, 0);
  } else if (S.IVStart->getType() == I32) {
    NewStart = S.IVStart;
  } else {
    IRBuilder<> PB(Pre->getTerminator());
    NewStart = PB.CreateTrunc(S.IVStart, I32, S.IV->getName() + ".start");
    moveVA(S.IV, cast<Instruction>(NewStart));
  }
  NewIV->addIncoming(NewStart, Pre);
  // Insert the new step before the old step: same block, and it dominates
  // the latch compare plus every later use. (Inserting after the old step
  // puts it between the step and its compare, which breaks dominance of
  // the new PHI incoming when the step feeds the compare in the same
  IRBuilder<> LB(S.Step);
  Value *NewStep =
      LB.CreateAdd(NewIV, ConstantInt::get(I32, 1), S.Step->getName() + ".idx");
  moveVA(S.Step, cast<Instruction>(NewStep));
  NewIV->addIncoming(NewStep, S.Latch);
  if (!S.ExitCmp)
    return false;
  // Exit compare on the same operand the old one tested (PHI or step).
  Value *Tested = S.ExitCmp->getOperand(0) == S.Step ||
                          S.ExitCmp->getOperand(1) == S.Step
                      ? NewStep
                      : static_cast<Value *>(NewIV);
  ICmpInst::Predicate OldPredicate = S.ExitCmp->getPredicate();
  IRBuilder<> CB(S.ExitCmp);
  Value *NewCmp =
      CB.CreateICmpSLT(Tested, S.Bound, S.ExitCmp->getName() + ".idx");
  moveVA(S.ExitCmp, cast<Instruction>(NewCmp));
  auto *Br = cast<BranchInst>(CB.GetInsertBlock()->getTerminator());
  Br->setCondition(cast<Value>(NewCmp));
  // Old `eq` continued on false; `ne` continued on true. `slt` continues true.
  if (OldPredicate == ICmpInst::ICMP_EQ)
    Br->swapSuccessors();
  S.ExitCmp->eraseFromParent();
  // Rebuild addresses. The old GEP base is loop-invariant and stays.
  for (auto &T : S.Touches) {
    GetElementPtrInst *Old = T.GEP;
    IRBuilder<> GB(Old);
    Value *NewGEP = GB.CreateInBoundsGEP(S.ElemTy, Old->getPointerOperand(),
                                         NewIV, Old->getName() + ".idx");
    moveVA(Old, cast<Instruction>(NewGEP));
    Old->replaceAllUsesWith(NewGEP);
    Old->eraseFromParent();
  }
  // Drop the old IV machinery. The matcher allows only step/compare/GEP
  // uses, all rewritten or detached by now, so both die user-free.
  // (No replaceAllUsesWith: it asserts on the deliberate i64 -> i32 type
  // change even when no users remain.)
  for (unsigned i = 0, e = S.IV->getNumIncomingValues(); i != e; ++i)
    if (S.IV->getIncomingBlock(i) == S.Latch)
      S.IV->setIncomingValue(i, UndefValue::get(S.IV->getType()));
  S.Step->eraseFromParent();
  S.IV->eraseFromParent();
  return true;
}

// Rewrite a pointer loop: new i32 index from 0, `slt` exit against the
// trip bound when it is the source `%n` (i32), typed GEPs replacing the
// pointer PHIs. Trip bounds of the form `%n - %done` (vector remainders)
// do not match: bail.
static bool rewritePtrLoop(LoopShape &S) {
  if (!S.Bound->getType()->isIntegerTy(32))
    return false;
  LLVMContext &Ctx = S.Header->getContext();
  Type *I32 = Type::getInt32Ty(Ctx);
  BasicBlock *Pre = nullptr;
  for (BasicBlock *P : predecessors(S.Header)) {
    if (P == S.Latch || P == S.Header)
      continue;
    if (Pre)
      return false;
    Pre = P;
  }
  if (!Pre)
    return false;

  IRBuilder<> HB(&S.Header->front());
  PHINode *NewIV = HB.CreatePHI(I32, 2, S.IV->getName() + ".idx");
  moveVA(S.IV, NewIV);
  NewIV->addIncoming(ConstantInt::get(I32, 0), Pre);
  // Same as the integer half: the new step goes before the old counter
  // step so it dominates every later use.
  IRBuilder<> LB(S.Step);
  Value *NewStep =
      LB.CreateAdd(NewIV, ConstantInt::get(I32, 1), S.Step->getName() + ".idx");
  moveVA(S.Step, cast<Instruction>(NewStep));
  NewIV->addIncoming(NewStep, S.Latch);
  Value *Tested = S.ExitCmp->getOperand(0) == S.Step ||
                          S.ExitCmp->getOperand(1) == S.Step
                      ? NewStep
                      : static_cast<Value *>(NewIV);
  IRBuilder<> CB(S.ExitCmp);
  Value *NewCmp =
      CB.CreateICmpSLT(Tested, S.Bound, S.ExitCmp->getName() + ".idx");
  moveVA(S.ExitCmp, cast<Instruction>(NewCmp));
  auto *PBr = cast<BranchInst>(CB.GetInsertBlock()->getTerminator());
  PBr->setCondition(cast<Value>(NewCmp));
  PBr->swapSuccessors();
  S.ExitCmp->eraseFromParent();

  // Each touch gets `getelementptr <elem>, start, %i`; the pointer PHI and
  // its step die. The index reaching a touch in the latch must be the
  // backedge value; touches in the header use the PHI. Both blocks see the
  // same NewIV PHI, whose incoming values already select correctly, so one
  // GEP per touch in front of the memory op works for both.
  for (unsigned p = 0; p < S.Ptrs.size(); ++p) {
    PHINode *Phi = S.Ptrs[p];
    // Start pointer: the non-latch incoming.
    Value *Start = nullptr;
    for (unsigned i = 0, e = Phi->getNumIncomingValues(); i != e; ++i)
      if (Phi->getIncomingBlock(i) != S.Latch)
        Start = Phi->getIncomingValue(i);
    for (auto &T : S.Touches) {
      if (T.PtrPhi != Phi)
        continue;
      IRBuilder<> GB(T.MemOp);
      Value *NewGEP = GB.CreateInBoundsGEP(S.ElemTy, Start, NewIV,
                                           Phi->getName() + ".idx");
      moveVA(Phi, cast<Instruction>(NewGEP));
      if (auto *LI = dyn_cast<LoadInst>(T.MemOp))
        LI->setOperand(0, NewGEP);
      else
        cast<StoreInst>(T.MemOp)->setOperand(1, NewGEP);
    }
    Phi->dropAllReferences();
    S.PtrSteps[p]->eraseFromParent();
    Phi->eraseFromParent();
  }

  // The counter PHI and step form a cycle; remove the incoming edge first.
  S.IV->dropAllReferences();
  S.Step->eraseFromParent();
  S.IV->eraseFromParent();
  return true;
}

} // namespace

namespace revenant {

llvm::PreservedAnalyses UnivPass::run(llvm::Function &F,
                                      llvm::FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return llvm::PreservedAnalyses::all();
  bool Changed = false;
  // Snapshot headers: the rewrite adds PHIs but never blocks.
  llvm::SmallVector<BasicBlock *, 16> Headers;
  for (BasicBlock &BB : F)
    if (!predecessors(&BB).empty())
      Headers.push_back(&BB);
  for (BasicBlock *H : Headers) {
    BasicBlock *Latch = findLatch(H);
    if (!Latch)
      continue;
    LoopShape S;
    if (matchIntLoop(H, Latch, S)) {
      Changed |= rewriteIntLoop(S);
      continue;
    }
    S = LoopShape();
    if (matchPtrLoop(H, Latch, S))
      Changed |= rewritePtrLoop(S);
  }
  if (!Changed)
    return llvm::PreservedAnalyses::all();
  return llvm::PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-univ` name in Pipeline/Register.cpp, so the entry point is
// compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
// Windows needs the explicit dllexport; without it opt fails with
// "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUniv", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-univ") {
                    PM.addPass(revenant::UnivPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
