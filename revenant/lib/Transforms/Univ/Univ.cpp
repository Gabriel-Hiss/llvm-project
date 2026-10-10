//===- Univ.cpp - undo IV rewriting and strength reduction -----------===//
//
// Part of the Revenant LLVM fork.
//
// Undoes `indvars` IV rewriting plus `loop-reduce` strength reduction on the
// loop shapes the corpus actually shows:
//
// 1. Widened integer-IV loops: the loop walks an `i64` IV PHI from 0
//    stepping by 1, exits on `icmp eq %next, %wide.trip.count` (or `eq %n,
//    %next` with the bound already narrow, the MSVC/GCC form), and touches
//    one element per iteration through a typed `getelementptr <elem>` (the
//    O2 `sum_vec` scalar tail after the vector main loop, `fill_vec`/`add`
//    tails, `sum_vec_d` tails; c-clang-o2/o3 ir-post). The touched GEP may
//    index the IV directly, index a widening of it (`getelementptr i32,
//    %base, i64 %sext`, the -O0/clang and MSVC form), or walk bytes with
//    the IV stride equal to the element size (`getelementptr i8` + `add
//    %iv, 4` on i32 elements, the GCC LSR form), and its base may be an
//    invariant expression materialized inside the loop.
// 2. Byte-stride IV loops with a scaled bound: GCC keeps a byte index
//    (`getelementptr i8, %base, %iv`, `add %iv, 4`) and compares against
//    `shl (zext %n), 2`; the bound is divided back out to the source `%n`.
// 3. Pointer-increment byte loops: the loop threads one (copy: two) `ptr`
//    PHI stepped by `getelementptr i8, %p, 1` and counts iterations with a
//    kept `i32` counter PHI tested by `icmp eq %count, %trip` (O2/O3
//    `copy_bytes` scalar tail and 8x-unrolled remainder; ir-pre shows the
//    same pointers next to an `i32` index tested with `icmp slt i32 %i,
//    %n`, which is the shape we rebuild).
// 4. Pointer loops with an integer address mirror: the loop threads a `ptr`
//    PHI stepped by a constant-stride byte GEP plus an `i64` mirror PHI of
//    the same stride, and exits on `icmp eq %mirror-step, %end` where
//    `%end = %start + %n * stride` (GCC `sum_vec`/`hash_vec` LSR form).
// 5. Countdown trip counters: the loop trips on an `i32` (or `i64`) PHI
//    stepped by -1 and tested with `icmp eq %step, 0`, the loop-reduce form
//    MSVC keeps for byte copies and for the trip of an unrolled body. The
//    bound is the countdown's start value; an extra mirror whose step
//    escapes into the exit block (the returned element count) is
//    materialized as `%start + %stride * %new_step` on that edge.
//
// All four rewrite to the ir-pre shape: a single `i32` index PHI from 0,
// `add i32 %i, 1` in the latch, `icmp slt i32 %i, %n` in the header, one
// `getelementptr <elem>, %base, i32 %i` per touched element, and the pointer
// PHIs/counters replaced by the equivalent address expression. Loops whose
// IV steps by more than 1, or that were unrolled, stay for the unroll and
// vector passes; a folded (reroll) body trips on the countdown this pass
// eliminates, which is why the two compose.
//
// Corpus:
// - `c-clang-o2` `sum_vec` (ir-pre `icmp slt i32` + `sext` + `getelementptr
//   i32`; ir-post scalar tail `phi i64` + `icmp eq %36, %5` + typed GEP).
// - `c-clang-o2`/`c-clang-o3` `copy_bytes` scalar tail (`phi ptr` pair +
//   `i32` counter + `icmp eq %46, %35`) and 8x remainder (`phi i32` +
//   `phi ptr` pair, exit `icmp eq %89, %2`).
// - `c-clang-o2` `break_continue` (`phi i32` + `add %iv, 1` + `icmp eq %n,
//   %next`, the counter also feeding the in-body `%3` magic division, no
//   memory touch), `cmp_vec` (IV escaping into the early-exit block as
//   `trunc i64 %iv to i32`), `hash_vec`/`sum_vec_d` scalar tails (base GEP
//   materialized inside the loop from invariant operands), `sum_vec`/
//   `fill_vec` tails (three-entry IV PHI: entry, latch, vector exit).
// - `c-o2` (MSVC) `break_continue`/`hash_vec`/`map_call_vec`/`copy_vec`/
//   `cmp_vec`: already `icmp slt %next, %n` with a narrow `i32` IV, so the
//   pass leaves them; MSVC defers indvars to `-O2` only for the trips it
//   rewrites, and these loops are already source-shaped.
// - `c-gcc-o2` `sum_vec`/`hash_vec` (`phi ptr` + `phi i64` mirror + `icmp
//   eq %mirror-step, %start + %n * 4`), `copy_vec`/`add_vec` (`getelementptr
//   i8` + `add %iv, 4` + `icmp eq %iv-next, shl (zext %n), 2`), `copy_bytes`
//   (`getelementptr i8` + `add %iv, 1` + `icmp eq %iv-next, zext %n`).
// - `c-o2` `copy_bytes` (two `ptr` PHIs + countdown `i32` trip + escaping
//   `i64` mirror) and, after `revenant-reroll` folds their unrolled bodies,
//   `dot4` and the `c-gcc-o2` `fill_vec` (see reroll-composition.ll).
//
// Safety: single-IV loops only. The IV PHI must step by constant 1 (byte
// loops: by the touched element size), and every use must be the step, the
// exit compare, a `trunc` escape into an i32 result, a loop-invariant-valued
// base expression, an in-loop value use of an already-narrow IV, or one of
// the recognized addresses. One countdown trip PHI is allowed beside the
// index; it may only feed its step, the compare, and (mirrors) a PHI on the
// exit edge. The new exit `icmp slt %i, %n` computes the
// same trip count (0..n-1) when the bound `%n` dominates and the old bound
// provably equals its widening (or its stride scaling of it). `!revenant.va`
// moves to each rebuilt instruction; `!revenant.opaque` is copied over,
// never dropped.
//
// Alive2-style argument for the integer half: with start 0 and step 1 the
// old exit fires at the first `%iv == %wide`. When `%wide == zext %n` the
// new exit fires at the first `%i >= %n` (signed), the same iteration for
// `%n >= 0`; for `%n <= 0` both trip zero times because the preheader guard
// (`icmp sgt %n, 0`) skips the loop. The rebuilt GEP `base + %i * sizeof`
// addresses the same bytes as the old typed GEP `base + %iv * sizeof` at
// equal iteration values, and both PHIs step together so the values stay
// equal. Byte-stride loops: the old exit fires when the byte index reaches
// `%n * sizeof`; since the byte index starts at 0 and advances by `sizeof`
// that is exactly `%n` iterations, and `base + %i * sizeof` addresses the
// same bytes as `base + %byte_index`. Mirror loops: the old exit fires when
// the mirror reaches `%start + %n * stride`, which is `%n` iterations for
// `%n >= 0`, and each pointer touch `%p + %k * stride` equals the rebuilt
// `%start + %k * sizeof` while `sizeof` equals the stride.
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
// step instruction, which must live in Latch and consume the PHI, and
// records every loop-entry (non-latch) value in Starts. Each start must be
// constant 0 or loop-invariant: vector-tail remainders start at the main
// loop's end count, and lifted merged headers enter from several guards.
static Instruction *
matchStep(PHINode *Phi, BasicBlock *Header, BasicBlock *Latch,
          int64_t &StepC,
          SmallVectorImpl<std::pair<Value *, BasicBlock *>> &Starts) {
  Starts.clear();
  Value *Back = nullptr;
  for (unsigned i = 0, e = Phi->getNumIncomingValues(); i != e; ++i) {
    BasicBlock *Pred = Phi->getIncomingBlock(i);
    Value *V = Phi->getIncomingValue(i);
    if (Pred == Latch) {
      if (Back)
        return nullptr;
      Back = V;
      continue;
    }
    // A header edge is a self edge, impossible unless header == latch;
    // anything else is a loop entry.
    if (Pred == Header)
      return nullptr;
    if (auto *C = dyn_cast<ConstantInt>(V)) {
      if (!C->isZero())
        return nullptr;
    } else if (!isLoopInvariant(V, Header, Latch)) {
      return nullptr;
    }
    Starts.emplace_back(V, Pred);
  }
  if (!Back || Starts.empty())
    return nullptr;
  auto *Step = dyn_cast<Instruction>(Back);
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
  StepC = C->getValue().getSExtValue();
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

// A value defined outside the loop, or a pure address computation inside
// it whose operands are all loop-invariant: the lifter materializes byte
// base GEPs (`getelementptr i8, %base, %scale`) and slot inttoptrs in the
// loop header.
static bool isInvariantBase(Value *V, BasicBlock *Header, BasicBlock *Latch) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return true;
  if (isLoopInvariant(V, Header, Latch))
    return true;
  if (!isa<IntToPtrInst>(I) && !isa<GetElementPtrInst>(I) && !isa<BitCastInst>(I))
    return false;
  for (Value *Op : I->operands())
    if (!isLoopInvariant(Op, Header, Latch))
      return false;
  return true;
}

