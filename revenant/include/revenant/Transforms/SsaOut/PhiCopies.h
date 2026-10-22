//===- PhiCopies.h - phi liveness and edge-copy order for phi locals ------===//
//
// Part of the Revenant LLVM fork.
//
// The two passes that replace phis with stack locals (`revenant-ssaout`
// and `revenant-locals-to-slots`) share these rules. Phis may share one
// local only when they never hold live values at the same time, and the
// copies one edge makes into phi locals are parallel: every copy reads
// the old value of the local it reads, even when another copy on the
// same edge writes that local.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_SSAOUT_PHICOPIES_H
#define REVENANT_TRANSFORMS_SSAOUT_PHICOPIES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

namespace revenant {

/// Where phis are live, for deciding which phis may share one local.
class PhiLiveness {
public:
  /// The blocks at whose start phi A is live: a use of A is reachable from
  /// there without passing A's own block, which redefines A. A phi use
  /// reads A at the end of its incoming block. Computed once per phi,
  /// backward from the uses.
  const llvm::SmallPtrSet<llvm::BasicBlock *, 16> &liveIn(llvm::PHINode *A) {
    auto [It, New] = LiveIn.try_emplace(A);
    llvm::SmallPtrSet<llvm::BasicBlock *, 16> &Live = It->second;
    if (!New)
      return Live;
    llvm::BasicBlock *Def = A->getParent();
    llvm::SmallVector<llvm::BasicBlock *, 16> Work;
    auto Reach = [&](llvm::BasicBlock *BB) {
      if (BB != Def && Live.insert(BB).second)
        Work.push_back(BB);
    };
    for (llvm::Use &U : A->uses()) {
      auto *I = llvm::cast<llvm::Instruction>(U.getUser());
      if (auto *PN = llvm::dyn_cast<llvm::PHINode>(I))
        Reach(PN->getIncomingBlock(U));
      else
        Reach(I->getParent());
    }
    while (!Work.empty())
      for (llvm::BasicBlock *Pred : llvm::predecessors(Work.pop_back_val()))
        Reach(Pred);
    return Live;
  }

  /// Two phis interfere when one is live where the other is defined (the
  /// start of its block). Phis of one block are defined together.
  bool interferes(llvm::ArrayRef<llvm::PHINode *> A,
                  llvm::ArrayRef<llvm::PHINode *> B) {
    for (llvm::PHINode *X : A)
      for (llvm::PHINode *Y : B)
        if (X->getParent() == Y->getParent() ||
            liveIn(X).count(Y->getParent()) ||
            liveIn(Y).count(X->getParent()))
          return true;
    return false;
  }

private:
  llvm::DenseMap<llvm::PHINode *, llvm::SmallPtrSet<llvm::BasicBlock *, 16>>
      LiveIn;
};

/// One copy on a CFG edge: store `V` into the local `Dst`. `ReadSlot` is
/// set when the copy reads a local at the copy site (the value of a phi
/// local, or a spill); the store then writes that load instead of `V`.
/// `Origin` is the phi the copy serves, for provenance.
struct EdgeCopy {
  llvm::AllocaInst *Dst;
  llvm::Value *V;
  llvm::AllocaInst *ReadSlot;
  llvm::Instruction *Origin = nullptr;
};

/// Order the parallel copies of one edge. Repeatedly takes the first copy
/// whose destination no other pending copy still reads, so copies without
/// a conflict keep their order. When every destination is still read (a
/// cycle), one destination's current value goes to `SpillFor(type)` first
/// and its readers read the spill instead.
inline llvm::SmallVector<EdgeCopy, 4>
sequenceEdgeCopies(llvm::SmallVector<EdgeCopy, 4> Pending,
                   llvm::function_ref<llvm::AllocaInst *(llvm::Type *)>
                       SpillFor) {
  llvm::SmallVector<EdgeCopy, 4> Ordered;
  while (!Pending.empty()) {
    size_t Safe = Pending.size();
    for (size_t I = 0; I < Pending.size() && Safe == Pending.size(); ++I) {
      bool Read = false;
      for (size_t J = 0; J < Pending.size() && !Read; ++J)
        Read = J != I && Pending[J].ReadSlot == Pending[I].Dst;
      if (!Read)
        Safe = I;
    }
    if (Safe == Pending.size()) {
      llvm::AllocaInst *Dst = Pending[0].Dst;
      llvm::AllocaInst *Spill = SpillFor(Dst->getAllocatedType());
      Ordered.push_back({Spill, nullptr, Dst, Pending[0].Origin});
      for (EdgeCopy &C : Pending)
        if (C.ReadSlot == Dst)
          C.ReadSlot = Spill;
      continue;
    }
    Ordered.push_back(Pending[Safe]);
    Pending.erase(Pending.begin() + Safe);
  }
  return Ordered;
}

} // namespace revenant

#endif
