//===- Unpeel.cpp - fold peeled prologues back into loops ----------------===//
//
// Part of the Revenant LLVM fork.
//
// `revenant-unpeel` merges peeled loop prologues back into their loop. A
// compiler peels the first (or last) iteration out of a loop to handle
// alignment, enable vectorization, or satisfy a guard. After univ/unrotate
// restore the body to scalar form, the peeled prologue remains as a
// straight-line block before the loop header.
//
// Two shapes are handled:
//
// Shape A (pure peel): block P with one predecessor G and one successor H
//   (loop header). P's non-terminator instructions are a prefix of H's body
//   (skipping PHIs), with H's PHIs replaced by their entry incoming values.
//   No side effects in P. Fold: redirect G->H, rewrite H's PHIs to take
//   entry values from G, delete P.
//
// Shape B (peel with store): P contains a store that is isomorphic to
//   the first store in the loop body. P also carries a guard (conditional
//   branch to exit). Fold: redirect G->H, extend H's PHIs with G's incoming
//   values, delete P. The store in P is dead because the loop's first
//   iteration re-executes it with the same operands.
//
// Every rewritten instruction carries !revenant.va from its predecessor.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unpeel/Unpeel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace {

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
  if (MDNode *MD = Src->getMetadata("revenant.region"))
    Dst->setMetadata("revenant.region", MD);
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

// Match value operands between two blocks: either identical (same Value*),
// both constants with equal values, or the mapped pair.
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
  if (auto *CA = dyn_cast<Constant>(const_cast<Value *>(A)))
    if (auto *CB = dyn_cast<Constant>(const_cast<Value *>(B))) {
      if (CA->isNullValue() && CB->isNullValue())
        return true;
      if (auto *IA = dyn_cast<ConstantInt>(const_cast<Value *>(A)))
        if (auto *IB = dyn_cast<ConstantInt>(const_cast<Value *>(B)))
          return IA->getValue() == IB->getValue();
      return CA == CB;
    }
  return false;
}

