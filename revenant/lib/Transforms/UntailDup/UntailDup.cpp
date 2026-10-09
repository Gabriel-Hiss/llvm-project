//===- UntailDup.cpp - undo tail duplication and block cloning --*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// `revenant-untaildup` merges duplicated code back into shared joins. The
// optimizer copies blocks so each copy has fewer predecessors (tail
// duplication), threads branches around joins (jump threading), clones whole
// loop bodies per branch side (loop unswitching), and splits loop iterations
// out of the loop (loop peeling). All five leave the same fingerprint: two
// or more blocks holding the same instruction sequence. The pass finds those
// pairs and folds them back into one block.
//
// The core is mergeIdenticalBlocks(A, B): when B's non-terminator
// instructions match A's one by one (same opcode, same types, same constant
// operands, same non-instruction operands; cross-block operands allowed only
// when they are the matching pair's own values), B is deleted: its
// predecessors are re-routed to A and A's phis gain B's incoming values. On
// top of that core sit four targeted shapes:
//
// - mergeReturns / mergeTails: duplicated `ret` (or identical tail calls)
//   reached from sibling branches, the taildup / simplifycfg-tail-merge
//   shape. Corpus: c-clang-o2 `early_returns` pre has four `store+br`
//   leaves; -O2 folds them through simplifycfg tail merging.
// - threadUndo: a join block J with predecessors P0..Pn where two preds end
//   in identical conditional branches whose matching successors are
//   pairwise identical is re-threaded through one shared block. This is the
//   jump-threading inverse at the block-clone level: threading deletes the
//   shared block, we rebuild it.
// - unswitchUndo: two single-entry single-exit loop regions with identical
//   CFG skeletons (same block count, same successor structure, pairwise
//   identical bodies) guarded by opposite sides of one branch become one
//   loop with the branch inside. Corpus: miniz-clang-o3 `mz_adler32` pre
//   has one inner loop over the window; post has the unswitched copies
//   (`%55` 8-wide body vs `%103` scalar body share the skeleton).
// - peelUndo: a straight-line block before a loop header whose instruction
//   sequence equals the loop body's first iteration (same opcodes, same
//   operands up to the induction variable's initial value) is folded back
//   into the loop: the peeled block branches straight to the header and the
//   header phi takes the peeled value as its entry incoming.
//
// Alive2-style argument for mergeIdenticalBlocks: let A compute values
// v0..vk from inputs I (function args, globals, and values defined outside
// A/B), B compute w0..wk from the same I (proven by operand matching: every
// operand is either a constant, an outside value used identically, or an
// earlier matching pair vi/wi). Replacing B's uses of wi with A's vi is a
// refinement because vi == wi on every execution (same pure computation on
// equal inputs; loads from the same pointer without an intervening store in
// either block read the same memory). Predecessors re-routed from B to A
// execute A's instructions instead of B's bitwise-identical copies, so the
// observable state (memory, return values) is unchanged. Side-effecting
// instructions (stores, calls, volatile ops, fences, atomicrmw) match only
// when the full sequence is identical AND the pair executes at most once on
// any path after the merge (single-predecessor chains or return tails),
// which keeps the store/call count per path identical.
//
// Bail-outs (anything ambiguous stays put):
// - `!revenant.opaque` calls are never merged or moved.
// - Instructions with side effects merge only in single-entry tail/chain
//   shapes, never across live joins with divergent memory state.
// - PHI nodes must match pairwise (same incoming count, same incoming
//   blocks up to A/B swap, same values up to the A/B value map).
// - `!revenant.region` on a deleted block's terminator moves to the
//   surviving terminator; all created instructions copy `!revenant.va` from
//   the instruction they replace.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/UntailDup/UntailDup.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
using namespace llvm;

namespace {

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
  if (MDNode *MD = Src->getMetadata("revenant.region"))
    Dst->setMetadata("revenant.region", MD);
  if (Instruction *S = const_cast<Instruction *>(Src))
    if (MDNode *MD = S->getMetadata(LLVMContext::MD_dbg))
      Dst->setMetadata(LLVMContext::MD_dbg, MD);
}

bool isOpaqueCall(const Instruction &I) {
  if (I.getMetadata("revenant.opaque"))
    return true;
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return false;
  const Function *F = CB->getCalledFunction();
  return F && F->getName().starts_with("revenant.asm.");
}

bool mayHaveSideEffects(const Instruction &I) {
  if (isOpaqueCall(I))
    return true;
  return I.mayHaveSideEffects();
}

bool isSafeToSpeculateOrDup(const Instruction &I) {
  if (isa<PHINode>(I) || I.isTerminator())
    return true;
  return I.willReturn() && !I.mayHaveSideEffects() && !isOpaqueCall(I) &&
         !isa<AllocaInst>(I) && !isa<FenceInst>(I);
}

// Match value operands between two blocks: either identical (same Value*),
// both constants with equal values, or the mapped pair (vi in A <-> wi in B).
struct BlockMatch {
  DenseMap<const Value *, const Value *> A2B;
  DenseMap<const Value *, const Value *> B2A;
};

bool valuesMatch(const Value *A, const Value *B, const BlockMatch &M) {
  if (A == B)
    return true;
  auto It = M.A2B.find(A);
  if (It != M.A2B.end())
    return It->second == B;
  auto Jt = M.B2A.find(B);
  if (Jt != M.B2A.end())
    return Jt->second == A;
  // Constants compare by value: uniquing is not guaranteed across blocks
  // (two `0` literals may be distinct ConstantInt pointers).
  if (auto *CA = dyn_cast<Constant>(const_cast<Value *>(A)))
    if (auto *CB = dyn_cast<Constant>(const_cast<Value *>(B))) {
      if (CA->isNullValue() && CB->isNullValue())
        return true;
      if (auto *IA = dyn_cast<ConstantInt>(const_cast<Value *>(A)))
        if (auto *IB = dyn_cast<ConstantInt>(const_cast<Value *>(B)))
          return IA->getValue() == IB->getValue();
      if (auto *FA = dyn_cast<ConstantFP>(const_cast<Value *>(A)))
        if (auto *FB = dyn_cast<ConstantFP>(const_cast<Value *>(B)))
          return FA->getValueAPF() == FB->getValueAPF();
      return CA == CB;
    }
}