// The narrow `i32` bound behind a byte-stride scaling of the trip count:
// `shl (zext %n), k` or `mul %zext, K`/`mul K, %zext` where `K` equals the
// loop's byte stride. GCC LSR compares the byte index against the byte
// length `%n * sizeof`; dividing the stride back out yields the source `%n`.
static Value *scaledBound(Value *V, int64_t Stride) {
  if (Stride <= 0)
    return nullptr;
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return nullptr;
  if (I->getOpcode() == Instruction::Shl) {
    auto *C = dyn_cast<ConstantInt>(I->getOperand(1));
    if (!C)
      return nullptr;
    int64_t K = C->getSExtValue();
    if (K < 0 || K >= 62 || Stride != (1LL << K))
      return nullptr;
    Value *N = peelWidening(I->getOperand(0));
    return N && N->getType()->isIntegerTy(32) ? N : nullptr;
  }
  if (I->getOpcode() == Instruction::Mul) {
    for (unsigned K = 0; K < 2; ++K) {
      auto *C = dyn_cast<ConstantInt>(I->getOperand(K));
      if (!C || C->getSExtValue() != Stride)
        continue;
      Value *N = peelWidening(I->getOperand(1 - K));
      if (N && N->getType()->isIntegerTy(32))
        return N;
    }
  }
  if (I->getOpcode() == Instruction::Add) {
    for (unsigned K = 0; K < 2; ++K) {
      Value *N = scaledBound(I->getOperand(K), Stride);
      if (N)
        return N;
    }
  }
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
  // Loop-entry values of the integer IV, one per non-latch predecessor
  // (constant 0 or loop-invariant).
  SmallVector<std::pair<Value *, BasicBlock *>, 2> Starts;
  PHINode *Count = nullptr; // optional countdown PHI for a lifted remainder
  Instruction *CountStep = nullptr;
  Value *CountStart = nullptr;
  bool IsPtr = false;
  // Pointer loop whose trip count is an integer mirror of the pointers with
  // an end-address compare (`%end = %start + %n * stride`).
  bool Mirror = false;
  // Pointer loop whose trip count is a countdown PHI stepped by -1 and
  // tested with `icmp eq %step, 0`; the bound is its start value.
  bool PtrCountdown = false;
  SmallVector<PHINode *, 2> Ptrs;
  SmallVector<GetElementPtrInst *, 2> PtrSteps;
  // Self-feeding extra integer PHIs (kept mirrors/counters the exit compare
  // does not test); both they and their steps die with the rewrite. An extra
  // whose step feeds a PHI on the loop's exit edge (the LSR end value used
  // to compute the returned element count) is rewritten to
  // `%start + %stride * %new_step` at that edge instead.
  SmallVector<PHINode *, 2> ExtraTrips;
  SmallVector<Instruction *, 2> ExtraSteps;
  // Exit-edge escapes of the extra mirrors: (user PHI, extra entry value,
  // extra step constant). %user takes `%extra + %k` on the exit edge and is
  // rewritten to `%start + %k * %new_step`.
  SmallVector<std::tuple<PHINode *, Value *, int64_t>, 2> Escapes;
  SmallVector<Touch, 4> Touches;
  Type *ElemTy = nullptr;
  // `trunc i64 %iv to i32` escapes into the loop result (early-exit loops
  // such as `cmp_vec`); rewritten to the new narrow IV.
  SmallVector<TruncInst *, 2> TruncEscapes;
  // `zext`/`sext` of the IV that only fed the rebuilt addresses; dead after
  // the rewrite.
  SmallVector<Instruction *, 2> Widenings;
  // LSR countdown trip counter stepped by -1 (integer half only); its start
  // value is the bound and both it and its step die with the rewrite.
  PHINode *Countdown = nullptr;
  Instruction *CountdownStep = nullptr;
  // The old exit compare tested a step value (the IV's step or the
  // countdown's step), so the rebuilt compare tests the new step.
  bool TripTestsStep = false;
  // The old IV is already `i32`, so in-loop value uses of it keep working
  // through an RAUW instead of being rebuilt away.
  bool IVIsNarrow = false;
};