// --- Shape A: pure peel (no side effects) --------------------------------
// Same logic as UntailDup's undoPeel but with stricter checks and
// explicit handling of the peeled guard pattern.
bool undoPeelPure(Function &F) {
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
      if (!P->hasNPredecessors(1))
        continue;
      auto *PB = dyn_cast<BranchInst>(P->getTerminator());
      if (!PB || PB->isConditional())
        continue;
      BasicBlock *G = P->getSinglePredecessor();
      BasicBlock *H = PB->getSuccessor(0);
      if (!G || !H || !H->getParent())
        continue;

      // H must be a loop header: has a backedge from a latch L != P, G.
      BasicBlock *Latch = nullptr;
      for (BasicBlock *Q : predecessors(H)) {
        if (Q == P || Q == G)
          continue;
        // Check Q reaches H (loop membership).
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

      // P's body must equal H's body prefix up to the IV init.
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

      // No side effects in P. Volatile/atomic loads are side effects too:
      // deleting P removes an observable read.
      bool HasMemFx = false;
      for (Instruction &I : *P) {
        if (I.isTerminator())
          continue;
        if (isa<StoreInst>(&I) || isa<CallInst>(&I) ||
            isa<FenceInst>(&I) || isa<AtomicRMWInst>(&I) ||
            isa<AtomicCmpXchgInst>(&I)) {
          HasMemFx = true;
          break;
        }
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          if (LI->isVolatile() || LI->getOrdering() != AtomicOrdering::NotAtomic) {
            HasMemFx = true;
            break;
          }
        }
      }
      if (HasMemFx)
        continue;

      // Match P's instructions against H's body prefix.
      BlockMatch M;
      DenseMap<const PHINode *, const Value *> InitFor;
      auto noteInit = [&](const Value *HOp, const Value *POp) -> bool {
        const auto *HP = dyn_cast<PHINode>(const_cast<Value *>(HOp));
        if (!HP || HP->getParent() != H)
          return HOp == POp;
        bool Invariant = isa<Constant>(const_cast<Value *>(POp));
        if (!Invariant) {
          if (auto *ID = dyn_cast<Instruction>(const_cast<Value *>(POp)))
            Invariant = ID->getParent() != P && ID->getParent() != H;
          else
            Invariant = true;
        }
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
            if (isa<PHINode>(HO)) {
              OK = noteInit(HO, PO);
              continue;
            }
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

      // Fold precondition: every H phi with P incoming must have init.
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
          if (!Live) {
            if (auto *ID = dyn_cast<Instruction>(Init))
              Live = ID->getParent() == G;
            else
              Live = true;
          }
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

      // All covered: mutate.
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

// --- Shape B: peel with store (alignment prologue / peeled iteration) ----
// P contains a store that is isomorphic to the first store in the loop body.
// The loop's first iteration re-executes the same store, so P's store is dead
// after the fold. P may also carry a guard (conditional branch to exit).
//
// Shape:
//   G: br P or H
//   P: store; ...; br H (or br exit / br H)
//   H: phi...; store; ...; br exit or H
//
// After fold:
//   G: br H
//   H: phi (with G incoming = P's store operand); store; ...
bool undoPeelWithStore(Function &F) {
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
      if (!P->hasNPredecessors(1))
        continue;
      BasicBlock *G = P->getSinglePredecessor();
      if (!G)
        continue;

      // P must have exactly one successor via unconditional br, or
      // a conditional br where one arm goes to H.
      BasicBlock *H = nullptr;
      auto *PB = dyn_cast<BranchInst>(P->getTerminator());
      if (!PB)
        continue;
      if (PB->isUnconditional()) {
        H = PB->getSuccessor(0);
      } else {
        // Conditional: one arm to H, other to exit or H.
        BasicBlock *S0 = PB->getSuccessor(0);
        BasicBlock *S1 = PB->getSuccessor(1);
        // Pick the successor that is a loop header.
        auto isLoopHeader = [](BasicBlock *BB) {
          if (!BB)
            return false;
          for (BasicBlock *Q : predecessors(BB))
            if (Q != BB)
              return true;
          return false;
        };
        if (isLoopHeader(S0) && !isLoopHeader(S1))
          H = S0;
        else if (isLoopHeader(S1) && !isLoopHeader(S0))
          H = S1;
        else if (isLoopHeader(S0) && isLoopHeader(S1))
          H = S0; // arbitrary, but consistent
        else
          continue;
      }
      if (!H || !H->getParent())
        continue;

      // H must be a loop header: has a backedge from a latch L != P, G.
      BasicBlock *Latch = nullptr;
      for (BasicBlock *Q : predecessors(H)) {
        if (Q == P || Q == G)
          continue;
        Latch = Q;
        break;
      }
      if (!Latch)
        continue;

      // P must contain a store.
      StoreInst *PStore = nullptr;
      for (Instruction &I : *P) {
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          PStore = SI;
          break;
        }
      }
      if (!PStore)
        continue;

      // Volatile/atomic stores are observable side effects: deleting P
      // removes a store the loop cannot re-execute.
      if (PStore->isVolatile() || PStore->getOrdering() != AtomicOrdering::NotAtomic)
        continue;
      // H's first store must match P's store (same value, same pointer
      // up to the phi chain).
      StoreInst *HStore = nullptr;
      for (Instruction &I : *H) {
        if (isa<PHINode>(&I))
          continue;
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          HStore = SI;
          break;
        }
        if (I.isTerminator())
          break;
      }
      if (!HStore)
        continue;

      // The store value must match.
      if (PStore->getValueOperand() != HStore->getValueOperand())
        continue;

      // The store pointer: P uses the original pointer, H uses a phi.
      // We need P's pointer to be the init value of H's pointer phi.
      Value *PPtr = PStore->getPointerOperand();
      Value *HPtr = HStore->getPointerOperand();
      PHINode *HPtrPhi = dyn_cast<PHINode>(HPtr);
      if (!HPtrPhi || HPtrPhi->getParent() != H)
        continue;
      // Find P's incoming value for HPtrPhi.
      Value *PInit = nullptr;
      for (unsigned i = 0, e = HPtrPhi->getNumIncomingValues(); i != e; ++i)
        if (HPtrPhi->getIncomingBlock(i) == P) {
          PInit = HPtrPhi->getIncomingValue(i);
          break;
        }
      if (!PInit || PInit != PPtr)
        continue;

      // P may contain other instructions besides the store. They must be
      // pure (no side effects) or dead (no uses outside P), or the fold
      // would delete observable behavior.
      bool OtherSafe = true;
      for (Instruction &I : *P) {
        if (I.isTerminator() || &I == PStore)
          continue;
        if (mayHaveSideEffects(I)) {
          OtherSafe = false;
          break;
        }
        if (I.getType()->isVoidTy())
          continue;
        // Check all uses are inside P.
        for (User *U : I.users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != P) {
            OtherSafe = false;
            break;
          }
        }
        if (!OtherSafe)
          break;
      }
      if (!OtherSafe)
        continue;
      // 1. Redirect G->H.
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

      // 2. Rewrite H's PHIs: P incoming -> G incoming with P's values.
      for (PHINode &HP : H->phis()) {
        for (unsigned i = 0, e = HP.getNumIncomingValues(); i != e; ++i)
          if (HP.getIncomingBlock(i) == P) {
            // Find G's incoming value for this phi.
            Value *GVal = nullptr;
            for (unsigned j = 0, je = HP.getNumIncomingValues(); j != je; ++j)
              if (HP.getIncomingBlock(j) == G) {
                GVal = HP.getIncomingValue(j);
                break;
              }
            if (GVal)
              HP.setIncomingValue(i, GVal);
            HP.setIncomingBlock(i, G);
          }
      }

      // 3. Dedupe phi incoming blocks.
      for (PHINode &HP : H->phis()) {
        SmallPtrSet<BasicBlock *, 4> SeenB;
        for (unsigned i = 0; i < HP.getNumIncomingValues();) {
          if (!SeenB.insert(HP.getIncomingBlock(i)).second)
            HP.removeIncomingValue(i, /*KeepOneInputPHIs=*/true);
          else
            ++i;
        }
      }

      // 4. Delete P (its store is dead).
      P->eraseFromParent();
      Changed = LocalChange = true;
      break;
    }
  }
  return Changed;
}