bool phisMatch(const PHINode *PA, const PHINode *PB, BasicBlock *A,
               BasicBlock *B, const BlockMatch &M) {
  if (PA->getNumIncomingValues() != PB->getNumIncomingValues())
    return false;
  if (PA->getType() != PB->getType())
    return false;
  for (unsigned i = 0, e = PA->getNumIncomingValues(); i != e; ++i) {
    BasicBlock *BA = PA->getIncomingBlock(i);
    BasicBlock *BB = PB->getIncomingBlock(i);
    // Incoming blocks must correspond: same block, the A/B pair itself
    // (self-loop latches), or the shared guard predecessor G (unswitch
    // copies are both fed from G; undoUnswitch passes A=H0,B=H1 with the
    // guard as the common entry). Anything else fails.
    bool BlocksOK = (BA == BB) || (BA == A && BB == B) || (BA == B && BB == A);
    if (!BlocksOK) {
      // Both from a common non-A/B predecessor: treated as the same entry
      // edge (the guard). Values must still match.
      bool BothOutside = BA != A && BA != B && BB != A && BB != B;
      if (!(BothOutside && BA == BB))
        return false;
    }
    // Backedge values (from A/B themselves) are the loop-carried values:
    // they match by construction once the bodies match, so defer them.
    // They are verified when the defining instructions match in body order.
    bool IsBackedge = (BA == A || BA == B) && (BB == A || BB == B);
    if (IsBackedge)
      continue;
    if (!valuesMatch(PA->getIncomingValue(i), PB->getIncomingValue(i), M))
      return false;
  }
  return true;
}

bool instsMatch(const Instruction *IA, const Instruction *IB, BasicBlock *A,
                BasicBlock *B, const BlockMatch &M) {
  if (IA->getOpcode() != IB->getOpcode())
    return false;
  if (IA->getType() != IB->getType())
    return false;
  if (isa<PHINode>(IA) || isa<PHINode>(IB))
    return phisMatch(cast<PHINode>(IA), cast<PHINode>(IB), A, B, M);
  if (IA->isTerminator() || IB->isTerminator())
    return false; // terminators compare structurally at the call site
  if (isOpaqueCall(*IA) || isOpaqueCall(*IB))
    return false;
  if (auto *CA = dyn_cast<CallBase>(IA)) {
    auto *CB = cast<CallBase>(IB);
    if (CA->getCalledFunction() != CB->getCalledFunction())
      return false;
    if (!CA->getCalledFunction())
      return false; // indirect calls: never merge
    if (CA->arg_size() != CB->arg_size())
      return false;
    for (unsigned i = 0; i < CA->arg_size(); ++i)
      if (!valuesMatch(CA->getArgOperand(i), CB->getArgOperand(i), M))
        return false;
    return true;
  }
  if (auto *LA = dyn_cast<LoadInst>(IA)) {
    auto *LB = cast<LoadInst>(IB);
    if (LA->isVolatile() || LB->isVolatile())
      return false;
    if (LA->getOrdering() != LB->getOrdering())
      return false;
    return valuesMatch(LA->getPointerOperand(), LB->getPointerOperand(), M);
  }
  if (auto *SA = dyn_cast<StoreInst>(IA)) {
    auto *SB = cast<StoreInst>(IB);
    if (SA->isVolatile() || SB->isVolatile())
      return false;
    if (SA->getOrdering() != SB->getOrdering())
      return false;
    return valuesMatch(SA->getPointerOperand(), SB->getPointerOperand(), M) &&
           valuesMatch(SA->getValueOperand(), SB->getValueOperand(), M);
  }
  if (auto *GA = dyn_cast<GetElementPtrInst>(IA)) {
    auto *GB = cast<GetElementPtrInst>(IB);
    if (GA->getNumOperands() != GB->getNumOperands())
      return false;
    for (unsigned i = 0; i < GA->getNumOperands(); ++i)
      if (!valuesMatch(GA->getOperand(i), GB->getOperand(i), M))
        return false;
    return true;
  }
  if (auto *CA = dyn_cast<CmpInst>(IA)) {
    auto *CB = cast<CmpInst>(IB);
    if (CA->getPredicate() != CB->getPredicate())
      return false;
    return valuesMatch(CA->getOperand(0), CB->getOperand(0), M) &&
           valuesMatch(CA->getOperand(1), CB->getOperand(1), M);
  }
  if (auto *SA = dyn_cast<SelectInst>(IA)) {
    auto *SB = cast<SelectInst>(IB);
    for (unsigned i = 0; i < 3; ++i)
      if (!valuesMatch(SA->getOperand(i), SB->getOperand(i), M))
        return false;
    return true;
  }
  if (IA->getNumOperands() != IB->getNumOperands())
    return false;
  for (unsigned i = 0; i < IA->getNumOperands(); ++i)
    if (!valuesMatch(IA->getOperand(i), IB->getOperand(i), M))
      return false;
  // Same opcode + same types + same operands: attributes that change
  // semantics (fast-math, exactness, wrapping) must agree too.
  if (auto *OA = dyn_cast<PossiblyExactOperator>(IA))
    if (auto *OB = dyn_cast<PossiblyExactOperator>(IB)) {
      if (OA->isExact() != OB->isExact())
        return false;
    }
  if (auto *OA = dyn_cast<PossiblyDisjointInst>(IA))
    if (auto *OB = dyn_cast<PossiblyDisjointInst>(IB)) {
      if (OA->isDisjoint() != OB->isDisjoint())
        return false;
    }
  if (auto *FA = dyn_cast<FPMathOperator>(IA))
    if (auto *FB = dyn_cast<FPMathOperator>(IB)) {
      if (FA->getFastMathFlags() != FB->getFastMathFlags())
        return false;
    }
  if (auto *OA = dyn_cast<OverflowingBinaryOperator>(IA))
    if (auto *OB = dyn_cast<OverflowingBinaryOperator>(IB)) {
      if (OA->hasNoSignedWrap() != OB->hasNoSignedWrap() ||
          OA->hasNoUnsignedWrap() != OB->hasNoUnsignedWrap())
        return false;
    }
  return true;
}