// Integer half. The IV steps by 1 (typed addressing) or by the touched
// element size (byte addressing), exits on `eq`/`ne` against a bound that
// is already `i32` or a widening (or a stride scaling) of an `i32` value,
// and touches memory through typed GEPs on the IV, on a widening of it, or
// through byte GEPs. An i32 IV may also feed in-loop value uses (the loop
// body computes on the index, e.g. the `i % 3` of `break_continue`) and
// may escape as `trunc` into the result.
static bool matchIntLoop(BasicBlock *Header, BasicBlock *Latch, LoopShape &S) {
  S = LoopShape();
  PHINode *IV = nullptr;
  Instruction *Step = nullptr;
  int64_t StepC = 0;
  SmallVector<std::pair<Value *, BasicBlock *>, 2> Starts;
  // A countdown trip PHI (stepped by -1) is not the index: it is the LSR trip
  // counter and its start value is the bound. Scanned separately from
  // matchStep because its start is the trip count, not a zero index.
  PHINode *Countdown = nullptr;
  Instruction *CountdownStep = nullptr;
  SmallVector<std::pair<Value *, BasicBlock *>, 2> CountdownStarts;
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isIntegerTy())
      continue;
    Value *Back = nullptr;
    SmallVector<std::pair<Value *, BasicBlock *>, 2> Entries;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      if (PN->getIncomingBlock(i) == Latch) {
        if (Back)
          Back = nullptr;
        Back = PN->getIncomingValue(i);
      } else {
        Entries.emplace_back(PN->getIncomingValue(i), PN->getIncomingBlock(i));
      }
    }
    auto *St = dyn_cast<Instruction>(Back);
    if (!St || St->getParent() != Latch ||
        St->getOpcode() != Instruction::Add)
      continue;
    Value *Other = St->getOperand(0) == PN   ? St->getOperand(1)
                   : St->getOperand(1) == PN ? St->getOperand(0)
                                             : nullptr;
    auto *C = Other ? dyn_cast<ConstantInt>(Other) : nullptr;
    if (!C || C->getSExtValue() != -1)
      continue;
    if (Countdown)
      return false; // two countdowns: not a shape we invert
    Countdown = PN;
    CountdownStep = St;
    CountdownStarts = std::move(Entries);
  }
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isIntegerTy())
      continue;
    SmallVector<std::pair<Value *, BasicBlock *>, 2> Cand;
    int64_t C = 0;
    Instruction *St = matchStep(PN, Header, Latch, C, Cand);
    if (!St)
      continue;
    if (IV)
      return false; // two stepped integer PHIs: coupled IVs
    IV = PN;
    Step = St;
    StepC = C;
    Starts = std::move(Cand);
  }
  if (!IV)
    return false;
  // No pointer PHIs either: those are the pointer half's loops.
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (PN->getType()->isPointerTy())
      return false;
  }

  // Exit compare in the header or latch: `ne`/`eq` on the IV, its step, or
  // the countdown trip PHI (whose step the LSR exit tests). A single-block
  // loop has Header == Latch; scan once so the exit compare is not seen
  // twice. Multi-block loops test in the latch (the header holds only
  // addresses); body-block compares belong to a different loop shape
  // (early exit) and stay put.
  ICmpInst *Cmp = nullptr;
  Value *Wide = nullptr;
  bool CountdownTrip = false;
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
      bool ACount = Countdown && (A == Countdown || A == CountdownStep);
      bool BCount = Countdown && (B == Countdown || B == CountdownStep);
      if (AIs == BIs && ACount == BCount)
        continue;
      // The other operand must be loop-invariant: body compares on the IV
      // (the `i % 3 == 0` of `break_continue`) are not trip counts.
      if (!isLoopInvariant(AIs || ACount ? B : A, Header, Latch))
        continue;
      if (Cmp)
        return false;
      Cmp = C;
      Wide = AIs || ACount ? B : A;
      CountdownTrip = ACount || BCount;
    }
  }
  if (!Cmp || !Wide || !isLoopInvariant(Wide, Header, Latch))
    return false;
  // The bound: an already-narrow `i32`, the widening of one, (byte loops)
  // its stride scaling, or the countdown's start value.
  Value *Narrow = nullptr;
  if (CountdownTrip) {
    if (CountdownStarts.size() != 1 || !CountdownStarts.front().first)
      return false;
    Narrow = CountdownStarts.front().first;
    if (!Narrow->getType()->isIntegerTy(32)) {
      Narrow = peelWidening(Narrow);
      if (!Narrow)
        return false;
    }
  } else if (Wide->getType()->isIntegerTy(32)) {
    Narrow = Wide;
  } else {
    Narrow = peelWidening(Wide);
    if (Narrow) {
      // `smax (n, 0)` folds the preheader guard into the trip count.
      if (auto *II = dyn_cast<IntrinsicInst>(Narrow)) {
        if (II->getIntrinsicID() == Intrinsic::smax &&
            isa<ConstantInt>(II->getOperand(1)) &&
            cast<ConstantInt>(II->getOperand(1))->isZero())
          Narrow = II->getOperand(0);
      }
    } else {
      Narrow = scaledBound(Wide, StepC);
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
  // Use discipline: IV feeds only step/compare/recognized addresses, an
  // early-exit `trunc`, or (when already narrow) in-loop value uses. The
  // step feeds only the PHI and the compare. Addresses may live in the
  // header, the latch, or a body block between them, and may index the IV
  // directly or through one widening of it (`getelementptr i32, %base, i64
  // %sext`, the -O0/MSVC form), so the widening's users are examined too.
  bool NarrowIV = IV->getType()->isIntegerTy(32);
  SmallVector<GetElementPtrInst *, 4> GEPs;
  SmallVector<TruncInst *, 2> Truncs;
  bool ByteTouch = false;  // a touch walks bytes with the element-size stride
  bool TypedTouch = false; // a touch walks elements (stride 1)
  // Element of the touched memory type, for byte GEPs whose source element
  // is `i8` but whose step is the element size.
  auto memType = [&](GetElementPtrInst *GI) -> Type * {
    for (User *G : GI->users()) {
      if (auto *L = dyn_cast<LoadInst>(G))
        return L->getType();
      if (auto *St = dyn_cast<StoreInst>(G))
        return St->getValueOperand()->getType();
    }
    return nullptr;
  };
  // Recognize one address GEP. Widen is the widening of the IV that indexes
  // it, or null when the GEP indexes the IV directly. Records the touch.
  auto touchGEP = [&](GetElementPtrInst *GI, Instruction *Widen) -> bool {
    if (!GI || GI->getNumIndices() != 1 || !inLoop(GI->getParent()))
      return false;
    Value *Idx = GI->getOperand(GI->getNumOperands() - 1);
    if (Idx == IV) {
      Widen = nullptr;
    } else if (Widen && Idx == Widen && Widen->getOperand(0) == IV) {
      if (!Widen->hasOneUse())
        return false; // the widening feeds other users too
    } else {
      return false;
    }
    Type *Elem = GI->getSourceElementType();
    if (Elem->isIntegerTy(8)) {
      // Byte addressing: the IV counts bytes, so its stride must be the
      // touched element size.
      Type *Mem = memType(GI);
      if (StepC <= 0 || !Mem || !Mem->isSized() ||
          StepC != (int64_t)Mem->getPrimitiveSizeInBits() / 8)
        return false;
      Elem = Mem;
      ByteTouch = true;
    } else {
      // Element addressing: the index advances one element per iteration.
      if (StepC != 1)
        return false;
      TypedTouch = true;
    }
    // The base must be loop-invariant-valued; the lifter materializes
    // byte base GEPs and slot inttoptrs inside the loop.
    if (!isInvariantBase(GI->getPointerOperand(), Header, Latch))
      return false;
    for (User *G : GI->users()) {
      auto *MI = dyn_cast<Instruction>(G);
      if (!MI || (!isa<LoadInst>(MI) && !isa<StoreInst>(MI)))
        return false;
      if (!inLoop(MI->getParent()))
        return false;
    }
    GEPs.push_back(GI);
    if (!S.ElemTy)
      S.ElemTy = Elem;
    else if (S.ElemTy != Elem)
      return false;
    return true;
  };
  SmallVector<Instruction *, 8> Widenings;
  for (User *U : IV->users()) {
    Instruction *I = dyn_cast<Instruction>(U);
    if (!I)
      return false;
    if (I == Step || I == Cmp)
      continue;
    if ((isa<ZExtInst>(I) || isa<SExtInst>(I)) && I->getOperand(0) == IV) {
      Widenings.push_back(I);
      continue;
    }
    if (touchGEP(dyn_cast<GetElementPtrInst>(I), nullptr))
      continue;
    if (auto *T = dyn_cast<TruncInst>(I)) {
      // Early-exit loops return the index: `trunc i64 %iv to i32` escapes
      // into the loop result.
      if (!NarrowIV && T->getType()->isIntegerTy(32))
        Truncs.push_back(T);
      continue;
    }
    // In-loop value uses are fine while the IV is already narrow: the
    // rewrite RAUWs the old PHI onto the new one.
    if (NarrowIV && inLoop(I->getParent()))
      continue;
    return false;
  }
  // A widening of the IV is the -O0 index form when it only feeds address
  // GEPs; when it also feeds arithmetic it is a value use of the index and
  // only survives an already-narrow IV.
  for (Instruction *W : Widenings) {
    bool OnlyGEPs = true;
    for (User *G : W->users())
      if (!isa<GetElementPtrInst>(G)) {
        OnlyGEPs = false;
        break;
      }
    if (!OnlyGEPs) {
      if (NarrowIV && inLoop(W->getParent()))
        continue;
      return false;
    }
    for (User *G : W->users())
      if (!touchGEP(dyn_cast<GetElementPtrInst>(G), W))
        return false;
  }
  for (User *U : Step->users()) {
    if (U == IV || U == Cmp)
      continue;
    return false;
  }
  // Step discipline: count up by one per element, or by the touched element
  // size per element on a byte loop. Countdowns, 2x-unrolled strides and
  // compound steps stay for the unroll pass; mixing byte and element
  // addressing on one IV is not a shape the corpus shows.
  if (StepC < 1 || (ByteTouch && TypedTouch) ||
      (StepC != 1 && !ByteTouch))
    return false;
  // Loops with no memory touch (pure accumulator loops such as
  // `break_continue`) still restore: the exit compare itself is the
  // indvars artifact that gets inverted.

  S.Header = Header;
  S.Latch = Latch;
  S.IV = IV;
  S.Step = Step;
  S.ExitCmp = Cmp;
  S.Bound = Narrow;
  S.IsPtr = false;
  S.Starts = std::move(Starts);
  S.TruncEscapes = std::move(Truncs);
  S.Widenings = std::move(Widenings);
  S.IVIsNarrow = NarrowIV;
  S.Countdown = Countdown;
  S.CountdownStep = CountdownStep;
  S.TripTestsStep = CountdownTrip ||
                    S.ExitCmp->getOperand(0) == Step ||
                    S.ExitCmp->getOperand(1) == Step;
  for (auto *GI : GEPs)
    S.Touches.push_back({nullptr, GI, nullptr});
  return true;
}
// Pointer half: one or two `ptr` PHIs stepped by a constant-stride byte GEP
// in the latch, plus an integer trip PHI, exited by `icmp eq %trip-step,
// %bound`. The trip PHI is either a kept `i32` counter from 0 stepping by 1
// (bound: the `i32` trip count) or an integer mirror of the pointers
// stepping by the same stride (bound: `%start + %n * stride`, the LSR end
// address). Loads/stores touch the ptr PHIs directly; unrolled remainders
// with extra constant GEPs do not match.
static bool matchPtrLoop(BasicBlock *Header, BasicBlock *Latch, LoopShape &S) {
  S = LoopShape();
  SmallVector<PHINode *, 2> Ptrs;
  SmallVector<GetElementPtrInst *, 2> Steps;
  Value *Start0 = nullptr;
  Type *ElemTy = nullptr;
  int64_t Stride = 0;
  unsigned Entries = 0;
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isPointerTy())
      continue;
    Value *Back = nullptr;
    Value *Start = nullptr;
    unsigned N = 0;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      if (PN->getIncomingBlock(i) == Latch) {
        if (Back)
          return false;
        Back = PN->getIncomingValue(i);
      } else {
        Start = PN->getIncomingValue(i);
        ++N;
      }
    }
    auto *Step = Back ? dyn_cast<GetElementPtrInst>(Back) : nullptr;
    if (!Step || Step->getParent() != Latch)
      return false;
    if (Step->getPointerOperand() != PN || Step->getNumIndices() != 1)
      return false;
    auto *C = dyn_cast<ConstantInt>(Step->getOperand(1));
    if (!C || C->getSExtValue() < 1)
      return false;
    int64_t K = C->getSExtValue();
    if (!isLoopInvariant(Start, Header, Latch))
      return false;
    if (Ptrs.size() == 2)
      return false;
    Ptrs.push_back(PN);
    Steps.push_back(Step);
    if (!Start0)
      Start0 = Start;
    // Byte addressing with a 1-element GEP is the classic pointer loop;
    // wider strides are the LSR form and their element type comes from the
    // touched memory.
    if (!Stride)
      Stride = K;
    else if (Stride != K)
      return false;
    if (!N)
      return false;
    if (!Entries)
      Entries = N;
    else if (Entries != N)
      return false; // pointer PHIs entered differently
    if (!ElemTy && K == 1)
      ElemTy = Step->getSourceElementType();
  }
  if (Ptrs.empty())
    return false;

  // Every stepped integer PHI of the header: the trip PHI (whose step or
  // value the exit compare tests) plus self-feeding mirrors the lifter
  // kept. Non-stepped integer PHIs are accumulators.
  struct Stepped {
    PHINode *Phi;
    Instruction *Step;
    int64_t StepC;
    SmallVector<std::pair<Value *, BasicBlock *>, 2> Starts;
  };
  SmallVector<Stepped, 4> Stepped;
  for (Instruction &I : *Header) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    if (!PN->getType()->isIntegerTy())
      continue;
    SmallVector<std::pair<Value *, BasicBlock *>, 2> Cand;
    int64_t C = 0;
    Instruction *St = matchStep(PN, Header, Latch, C, Cand);
    if (!St)
      continue;
    Stepped.push_back({PN, St, C, std::move(Cand)});
  }
  if (Stepped.empty())
    return false;

  // Exit: `icmp eq %trip-step, %bound`, false edge leaves the loop. A
  // single-block loop has Header == Latch; scan once so the exit compare
  // is not seen twice. The tested PHI decides the trip form: a kept `i32`
  // counter from 0 (bound: the `i32` trip count), an integer mirror of the
  // pointers (bound: `%start + %n * stride`), or a countdown trip counter
  // (bound: its start value).
  ICmpInst *Cmp = nullptr;
  Value *Bound = nullptr;
  unsigned TripIdx = 0;
  bool Mirror = false;
  bool Countdown = false;
  SmallVector<std::pair<Value *, BasicBlock *>, 2> MirrorStarts;
  SmallVector<BasicBlock *, 2> CmpBlocks{Header};
  if (Latch != Header)
    CmpBlocks.push_back(Latch);
  for (BasicBlock *BB : CmpBlocks) {
    for (Instruction &I : *BB) {
      auto *C = dyn_cast<ICmpInst>(&I);
      if (!C || C->getPredicate() != ICmpInst::ICMP_EQ)
        continue;
      Value *A = C->getOperand(0), *B = C->getOperand(1);
      unsigned Hit = 0;
      for (unsigned K = 0; K < Stepped.size(); ++K)
        if (A == Stepped[K].Phi || A == Stepped[K].Step ||
            B == Stepped[K].Phi || B == Stepped[K].Step)
          Hit = K + 1;
      if (!Hit)
        continue;
      const struct Stepped &T = Stepped[Hit - 1];
      if (!isLoopInvariant(A == T.Phi || A == T.Step ? B : A, Header, Latch))
        continue;
      if (Cmp)
        return false;
      Cmp = C;
      TripIdx = Hit - 1;
      Bound = A == T.Phi || A == T.Step ? B : A;
      if (T.StepC == 1 && T.Phi->getType()->isIntegerTy(32)) {
        // Classic kept counter: the bound is the i32 trip count.
      } else if (T.StepC == Stride) {
        // Address mirror: the bound is `%start + %n * stride`.
        Mirror = true;
        MirrorStarts = T.Starts;
      } else if (T.StepC == -1) {
        // Countdown trip counter: the bound is its start value.
        Countdown = true;
        MirrorStarts = T.Starts;
      } else {
        return false; // coupled IV tested by the exit
      }
    }
  }
  if (!Cmp)
    return false;
  SmallVector<std::tuple<PHINode *, Value *, int64_t>, 2> Escapes;
  // The other stepped PHIs feed only their step, and the step feeds the PHI
  // plus at most PHIs on the loop's exit edge (the LSR end value the caller
  // turns into the returned element count); those are rewritten to
  // `%start + %stride * %new_step` at the exit edge.
  for (unsigned K = 0; K < Stepped.size(); ++K) {
    const struct Stepped &S = Stepped[K];
    if (K == TripIdx)
      continue;
    if (S.StepC != Stride || S.Starts.size() != 1 || !S.Starts.front().first)
      return false; // the extra must mirror the pointers by the same stride
    Value *EStart = S.Starts.front().first;
    for (User *U : S.Step->users()) {
      if (U == S.Phi)
        continue;
      auto *P = dyn_cast<PHINode>(U);
      if (!P || P->getParent() == Header || P->getParent() == Latch)
        return false;
      // The PHI must take this value only from the compare's block (the
      // loop's exit edge).
      for (unsigned i = 0, e = P->getNumIncomingValues(); i != e; ++i)
        if (P->getIncomingValue(i) == S.Step &&
            P->getIncomingBlock(i) != Cmp->getParent())
          return false;
      Escapes.emplace_back(P, EStart, S.StepC);
    }
    for (User *U : S.Phi->users())
      if (U != S.Step)
        return false;
  }
  if (!Cmp || !Bound || !isLoopInvariant(Bound, Header, Latch))
    return false;
  if (Mirror) {
    // `%end = %start + %n * stride`; the new index runs 0..%n-1.
    if (MirrorStarts.size() != 1 || !MirrorStarts.front().first)
      return false;
    Value *MStart = MirrorStarts.front().first;
    auto *Add = dyn_cast<BinaryOperator>(Bound);
    if (!Add || Add->getOpcode() != Instruction::Add)
      return false;
    Value *N = scaledBound(Add->getOperand(1), Stride);
    if (N && Add->getOperand(0) == MStart)
      Bound = N;
    else if ((N = scaledBound(Add->getOperand(0), Stride)) &&
             Add->getOperand(1) == MStart)
      Bound = N;
    else
      return false;
  }
  if (Countdown) {
    // The countdown starts at the trip count (one entry only: the peeled
    // remainder form would need the exit count too).
    if (MirrorStarts.size() != 1 || !MirrorStarts.front().first)
      return false;
    Value *N = MirrorStarts.front().first;
    if (!N->getType()->isIntegerTy(32)) {
      N = peelWidening(N);
      if (!N)
        return false;
    }
    Bound = N;
  }
  if (!Bound->getType()->isIntegerTy(32) ||
      !isLoopInvariant(Bound, Header, Latch))
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
        Type *Mem = isa<LoadInst>(I) ? cast<LoadInst>(I)->getType()
                                     : cast<StoreInst>(I)->getValueOperand()->getType();
        if (Mirror || Stride != 1) {
          // Wider strides must match the touched element size exactly.
          if (!Mem->isSized() ||
              Stride != (int64_t)Mem->getPrimitiveSizeInBits() / 8)
            return false;
          ElemTy = Mem;
        }
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
  // The trip PHI feeds only its step and the compare.
  for (User *U : Stepped[TripIdx].Phi->users()) {
    if (U == Stepped[TripIdx].Step || U == Cmp)
      continue;
    return false;
  }
  if (Touches.empty())
    return false;
  if ((Mirror || Countdown) && Entries != 1)
    return false; // derived bounds are only restored for one entry

  S.Header = Header;
  S.Latch = Latch;
  S.IV = Stepped[TripIdx].Phi;
  S.Step = Stepped[TripIdx].Step;
  S.ExitCmp = Cmp;
  S.Escapes = std::move(Escapes);
  S.Bound = Bound;
  S.IsPtr = true;
  S.Mirror = Mirror;
  S.PtrCountdown = Countdown;
  // Self-feeding extra PHIs (kept mirrors/counters the compare does not
  // test) die with the rewrite.
  for (unsigned K = 0; K < Stepped.size(); ++K)
    if (K != TripIdx) {
      S.ExtraTrips.push_back(Stepped[K].Phi);
      S.ExtraSteps.push_back(Stepped[K].Step);
    }
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

  // Every loop entry (non-latch predecessor of the header) feeds one
  // incoming value of the new index: constant 0, an already-narrow value,
  // or a `trunc` of the wider start (vector-tail remainders, lifted
  // merged headers).
  SmallVector<std::pair<Value *, BasicBlock *>, 2> Entries = S.Starts;
  if (Entries.empty()) {
    for (BasicBlock *P : predecessors(S.Header)) {
      if (P == S.Latch || P == S.Header)
        continue;
      Entries.emplace_back(nullptr, P);
    }
  }
  if (Entries.empty())
    return false;
  {
    SmallPtrSet<BasicBlock *, 4> Seen;
    unsigned NonLatch = 0;
    for (BasicBlock *P : predecessors(S.Header))
      if (P != S.Latch && P != S.Header)
        ++NonLatch;
    for (auto &E : Entries) {
      if (!Seen.insert(E.second).second || E.second == S.Latch ||
          E.second == S.Header)
        return false; // not a merge of loop entries
      if (!E.first)
        return false;
    }
    if (NonLatch != Entries.size())
      return false;
  }

  IRBuilder<> HB(&S.Header->front());
  PHINode *NewIV = HB.CreatePHI(I32, Entries.size() + 1, S.IV->getName() + ".idx");
  moveVA(S.IV, NewIV);
  for (auto &E : Entries) {
    // Start the new index at the narrowed entry value: constant 0, an
    // already-narrow invariant, or `trunc` of a wider one.
    Value *NewStart = nullptr;
    if (auto *C = dyn_cast<ConstantInt>(E.first)) {
      if (!C->isZero())
        return false;
      NewStart = ConstantInt::get(I32, 0);
    } else if (E.first->getType() == I32) {
      NewStart = E.first;
    } else {
      IRBuilder<> PB(E.second->getTerminator());
      NewStart = PB.CreateTrunc(E.first, I32, S.IV->getName() + ".start");
      moveVA(S.IV, cast<Instruction>(NewStart));
    }
    if (!isLoopInvariant(NewStart, S.Header, S.Latch))
      return false;
    NewIV->addIncoming(NewStart, E.second);
  }
  // Insert the new step before the old step: same block, and it dominates
  // the latch compare plus every later use. (Inserting after the old step
  // puts it between the step and its compare, which breaks dominance of
  // the new PHI incoming when the step feeds the compare in the same
  // block.)
  IRBuilder<> LB(S.Step);
  Value *NewStep =
      LB.CreateAdd(NewIV, ConstantInt::get(I32, 1), S.Step->getName() + ".idx");
  moveVA(S.Step, cast<Instruction>(NewStep));
  NewIV->addIncoming(NewStep, S.Latch);
  if (!S.ExitCmp)
    return false;
  // Exit compare on the same operand the old one tested (PHI or step).
  Value *Tested = S.TripTestsStep ? static_cast<Value *>(NewStep)
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
  // Early-exit loops hand the index to the result as `trunc i64 %iv to i32`;
  // the new index is already i32 and dominates the same blocks.
  for (auto *T : S.TruncEscapes) {
    T->replaceAllUsesWith(NewIV);
    T->eraseFromParent();
  }
  // Widenings that only fed the rebuilt addresses are dead now.
  for (auto *W : S.Widenings)
    if (W->use_empty())
      W->eraseFromParent();
  // The countdown trip counter dies with its step: nothing references it
  // after the compare above was replaced.
  if (S.Countdown) {
    S.Countdown->dropAllReferences();
    S.CountdownStep->eraseFromParent();
    S.Countdown->eraseFromParent();
  }
  // Drop the old IV machinery. Detach the latch incoming (it references the
  // old step, erased next) before anything else; then rewrite any
  // remaining in-loop value user of an already-narrow IV onto the new
  // index. A widened IV has no users left by now: its only recognized uses
  // were the step, the compare, the addresses and the trunc escapes, all
  // rewritten or detached above.
  // (No blind replaceAllUsesWith on a widened IV: it asserts on the
  // deliberate i64 -> i32 type change even when no users remain.)
  for (unsigned i = 0, e = S.IV->getNumIncomingValues(); i != e; ++i)
    if (S.IV->getIncomingBlock(i) == S.Latch)
      S.IV->setIncomingValue(i, UndefValue::get(S.IV->getType()));
  if (S.IVIsNarrow)
    S.IV->replaceAllUsesWith(NewIV);
  S.Step->eraseFromParent();
  S.IV->eraseFromParent();
  return true;
}

