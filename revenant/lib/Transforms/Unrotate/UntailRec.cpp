//===- UntailRec.cpp - tail-recursion reversal ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unrotate/UntailRec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

// A self tail-call block: the call plus any copy instructions we splice
// before the new backedge.
struct TailSite {
  BasicBlock *BB;
  CallInst *Call;
  SmallVector<Value *, 4> Args; // post-copy argument values
};

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
}

// Find blocks ending in an exact self tail-call. Mirrors the match in
// tail_rec.rs: `call @self(args)` immediately before `ret` (returning the
// call result, or void), or immediately before `unreachable`.
bool collectTails(Function &F, SmallVectorImpl<TailSite> &Tails) {
  for (BasicBlock &BB : F) {
    Instruction *Term = BB.getTerminator();
    if (!Term)
      continue;
    bool IsRet = isa<ReturnInst>(Term);
    bool IsUnreachable = isa<UnreachableInst>(Term);
    if (!IsRet && !IsUnreachable)
      continue;
    // The call must be the last non-debug instruction before the terminator.
    Instruction *Prev = Term->getPrevNode();
    while (Prev && Prev->isDebugOrPseudoInst())
      Prev = Prev->getPrevNode();
    auto *Call = dyn_cast_or_null<CallInst>(Prev);
    if (!Call || Call->getCalledFunction() != &F)
      continue;
    // Argument count must match the parameter count.
    if (Call->arg_size() != F.arg_size())
      continue;
    // Return types must agree (unopt compares ret_ty with f.ret).
    if (Call->getType() != F.getReturnType())
      continue;
    if (auto *Ret = dyn_cast<ReturnInst>(Term)) {
      Value *RV = Ret->getReturnValue();
      if (Call->getType()->isVoidTy()) {
        if (RV)
          continue;
      } else {
        if (RV != Call)
          continue; // result not returned directly: not a tail call
      }
    }
    // Nothing may sit between the call and the terminator.
    TailSite T{&BB, Call, {}};
    for (Value *A : Call->args())
      T.Args.push_back(A);
    Tails.push_back(std::move(T));
  }
  return !Tails.empty();
}

bool rewrite(Function &F, SmallVectorImpl<TailSite> &Tails) {
  LLVMContext &Ctx = F.getContext();

  // Collect every PHI destination name: an argument naming one would
  // collapse the phi web at coalescing (the swap pattern a<-b, b<-t merges
  // a and b), so route it through a copy. Mirrors tail_rec.rs.
  DenseSet<StringRef> PhiDsts;
  for (BasicBlock &BB : F)
    for (PHINode &PN : BB.phis())
      PhiDsts.insert(PN.getName());
  for (Argument &A : F.args())
    PhiDsts.insert(A.getName());

  // Insert copies before each tail call for args that name a PHI dst.
  for (TailSite &T : Tails) {
    IRBuilder<> B(T.Call);
    for (unsigned k = 0, e = T.Args.size(); k != e; ++k) {
      Value *A = T.Args[k];
      if (!isa<Instruction>(A) && !isa<Argument>(A))
        continue;
      if (!A->hasName() || !PhiDsts.count(A->getName()))
        continue;
      Type *Ty = (F.getArg(k))->getType();
      auto *Copy = B.CreateBitOrPointerCast(A, Ty, "u0tc");
      if (auto *CopyI = dyn_cast<Instruction>(Copy))
        copyVA(T.Call, CopyI);
      T.Args[k] = Copy;
    }
  }

  // New entry: `entry: br head`. The head takes over the old entry's name
  // with a `.lp` suffix so successors and phi predecessors stay valid.
  BasicBlock &OldEntry = F.getEntryBlock();
  std::string EntryName = OldEntry.getName().str();
  std::string HeadName = EntryName + ".lp";
  BasicBlock *Head =
      BasicBlock::Create(Ctx, HeadName, &F, OldEntry.getNextNode());

  // Move the old entry's instructions into the head; leave the old entry
  Instruction *OldTerm = OldEntry.getTerminator();
  SmallVector<Instruction *, 8> Body;
  for (Instruction &I : OldEntry)
    if (!I.isTerminator())
      Body.push_back(&I);
  for (Instruction *I : Body)
    I->moveBefore(*Head, Head->end());
  OldTerm->moveBefore(*Head, Head->end());

  // Rename the function's arguments p -> p.in (uses still say p; the new
  // PHIs will own those names). Argument names feed the debug info, so
  // update them in place.
  SmallVector<std::string, 4> OrigNames;
  for (Argument &A : F.args()) {
    OrigNames.push_back(A.getName().str());
    A.setName(A.getName() + ".in");
  }

  // Parameter PHIs at the head: [orig, entry] + each tail's args.
  for (unsigned k = 0, e = OrigNames.size(); k != e; ++k) {
    Argument *Orig = F.getArg(k);
    PHINode *PN = PHINode::Create(Orig->getType(), 1 + (unsigned)Tails.size(),
                                  OrigNames[k], &Head->front());
    PN->addIncoming(Orig, &OldEntry);
    for (TailSite &T : Tails)
      PN->addIncoming(T.Args[k], T.BB);
    // Replace uses of the renamed argument except the PHI's own incoming.
    SmallVector<Use *, 8> Uses;
    for (Use &U : Orig->uses())
      if (U.getUser() != PN)
        Uses.push_back(&U);
    for (Use *U : Uses)
      U->set(PN);
  }

  // Rewrite tail blocks: drop the terminator first (it uses the call),
  // then the call, keep spliced copies, `br head`.
  for (TailSite &T : Tails) {
    Instruction *OldT = T.BB->getTerminator();
    OldT->eraseFromParent();
    BranchInst *Back = BranchInst::Create(Head, T.BB);
    copyVA(T.Call, Back);
    T.Call->eraseFromParent();
  }

  // The old entry becomes `br head` (OldTerm already moved to the head).
  BranchInst::Create(Head, &OldEntry);
  return true;
}


PreservedAnalyses revenant::UntailRecPass::run(Function &F, FunctionAnalysisManager &) {
  SmallVector<TailSite, 4> Tails;
  if (!collectTails(F, Tails))
    return PreservedAnalyses::all();
  rewrite(F, Tails);
  return PreservedAnalyses::none();
}