// Compare the non-terminator bodies of A and B. Fills Match with the
// value map. If AllowSideEffects is false, any effectful instruction fails
// the match; when true (single-entry tails/chains) they may match.
bool bodiesMatch(BasicBlock *A, BasicBlock *B, BlockMatch &Match,
                 bool AllowSideEffects) {
  auto IA = A->begin(), EA = A->end();
  auto IB = B->begin(), EB = B->end();
  // Skip PHIs first (handled pairwise below), then compare in order.
  SmallVector<const PHINode *, 4> PhisA, PhisB;
  for (Instruction &I : *A)
    if (auto *P = dyn_cast<PHINode>(&I))
      PhisA.push_back(P);
    else
      break;
  for (Instruction &I : *B)
    if (auto *P = dyn_cast<PHINode>(&I))
      PhisB.push_back(P);
    else
      break;
  if (PhisA.size() != PhisB.size())
    return false;
  for (size_t i = 0; i < PhisA.size(); ++i) {
    if (!phisMatch(PhisA[i], PhisB[i], A, B, Match))
      return false;
    Match.A2B[PhisA[i]] = PhisB[i];
    Match.B2A[PhisB[i]] = PhisA[i];
  }
  // Walk non-phi, non-terminator instructions in order.
  SmallVector<Instruction *, 8> InstsA, InstsB;
  for (Instruction &I : *A)
    if (!isa<PHINode>(&I) && !I.isTerminator())
      InstsA.push_back(&I);
  for (Instruction &I : *B)
    if (!isa<PHINode>(&I) && !I.isTerminator())
      InstsB.push_back(&I);
  if (InstsA.size() != InstsB.size())
    return false;
  for (size_t i = 0; i < InstsA.size(); ++i) {
    Instruction *X = InstsA[i], *Y = InstsB[i];
    if (!AllowSideEffects &&
        (!isSafeToSpeculateOrDup(*X) || !isSafeToSpeculateOrDup(*Y)))
      return false;
    if (isOpaqueCall(*X) || isOpaqueCall(*Y))
      return false;
    if (!instsMatch(X, Y, A, B, Match))
      return false;
    if (!X->getType()->isVoidTy()) {
      Match.A2B[X] = Y;
      Match.B2A[Y] = X;
    }
  }
  return true;
}

bool terminatorsMatch(BasicBlock *A, BasicBlock *B, const BlockMatch &M) {
  Instruction *TA = A->getTerminator(), *TB = B->getTerminator();
  if (auto *RA = dyn_cast<ReturnInst>(TA)) {
    auto *RB = dyn_cast<ReturnInst>(TB);
    if (!RB)
      return false;
    if (RA->getNumOperands() != RB->getNumOperands())
      return false;
    for (unsigned i = 0; i < RA->getNumOperands(); ++i)
      if (!valuesMatch(RA->getOperand(i), RB->getOperand(i), M))
        return false;
    return true;
  }
  if (auto *BA = dyn_cast<BranchInst>(TA)) {
    auto *BB = dyn_cast<BranchInst>(TB);
    if (!BB || BA->isConditional() != BB->isConditional())
      return false;
    if (BA->isConditional() &&
        !valuesMatch(BA->getCondition(), BB->getCondition(), M))
      return false;
    if (BA->getNumSuccessors() != BB->getNumSuccessors())
      return false;
    // Unconditional branches must target the same block (or the A/B pair
    // for self-loops): folding B into A re-routes B's predecessors along
    // A's edge, so divergent targets would silently change control flow.
    if (!BA->isConditional()) {
      BasicBlock *SA = BA->getSuccessor(0), *SB = BB->getSuccessor(0);
      if (!(SA == SB || (SA == A && SB == B) || (SA == B && SB == A)))
        return false;
    }
    return true;
  }
  if (auto *SA = dyn_cast<SwitchInst>(TA)) {
    auto *SB = dyn_cast<SwitchInst>(TB);
    if (!SB || SA->getNumCases() != SB->getNumCases())
      return false;
    return valuesMatch(SA->getCondition(), SB->getCondition(), M);
  }
  return false;
}