// Rewrite a pointer loop: new i32 index from 0, `slt` exit against the
// trip bound when it is the source `%n` (i32), typed GEPs replacing the
// pointer PHIs. Trip bounds of the form `%n - %done` (vector remainders)
// do not match: bail. The trip PHI is either the kept `i32` counter or the
// integer address mirror; both die with their step.
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
  BasicBlock *CmpBB = S.ExitCmp->getParent();
  PBr->setCondition(cast<Value>(NewCmp));
  PBr->swapSuccessors();
  S.ExitCmp->eraseFromParent();

  // Exit-edge escapes: the LSR end value `%extra + %stride` leaves the loop
  // through a PHI on the exit edge. At that edge the new index has just
  // failed the compare, so `%new_step` is the trip count and the same value
  // is `%extra_start + %stride * %new_step`. Materialize it at the end of the
  // compare's block (which dominates the exit) and point the escape PHI at
  // it; the old extra PHI and its step then die below.
  for (auto &E : S.Escapes) {
    PHINode *User = std::get<0>(E);
    Value *EStart = std::get<1>(E);
    int64_t K = std::get<2>(E);
    IRBuilder<> EB(CmpBB->getTerminator());
    Value *Idx = NewStep;
    Type *ETy = EStart->getType();
    if (ETy != I32) {
      Idx = EB.CreateSExt(NewStep, ETy, "escape.idx");
      moveVA(S.Step, cast<Instruction>(Idx));
    }
    if (K != 1) {
      Idx = EB.CreateMul(Idx, ConstantInt::get(ETy, K), "escape.scaled");
      moveVA(S.Step, cast<Instruction>(Idx));
    }
    Value *Val = EB.CreateAdd(EStart, Idx, "escape.end");
    moveVA(S.Step, cast<Instruction>(Val));
    User->setIncomingValueForBlock(CmpBB, Val);
  }

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

  // The counter/mirror/countdown PHI and step form a cycle; remove the
  // incoming edge first. Self-feeding extras die the same way.
  for (auto *P : S.ExtraTrips)
    P->dropAllReferences();
  for (auto *St : S.ExtraSteps)
    St->eraseFromParent();
  for (auto *P : S.ExtraTrips)
    P->eraseFromParent();
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