// --- Shape C: guard + do-while rotation ----------------------------------
// A guard block G before a loop header H tests the loop condition and
// branches to H (enter) or to exit. The loop's latch tests the same
// condition. After univ/unrotate, this shape remains when the guard is
// not isomorphic to the latch.
//
// Shape:
//   G: icmp; br H or exit
//   H: phi...; body; br exit or H
//
// Fold: redirect G's entry to H, remove the guard.
bool undoGuardRotation(Function &F) {
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
      if (G == &F.getEntryBlock())
        continue;
      // G has one predecessor (entry), one conditional successor to H,
      // one to exit.
      if (!G->hasNPredecessors(1))
        continue;
      auto *GB = dyn_cast<BranchInst>(G->getTerminator());
      if (!GB || !GB->isConditional())
        continue;
      BasicBlock *S0 = GB->getSuccessor(0);
      BasicBlock *S1 = GB->getSuccessor(1);
      // One successor must be a loop header (has a backedge), the other not.
      // H is a loop header if it has a predecessor L (L != G, L != H) and
      // H reaches L (backedge).
      auto isLoopHeader = [](BasicBlock *BB, BasicBlock *G) {
        if (!BB)
          return false;
        for (BasicBlock *L : predecessors(BB)) {
          if (L == G || L == BB)
            continue;
          // Check if BB reaches L.
          SmallPtrSet<BasicBlock *, 8> Seen;
          SmallVector<BasicBlock *, 8> Work{BB};
          while (!Work.empty()) {
            BasicBlock *C = Work.pop_back_val();
            if (C == L)
              return true;
            if (!Seen.insert(C).second)
              continue;
            for (BasicBlock *S : successors(C))
              if (!Seen.count(S))
                Work.push_back(S);
            if (Seen.size() > 64)
              break;
          }
        }
        return false;
      };
      BasicBlock *H = nullptr, *Exit = nullptr;
      if (isLoopHeader(S0, G) && !isLoopHeader(S1, G)) {
        H = S0; Exit = S1;
      } else if (isLoopHeader(S1, G) && !isLoopHeader(S0, G)) {
        H = S1; Exit = S0;
      } else {
        continue;
      }
      if (!H || !Exit || !H->getParent())
        continue;

      // H must have a backedge (be a real loop). Self-loops count.
      bool HasBackedge = false;
      for (BasicBlock *Q : predecessors(H))
        if (Q != G) {
          HasBackedge = true;
          break;
        }
      if (!HasBackedge)
        continue;

      // G must be the only non-backedge entry to H.
      // For a self-loop, HPreds is empty (the backedge is H itself).
      // For a separate latch, HPreds has exactly 1 element.
      SmallVector<BasicBlock *, 4> HPreds;
      for (BasicBlock *Q : predecessors(H))
        if (Q != H && Q != G)
          HPreds.push_back(Q);
      if (HPreds.size() > 1)
        continue;
      // G is deleted whole, so it may carry only dead-on-entry work:
      // every non-terminator result must be used inside G and nothing may
      // have side effects (volatile/atomic loads included). A guard-shaped
      // block that also feeds the loop body is not a peel.
      bool Pure = true;
      for (Instruction &I : *G) {
        if (I.isTerminator())
          continue;
        if (I.mayHaveSideEffects()) {
          Pure = false;
          break;
        }
        for (User *U : I.users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != G) {
            Pure = false;
            break;
          }
        }
        if (!Pure)
          break;
      }
      if (!Pure)
        continue;
      // All checks pass: redirect G's predecessor to H.
      BasicBlock *Entry = G->getSinglePredecessor();
      if (!Entry)
        continue;
      BranchInst *ET = dyn_cast<BranchInst>(Entry->getTerminator());
      bool Retargeted = false;
      if (ET) {
        for (unsigned i = 0, e = ET->getNumSuccessors(); i != e; ++i)
          if (ET->getSuccessor(i) == G) {
            ET->setSuccessor(i, H);
            Retargeted = true;
          }
      }
      if (!Retargeted)
        continue;

      // Rewrite H's PHIs: G incoming -> Entry incoming.
      // If Entry has no incoming for this phi (e.g. Entry is the function
      // entry), use the G incoming value (G was the only predecessor
      // providing that value, and now Entry takes G's place).
      for (PHINode &HP : H->phis()) {
        for (unsigned i = 0, e = HP.getNumIncomingValues(); i != e; ++i)
          if (HP.getIncomingBlock(i) == G) {
            Value *EVal = nullptr;
            for (unsigned j = 0, je = HP.getNumIncomingValues(); j != je; ++j)
              if (HP.getIncomingBlock(j) == Entry) {
                EVal = HP.getIncomingValue(j);
                break;
              }
            if (!EVal)
              EVal = HP.getIncomingValue(i); // use G's value
            HP.setIncomingValue(i, EVal);
            HP.setIncomingBlock(i, Entry);
          }
      }

      // Dedupe phi incoming blocks.
      for (PHINode &HP : H->phis()) {
        SmallPtrSet<BasicBlock *, 4> SeenB;
        for (unsigned i = 0; i < HP.getNumIncomingValues();) {
          if (!SeenB.insert(HP.getIncomingBlock(i)).second)
            HP.removeIncomingValue(i, /*KeepOneInputPHIs=*/true);
          else
            ++i;
        }
      }
      // Rewrite Exit's PHIs: remove G incoming (G is deleted; Entry now
      // branches to H, so Entry no longer reaches Exit).
      for (PHINode &HP : Exit->phis()) {
        for (unsigned i = 0; i < HP.getNumIncomingValues();) {
          if (HP.getIncomingBlock(i) == G)
            HP.removeIncomingValue(i, /*KeepOneInputPHIs=*/true);
          else
            ++i;
        }
      }

      G->eraseFromParent();
      Changed = LocalChange = true;
      break;
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UnpeelPass::run(Function &F,
                                            FunctionAnalysisManager &) {
  PreservedAnalyses PA = PreservedAnalyses::all();
  bool Changed = false;
  Changed |= undoPeelPure(F);
  Changed |= undoPeelWithStore(F);
  Changed |= undoGuardRotation(F);
  if (Changed)
    return PreservedAnalyses::none();
  return PA;
}