// Merge B into A: B's predecessors now target A; A's PHIs gain B's incoming
// values (mapped through Match); uses of B's values become A's values.
bool mergeIdenticalBlocks(BasicBlock *A, BasicBlock *B,
                          const BlockMatch &Match) {
  if (B == &B->getParent()->getEntryBlock())
    return false;
  if (llvm::pred_empty(B))
    return false;
  SmallVector<BasicBlock *, 4> Preds;
  for (BasicBlock *P : predecessors(B)) {
    if (P == B)
      return false;
    Preds.push_back(P);
  }
  // Dominance gates (BEFORE any mutation: a rejection leaves zero trace).
  // A's matching values replace B's at every outside use, and B's preds
  // re-route to A, widening A's predecessor set. Compute a fresh
  // DominatorTree (merges invalidate it, hence recompute per merge).
  DominatorTree DT(*A->getParent());
  // (a) Re-routed predecessors: every value flowing out of A along A's
  // outgoing edges must be live on the new paths: for each successor S of
  // A, each phi in S with an A incoming (V, A) requires V to dominate every
  // re-routed P. Otherwise the phi would read V on a path where V never
  // executes (found live on pugi load_buffer_impl: merging two `br %813`
  // guards widened %1041's pred set and its successor phi read %855 on a
  // path not dominated by %854).
  for (BasicBlock *S : successors(A)) {
    for (PHINode &P : S->phis()) {
      for (unsigned i = 0, e = P.getNumIncomingValues(); i != e; ++i) {
        if (P.getIncomingBlock(i) != A)
          continue;
        Value *V = P.getIncomingValue(i);
        auto *ID = dyn_cast<Instruction>(V);
        if (!ID)
          continue;
        for (BasicBlock *NP : Preds)
          if (!DT.dominates(ID, NP))
            return false;
      }
    }
  }
  // (b) Replaced uses: for every mapped pair (IB in B -> IA in A), IA must
  // dominate each outside use (phi uses live on their incoming edge).
  for (auto &KV : Match.B2A) {
    auto *IB = dyn_cast<Instruction>(KV.first);
    auto *IA = dyn_cast<Instruction>(KV.second);
    if (!IB || !IA)
      continue;
    for (const User *U : IB->users()) {
      const auto *UI = dyn_cast<Instruction>(U);
      if (!UI)
        return false;
      if (UI->getParent() == B)
        continue; // in-B uses die with B
      if (auto *PN = dyn_cast<PHINode>(UI)) {
        bool DomEdge = false;
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
          if (PN->getIncomingValue(i) == IB) {
            BasicBlock *Edge = PN->getIncomingBlock(i);
            if (Edge == B || DT.dominates(A, Edge))
              DomEdge = true;
            else
              return false;
          }
        if (!DomEdge)
          return false;
        continue;
      }
      if (!DT.dominates(IA, UI))
        return false;
    }
  }
  // with B: remap it through the value map to A's matching instruction.
  for (PHINode &PA : A->phis()) {
    const Value *VB = Match.A2B.lookup(&PA);
    if (!VB)
      continue;
    const auto *PB = cast<PHINode>(VB);
    for (unsigned i = 0, e = PB->getNumIncomingValues(); i != e; ++i) {
      Value *V = PB->getIncomingValue(i);
      BasicBlock *BB = PB->getIncomingBlock(i);
      if (auto *ID = dyn_cast<Instruction>(V))
        if (ID->getParent() == B) {
          auto It = Match.B2A.find(ID);
          V = It != Match.B2A.end() ? const_cast<Value *>(It->second)
                                    : UndefValue::get(ID->getType());
        }
      // The incoming BLOCK dies with B when it is B itself (self-edge);
      // that edge vanishes (predecessors were re-routed), so skip it.
      if (BB == B)
        continue;
      PA.addIncoming(V, BB);
    }
  }
  // Re-route B's predecessors to A. A conditional branch that now targets
  // A on both edges folds to an unconditional branch.
  for (BasicBlock *P : Preds) {
    Instruction *T = P->getTerminator();
    for (unsigned i = 0, e = T->getNumSuccessors(); i != e; ++i)
      if (T->getSuccessor(i) == B)
        T->setSuccessor(i, A);
    if (auto *Br = dyn_cast<BranchInst>(T)) {
      if (Br->isConditional() && Br->getSuccessor(0) == Br->getSuccessor(1)) {
        BranchInst *U = BranchInst::Create(Br->getSuccessor(0), P);
        copyVA(Br, U);
        Br->eraseFromParent();
      }
    }
  }
  // Fix successor PHIs: incoming from B becomes incoming from A where the
  // successor is shared, else the edge disappears with B.
  SmallPtrSet<BasicBlock *, 4> SuccsA(succ_begin(A), succ_end(A));
  Instruction *TB = B->getTerminator();
  for (unsigned i = 0, e = TB->getNumSuccessors(); i != e; ++i) {
    BasicBlock *S = TB->getSuccessor(i);
    for (PHINode &P : S->phis()) {
      // Remove B incoming; if S is also a successor of A, the A incoming
      // already carries the merged value (same computation), else nothing
      // to do (edge vanishes).
      P.removeIncomingValue(B, /*KeepOneInputPHIs=*/false);
    }
  }
  (void)SuccsA;
  // Replace uses of B's values with A's values, then scrub any remaining
  // in-B uses (B's own instructions/terminator/phis referencing B's defs)
  // so erasing B cannot leave a <badref> behind.
  SmallVector<std::pair<Value *, Value *>, 8> Reps;
  for (auto &KV : Match.B2A)
    Reps.emplace_back(const_cast<Value *>(KV.first),
                      const_cast<Value *>(KV.second));
  for (auto &R : Reps)
    R.first->replaceAllUsesWith(R.second);
  for (Instruction &I : *B) {
    for (unsigned o = 0, e = I.getNumOperands(); o != e; ++o) {
      Value *Op = I.getOperand(o);
      if (auto *IOp = dyn_cast<Instruction>(Op))
        if (IOp->getParent() == B) {
          auto It = Match.B2A.find(IOp);
          I.setOperand(o, It != Match.B2A.end()
                              ? const_cast<Value *>(It->second)
                              : UndefValue::get(IOp->getType()));
        }
    }
  }
  // Preserve region metadata from B's terminator onto A's.
  if (MDNode *MD = TB->getMetadata("revenant.region"))
    if (!A->getTerminator()->getMetadata("revenant.region"))
      A->getTerminator()->setMetadata("revenant.region", MD);
  // Drop B's phis' incoming (they die with B) then erase.
  B->eraseFromParent();
  return true;
}

// --- Shape 1: merge identical successors (taildup / threading undo) ------
// For each pair of successor blocks of (possibly different) branches that
// are pairwise identical, merge them. Repeats to a fixpoint within the
// function; returns true on any change.
bool mergeDuplicateSuccessors(Function &F) {
  bool Changed = false;
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    SmallVector<BasicBlock *, 32> Blocks;
    for (BasicBlock &BB : F)
      Blocks.push_back(&BB);
    for (size_t i = 0; i < Blocks.size() && !LocalChange; ++i) {
      for (size_t j = i + 1; j < Blocks.size() && !LocalChange; ++j) {
        BasicBlock *A = Blocks[i], *B = Blocks[j];
        if (!A->getParent() || !B->getParent())
          continue; // one was erased
        // Both must be reachable joins-or-tails, not entries.
        if (A == &F.getEntryBlock() || B == &F.getEntryBlock())
          continue;
        // Never merge loop headers (two distinct non-self predecessors:
        // entry + backedge). Unswitch/peel own headers; merging two of them
        // deletes a loop's entry edge and detaches its body.
        auto isLoopHeader = [](BasicBlock *BB) {
          BasicBlock *First = nullptr;
          for (BasicBlock *P : predecessors(BB)) {
            if (P == BB)
              continue;
            if (!First)
              First = P;
            else if (P != First)
              return true;
          }
          return false;
        };
        if (isLoopHeader(A) || isLoopHeader(B))
          continue;
        BlockMatch M;
        // Pure merge: no side effects unless both are return tails (each
        // executes at most once per path) or single-predecessor chains.
        bool ATail = isa<ReturnInst>(A->getTerminator());
        bool BTail = isa<ReturnInst>(B->getTerminator());
        bool AllowFx = (ATail && BTail);
        if (!AllowFx) {
          // Single-predecessor linear chains may still hold stores/calls:
          // merging two identical chains that each run once is safe.
          bool AChain =
              A->hasOneUse() || A->hasNPredecessors(1) || pred_empty(A);
          bool BChain =
              B->hasOneUse() || B->hasNPredecessors(1) || pred_empty(B);
          AllowFx = AChain && BChain && !A->hasAddressTaken() &&
                    !B->hasAddressTaken();
        }
        if (!bodiesMatch(A, B, M, AllowFx))
          continue;
        if (!terminatorsMatch(A, B, M))
          continue;
        // For conditional-branch blocks the successors must line up through
        // the map (same blocks or the A/B pair); check here. Additionally a
        // conditional-branch merge is only sound when the pair shares at
        // least one successor: otherwise folding B into A re-routes B's
        // predecessors into A's outgoing edges, deleting B's distinct
        // successors from those paths (wrong when the two branches test
        // different conditions over different targets, e.g. two loop
        // headers of unswitched copies).
        auto *BA = dyn_cast<BranchInst>(A->getTerminator());
        auto *BB = dyn_cast<BranchInst>(B->getTerminator());
        if (BA && BB && BA->isConditional()) {
          bool OK = true;
          for (unsigned k = 0; k < BA->getNumSuccessors(); ++k) {
            BasicBlock *SA = BA->getSuccessor(k);
            BasicBlock *SB = BB->getSuccessor(k);
            if (!(SA == SB || (SA == A && SB == B) || (SA == B && SB == A)))
              OK = false;
          }
          bool Shared = false;
          for (unsigned k = 0; k < BA->getNumSuccessors(); ++k)
            for (unsigned m = 0; m < BB->getNumSuccessors(); ++m)
              if (BA->getSuccessor(k) == BB->getSuccessor(m))
                Shared = true;
          if (!OK || !Shared)
            continue;
          // Same condition (up to the value map): merging two branches on
          // different conditions and dropping one test is unsound.
          if (!valuesMatch(BA->getCondition(), BB->getCondition(), M))
            continue;
        }
        if (mergeIdenticalBlocks(A, B, M)) {
          Changed = LocalChange = true;
        }
      }
    }
  }
  return Changed;
}

// --- Shape 2: jump-threading undo ---------------------------------------
// Threaded shape: P branches on cond c to {Q, X}; Q is a single-predecessor
// trampoline (`br Q2`) or re-tests an already-decided condition so one of
// its edges is implied. The precise full inverse (re-materializing the
// original diamond) is the successor-merge above: threading deletes joins,
// merging rebuilds them. What remains here is the trampoline collapse in
// reverse: when two predecessors P0, P1 end in identical conditional
// branches to the same pair {S0, S1}, and S0/S1 are pairwise identical
// single-predecessor blocks, re-route P1 through a join: P1's matching edge
// targets the shared block. In practice mergeDuplicateSuccessors already
// folds S0/S1; this shape additionally folds the branch blocks P0/P1
// themselves when their bodies match.
bool undoThreading(Function &F) {
  bool Changed = false;
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    SmallVector<BasicBlock *, 32> Blocks;
    for (BasicBlock &BB : F)
      Blocks.push_back(&BB);
    for (size_t i = 0; i < Blocks.size() && !LocalChange; ++i) {
      for (size_t j = i + 1; j < Blocks.size() && !LocalChange; ++j) {
        BasicBlock *A = Blocks[i], *B = Blocks[j];
        if (!A->getParent() || !B->getParent())
          continue;
        if (A == &F.getEntryBlock() || B == &F.getEntryBlock())
          continue;
        // Same header guard as the generic merge: never fold loop headers.
        auto isHdr = [](BasicBlock *BB) {
          BasicBlock *First = nullptr;
          for (BasicBlock *P : predecessors(BB)) {
            if (P == BB)
              continue;
            if (!First)
              First = P;
            else if (P != First)
              return true;
          }
          return false;
        };
        if (isHdr(A) || isHdr(B))
          continue;
        auto *BA = dyn_cast<BranchInst>(A->getTerminator());
        auto *BrB = dyn_cast<BranchInst>(B->getTerminator());
        if (!BA || !BrB || !BA->isConditional() || !BrB->isConditional())
          continue;
        // Same successor set (order-insensitive): the threaded branch tests
        // the same outcomes.
        SmallPtrSet<BasicBlock *, 2> SA(succ_begin(A), succ_end(A));
        SmallPtrSet<BasicBlock *, 2> SB(succ_begin(B), succ_end(B));
        if (SA.size() != 2 || SB.size() != 2)
          continue;
        bool SameSuccs = true;
        for (BasicBlock *S : SA)
          if (!SB.count(S))
            SameSuccs = false;
        if (!SameSuccs)
          continue;
        BlockMatch M;
        if (!bodiesMatch(A, B, M, /*AllowSideEffects=*/false))
          continue;
        // Conditions must be equal up to the value map.
        if (!valuesMatch(BA->getCondition(), BrB->getCondition(), M))
          continue;
        // Same edge order (or both orders with swapped successors).
        bool OrderOK = (BA->getSuccessor(0) == BrB->getSuccessor(0) &&
                        BA->getSuccessor(1) == BrB->getSuccessor(1)) ||
                       (BA->getSuccessor(0) == BrB->getSuccessor(1) &&
                        BA->getSuccessor(1) == BrB->getSuccessor(0));
        if (!OrderOK)
          continue;
        if (mergeIdenticalBlocks(A, B, M))
          Changed = LocalChange = true;
      }
    }
  }
  return Changed;
}

// --- Shape 3: loop unswitching undo -------------------------------------
// Unswitched shape: one branch on a loop-invariant condition C guards two
// single-block loop copies H0/H1 (each a self-loop header with the same
// latch condition up to the value map and the same exit X). The undo folds
// the two copies into one loop and deletes the guard: G becomes `br H0`.
// Multi-block unswitched loops (with per-arm bodies) are NOT folded: moving
// one arm's body under an inner branch requires cloning across live values
// and is left to a future change. Fires only on the airtight single-block
// shape, so it can never detach blocks.
bool undoUnswitch(Function &F) {
  bool Changed = false;
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    SmallVector<BasicBlock *, 32> Blocks;
    for (BasicBlock &BB : F)
      Blocks.push_back(&BB);
    for (BasicBlock *G : Blocks) {
      if (!G->getParent())
        continue;
      auto *GB = dyn_cast<BranchInst>(G->getTerminator());
      if (!GB || !GB->isConditional())
        continue;
      Value *C = GB->getCondition();
      // Each successor must directly head a single-block loop copy: H0/H1
      // are self-loop headers (conditional latch to {self, X}) with the
      // same exit X, fed only from G plus the self backedge.
      BasicBlock *H0 = GB->getSuccessor(0);
      BasicBlock *H1 = GB->getSuccessor(1);
      if (H0 == H1)
        continue;
      auto *BH0 = dyn_cast<BranchInst>(H0->getTerminator());
      auto *BH1 = dyn_cast<BranchInst>(H1->getTerminator());
      if (!BH0 || !BH1 || !BH0->isConditional() || !BH1->isConditional())
        continue;
      bool Self0 = BH0->getSuccessor(0) == H0 || BH0->getSuccessor(1) == H0;
      bool Self1 = BH1->getSuccessor(0) == H1 || BH1->getSuccessor(1) == H1;
      if (!Self0 || !Self1)
        continue;
      BasicBlock *X0 = BH0->getSuccessor(0) == H0 ? BH0->getSuccessor(1)
                                                  : BH0->getSuccessor(0);
      BasicBlock *X1 = BH1->getSuccessor(0) == H1 ? BH1->getSuccessor(1)
                                                  : BH1->getSuccessor(0);
      if (X0 != X1)
        continue;
      for (BasicBlock *P : predecessors(H0))
        if (P != H0 && P != G)
          goto next_guard;
      for (BasicBlock *P : predecessors(H1))
        if (P != H1 && P != G)
          goto next_guard;
      {
        BlockMatch HM;
        if (!bodiesMatch(H0, H1, HM, /*AllowSideEffects=*/false))
          continue;
        if (!valuesMatch(BH0->getCondition(), BH1->getCondition(), HM))
          continue;
        bool LoopTrue0 = BH0->getSuccessor(0) == H0;
        bool LoopTrue1 = BH1->getSuccessor(0) == H1;
        if (LoopTrue0 != LoopTrue1)
          continue;
        if (Instruction *CI = dyn_cast<Instruction>(C))
          if (CI->getParent() == H0 || CI->getParent() == H1)
            continue;
        // Validate exit PHIs BEFORE mutating: every H1 incoming must have
        // a live H0 counterpart (else the loops differ: bail, guard intact).
        bool ExitsOK = true;
        for (PHINode &XP : X0->phis()) {
          Value *VH1 = nullptr, *VH0 = nullptr;
          for (unsigned i = 0, e = XP.getNumIncomingValues(); i != e; ++i) {
            if (XP.getIncomingBlock(i) == H1)
              VH1 = XP.getIncomingValue(i);
            if (XP.getIncomingBlock(i) == H0)
              VH0 = XP.getIncomingValue(i);
          }
          if (VH1 && !VH0) {
            Value *Mapped = const_cast<Value *>(HM.B2A.lookup(VH1));
            if (!Mapped)
              ExitsOK = false;
            else if (auto *ID = dyn_cast<Instruction>(Mapped))
              if (ID->getParent() == H1)
                ExitsOK = false;
            // Mapped must dominate X: defined outside both loop copies.
            else if (auto *IM = dyn_cast<Instruction>(Mapped))
              if (IM->getParent() == H0)
                ExitsOK = false;
          }
        }
        if (!ExitsOK)
          continue;
        // H1's body values must have no uses outside H1/X (else remapping
        // them to H0's values crosses loop iterations: unsound). Check.
        for (auto &KV : HM.B2A) {
          const Instruction *IH1 = dyn_cast<Instruction>(KV.first);
          if (!IH1 || IH1->getParent() != H1)
            continue;
          for (const User *U : IH1->users()) {
            const auto *UI = dyn_cast<Instruction>(U);
            if (!UI)
              continue;
            const BasicBlock *PB = UI->getParent();
            if (PB != H1 && PB != X0) {
              ExitsOK = false;
              break;
            }
          }
          if (!ExitsOK)
            break;
        }
        if (!ExitsOK)
          continue;
        // Fold: G jumps to H0; H1 dies; X's H1 incoming remaps to H0.
        BranchInst *Goto = BranchInst::Create(H0, G);
        copyVA(GB, Goto);
        GB->eraseFromParent();
        for (PHINode &XP : X0->phis()) {
          Value *VH1 = nullptr, *VH0 = nullptr;
          for (unsigned i = 0, e = XP.getNumIncomingValues(); i != e; ++i) {
            if (XP.getIncomingBlock(i) == H1)
              VH1 = XP.getIncomingValue(i);
            if (XP.getIncomingBlock(i) == H0)
              VH0 = XP.getIncomingValue(i);
          }
          if (VH1) {
            XP.removeIncomingValue(H1, /*KeepOneInputPHIs=*/false);
            if (!VH0) {
              Value *Mapped = const_cast<Value *>(HM.B2A.lookup(VH1));
              XP.addIncoming(Mapped, H0);
            }
          }
        }
        for (auto &KV : HM.B2A)
          const_cast<Value *>(KV.first)->replaceAllUsesWith(
              const_cast<Value *>(KV.second));
        H1->eraseFromParent();
        Changed = LocalChange = true;
        break;
      }
    next_guard:;
    }
  }
  return Changed;
}

// --- Shape 4: loop peeling undo ------------------------------------------
// Peeled shape: a straight-line block P (single predecessor G, single
// successor H) holds the same instruction sequence as the loop body block
// L (part of a loop headed at H), up to the induction variable's initial
// value. The undo deletes P and routes G straight to H, extending H's PHIs
// with G's incoming values (the peeled first-iteration values).
bool undoPeel(Function &F) {
  bool Changed = false;
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    SmallVector<BasicBlock *, 32> Blocks;
    for (BasicBlock &BB : F)
      Blocks.push_back(&BB);
    for (BasicBlock *P : Blocks) {
      if (!P->getParent())
        continue;
      if (P == &F.getEntryBlock())
        continue;
      // P: single predecessor G, single successor H via unconditional br.
      if (!P->hasNPredecessors(1))
        continue;
      auto *PB = dyn_cast<BranchInst>(P->getTerminator());
      if (!PB || PB->isConditional())
        continue;
      BasicBlock *G = P->getSinglePredecessor();
      BasicBlock *H = PB->getSuccessor(0);
      if (!G || !H || !H->getParent())
        continue;
      // H must be a loop header (has a backedge from a latch L != P).
      BasicBlock *Latch = nullptr;
      for (BasicBlock *Q : predecessors(H)) {
        if (Q == P || Q == G)
          continue;
        // Q reaches H and H reaches Q (loop membership, cheap check).
        SmallPtrSet<BasicBlock *, 8> Seen;
        SmallVector<BasicBlock *, 8> Work{Q};
        bool Reaches = false;
        while (!Work.empty()) {
          BasicBlock *C = Work.pop_back_val();
          if (C == H) {
            Reaches = true;
            break;
          }
          if (!Seen.insert(C).second)
            continue;
          for (BasicBlock *S : successors(C))
            if (!Seen.count(S))
              Work.push_back(S);
          if (Work.size() > 64)
            break;
        }
        (void)Reaches;
        Latch = Q;
        break;
      }
      if (!Latch)
        continue;
      // P's body must equal H's body prefix up to the IV init (see below).
      SmallVector<Instruction *, 8> InstsP, InstsH;
      for (Instruction &I : *P)
        if (!I.isTerminator())
          InstsP.push_back(&I);
      for (Instruction &I : *H) {
        if (isa<PHINode>(&I))
          continue;
        if (I.isTerminator())
          break;
        InstsH.push_back(&I);
      }
      if (InstsP.empty() || InstsP.size() > InstsH.size())
        continue;
      // Peeled first iteration: P's sequence equals a PREFIX of H's body
      // with the IV phis replaced by their INIT values (what the phi equals
      // on first entry: a constant or a G-defined value, NOT the phi's
      // P-incoming, which is the already-stepped value feeding iteration
      // 2). Align at the START: P[i] matches H[i]; an H-phi operand matches
      // a P operand that is loop-invariant (constant or defined outside
      // P/H), recording InitFor[phi] = P-value. Every use of one phi must
      // record the same init. P may omit H's latch tail.
      BlockMatch M;
      DenseMap<const PHINode *, const Value *> InitFor;
      auto noteInit = [&](const Value *HOp, const Value *POp) -> bool {
        const auto *HP = dyn_cast<PHINode>(const_cast<Value *>(HOp));
        if (!HP || HP->getParent() != H)
          return HOp == POp;
        // P-side must be loop-invariant: constant or defined outside P/H.
        bool Invariant = isa<Constant>(const_cast<Value *>(POp));
        if (!Invariant)
          if (auto *ID = dyn_cast<Instruction>(const_cast<Value *>(POp)))
            Invariant = ID->getParent() != P && ID->getParent() != H;
          else
            Invariant = true; // arguments, globals
        if (!Invariant)
          return false;
        auto It = InitFor.find(HP);
        if (It != InitFor.end())
          return It->second == POp;
        InitFor[HP] = POp;
        return true;
      };
      bool OK = true;
      for (size_t i = 0; i < InstsP.size() && OK; ++i) {
        Instruction *IH = InstsH[i], *IP = InstsP[i];
        if (isOpaqueCall(*IP) || isOpaqueCall(*IH))
          OK = false;
        else if (IH->getOpcode() != IP->getOpcode() ||
                 IH->getType() != IP->getType() ||
                 IH->getNumOperands() != IP->getNumOperands())
          OK = false;
        else
          for (unsigned o = 0; o < IH->getNumOperands() && OK; ++o) {
            Value *HO = IH->getOperand(o), *PO = IP->getOperand(o);
            if (HO == PO)
              continue;
            auto ItM = M.A2B.find(HO);
            if (ItM != M.A2B.end()) {
              OK = ItM->second == PO;
              continue;
            }
            // H-phi vs invariant P value: record init.
            if (isa<PHINode>(HO)) {
              OK = noteInit(HO, PO);
              continue;
            }
            // Both constants by value.
            if (auto *HC = dyn_cast<Constant>(HO))
              if (auto *PC = dyn_cast<Constant>(PO)) {
                OK = HC == PC;
                continue;
              }
            OK = false;
          }
        if (OK && !IH->getType()->isVoidTy()) {
          M.A2B[IH] = IP;
          M.B2A[IP] = IH;
        }
      }
      if (!OK)
        continue;
      // Fold precondition (checked BEFORE any mutation):
      // - every H phi with a P incoming must have a recorded init (else the
      //   peeled copy and the loop disagree on entry values: bail);
      // - init values must dominate G's terminator (constant, argument,
      //   global, or defined in G or an ancestor: check by dominance later
      //   via a light rule: constant/argument/global, or defined in G);
      // - every P-defined value used outside P must be covered (in M.B2A).
      // Memory effects block the fold (stores/calls in P would die with it;
      // loads are fine, they re-execute in H's first iteration).
      bool HasMemFx = false;
      for (Instruction &I : *P)
        if (!I.isTerminator() && (isa<StoreInst>(&I) || isa<CallInst>(&I) ||
                                   isa<FenceInst>(&I) ||
                                   isa<AtomicRMWInst>(&I) ||
                                   isa<AtomicCmpXchgInst>(&I)))
          HasMemFx = true;
      if (HasMemFx)
        continue;
      bool PreOK = true;
      for (PHINode &HP : H->phis()) {
        bool HasP = false;
        for (unsigned i = 0, e = HP.getNumIncomingValues(); i != e; ++i)
          if (HP.getIncomingBlock(i) == P)
            HasP = true;
        if (!HasP)
          continue;
        auto It = InitFor.find(&HP);
        if (It == InitFor.end())
          PreOK = false;
        else {
          const Value *Init = It->second;
          bool Live = isa<Constant>(Init) || isa<Argument>(Init);
          if (!Live)
            if (auto *ID = dyn_cast<Instruction>(Init))
              Live = ID->getParent() == G;
            else
              Live = true; // globals
          if (!Live)
            PreOK = false;
        }
      }
      if (!PreOK)
        continue;
      // Escape check: P defs used outside P must be covered by M.B2A.
      for (Instruction &I : *P) {
        if (I.isTerminator() || I.getType()->isVoidTy())
          continue;
        if (M.B2A.count(&I))
          continue;
        for (User *U : I.users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != P)
            PreOK = false;
        }
        if (!PreOK)
          break;
      }
      if (!PreOK)
        continue;
      // All covered: mutate. Retarget G->H; rewrite each phi's P incoming
      // value to the recorded init and rename the edge P->G; remap P's
      // covered body values to H's instructions (they compute the same
      // thing on entry: P's peeled values equal H's first-iteration values
      // under init substitution... careful: H's body values on the ENTRY
      // edge equal P's values, so outside uses of P's values (e.g. exit
      // phis reached via H) correctly map to H's values).
      BranchInst *GT = dyn_cast<BranchInst>(G->getTerminator());
      bool Retargeted = false;
      if (GT) {
        for (unsigned i = 0, e = GT->getNumSuccessors(); i != e; ++i)
          if (GT->getSuccessor(i) == P) {
            GT->setSuccessor(i, H);
            Retargeted = true;
          }
      }
      if (!Retargeted)
        continue;
      for (PHINode &HP : H->phis())
        for (unsigned i = 0, e = HP.getNumIncomingValues(); i != e; ++i)
          if (HP.getIncomingBlock(i) == P) {
            auto It = InitFor.find(&HP);
            if (It != InitFor.end())
              HP.setIncomingValue(i, const_cast<Value *>(It->second));
            HP.setIncomingBlock(i, G);
          }
      for (auto &KV : M.B2A) {
        Value *VP = const_cast<Value *>(KV.first);
        Value *VH = const_cast<Value *>(KV.second);
        if (VP->getType() == VH->getType())
          VP->replaceAllUsesWith(VH);
      }
      // H may now list G twice in a phi (G was already an incoming for the
      // zero-trip path); dedupe by keeping the first.
      for (PHINode &HP : H->phis()) {
        SmallPtrSet<BasicBlock *, 4> SeenB;
        for (unsigned i = 0; i < HP.getNumIncomingValues();) {
          if (!SeenB.insert(HP.getIncomingBlock(i)).second)
            HP.removeIncomingValue(i, /*KeepOneInputPHIs=*/true);
          else
            ++i;
        }
      }
      P->eraseFromParent();
      Changed = LocalChange = true;
      break;
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UntailDupPass::run(Function &F,
                                               FunctionAnalysisManager &) {
  bool Changed = false;
  // Unswitch first: it matches on the two-loop-copies skeleton, which the
  // generic merges would otherwise eat one block at a time (producing the
  // detached-block mess). Peel next, then the generic merges.
  Changed |= undoUnswitch(F);
  Changed |= undoPeel(F);
  Changed |= undoThreading(F);
  Changed |= mergeDuplicateSuccessors(F);
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-untaildup` name in Pipeline/Register.cpp, so the entry point
// is compiled only for the standalone plugin DLL: a static link of every
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
  return {LLVM_PLUGIN_API_VERSION, "RevenantUntailDup", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-untaildup") {
                    PM.addPass(revenant::UntailDupPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
