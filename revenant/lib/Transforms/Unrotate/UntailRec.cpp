//===- UntailRec.cpp - tail-call elimination reversal ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Undoes optimizer tail-call elimination on lifted code: a loop produced by
// LLVM's TailCallElim (and the MSVC/GCC equivalents) becomes the recursive
// call the source had. This is the IR-1:1 direction: compiler pre-opt IR
// (`artifacts/*.pre.ll`, kind `ir-pre`) keeps exact recursive calls
// (`@tail_gcd`, `@tail_count` in the corpus C target), while O2 binaries
// lift with the loop already in place.
//
// Fires only on the exact TailCallElim shape:
//
//   entry: <cond> ; br cond, exit, header     -- entry split, header is the
//   header: p = phi [arg, entry], [next, latch] ...  -- one phi per argument
//           ... body computing next ...
//           br cond2, exit2, latch               -- latch branches back
//   latch: ... next ... ; br header            -- backedge into the header
//
// The header PHIs must cover every function argument exactly once (one phi
// per argument, in any order); the latch must feed every header phi and do
// nothing else observable: pure value ops (binop/cmp/cast/select/gep) plus
// the backedge branch. The exit path returns directly as before.
//
// Rewritten shape (mirrors the clang -O0 form):
//
//   entry: <cond> ; br cond, exit, rec
//   rec: r = call @self(next...); ret r       -- or void ret
//
// The latch block becomes the call block (moved defs first, then the call,
// so use-def chains stay valid); the old header is deleted. Needed header
// body defs move into the call block with header phis replaced by their
// entry values; leftover header-value uses read the call result.
//
// Guarantees: only fires on the exact shape above; never turns an existing
// recursive call into a loop (self-calls anywhere in the function veto the
// rewrite); preserves !revenant.va on the call by carrying the latch's VA;
// never touches !revenant.opaque calls.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unrotate/UntailRec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace {

// Candidate loop: entry E branching to header H and some exit, header H
// with per-argument PHIs, latch L branching back to H.
struct LoopShape {
  BasicBlock *Entry = nullptr;
  BasicBlock *Header = nullptr;
  BasicBlock *Latch = nullptr;
  // Header phi per argument index: PhiForArg[k] is the phi fed by arg k.
  SmallVector<PHINode *, 4> PhiForArg;
  // Latch incoming value per argument index.
  SmallVector<Value *, 4> NextForArg;
};

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
}

// An existing direct self-call anywhere vetoes the rewrite: this pass must
// never turn a recursive function into a loop.
bool hasSelfCall(const Function &F) {
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      if (const auto *Call = dyn_cast<CallInst>(&I))
        if (Call->getCalledFunction() == &F)
          return true;
      if (const auto *Invoke = dyn_cast<InvokeInst>(&I))
        if (Invoke->getCalledFunction() == &F)
          return true;
    }
  return false;
}

bool isOpaqueCall(const Instruction *I) {
  return I->hasMetadata("revenant.opaque");
}

bool isPureForLatch(const Instruction &I) {
  if (isOpaqueCall(&I))
    return false;
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::ICmp:
  case Instruction::FCmp:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
  case Instruction::Select:
  case Instruction::GetElementPtr:
  case Instruction::PHI:
    return true;
  default:
    return false;
  }
}

// Match the TailCallElim loop shape. Returns true and fills S on success.
bool matchLoop(Function &F, LoopShape &S) {
  unsigned NumArgs = F.arg_size();
  if (NumArgs == 0)
    return false;

  // The entry block must end in a conditional branch (the split guard).
  BasicBlock &E = F.getEntryBlock();
  auto *Guard = dyn_cast<BranchInst>(E.getTerminator());
  if (!Guard || !Guard->isConditional())
    return false;

  // Try each successor as the loop header.
  for (unsigned h = 0; h < 2; ++h) {
    BasicBlock *H = Guard->getSuccessor(h);
    if (H == &E)
      continue;

    // Header PHIs must cover every argument exactly once.
    SmallVector<PHINode *, 4> PhiForArg(NumArgs, nullptr);
    bool Bad = false;
    for (PHINode &PN : H->phis()) {
      unsigned Found = NumArgs;
      unsigned Count = 0;
      for (unsigned k = 0; k < NumArgs; ++k) {
        if (F.getArg(k)->getType() != PN.getType())
          continue;
        // The entry incoming must be the argument itself.
        for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i) {
          if (PN.getIncomingBlock(i) == &E &&
              PN.getIncomingValue(i) == F.getArg(k)) {
            Found = k;
            ++Count;
          }
        }
      }
      if (Count != 1 || Found == NumArgs || PhiForArg[Found]) {
        Bad = true;
        break;
      }
      PhiForArg[Found] = &PN;
    }
    if (Bad)
      continue;
    for (PHINode *PN : PhiForArg)
      if (!PN) {
        Bad = true;
        break;
      }
    if (Bad)
      continue;
    // Every header phi must have exactly two incomings: entry + one latch.
    BasicBlock *Latch = nullptr;
    SmallVector<Value *, 4> NextForArg(NumArgs, nullptr);
    for (unsigned k = 0; k < NumArgs; ++k) {
      PHINode *PN = PhiForArg[k];
      if (PN->getNumIncomingValues() != 2) {
        Bad = true;
        break;
      }
      for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
        BasicBlock *Pred = PN->getIncomingBlock(i);
        if (Pred == &E)
          continue;
        if (Latch && Pred != Latch) {
          Bad = true;
          break;
        }
        Latch = Pred;
        NextForArg[k] = PN->getIncomingValue(i);
      }
      if (Bad)
        break;
    }
    if (Bad || !Latch || Latch == &E)
      continue;
    // A self-loop header (Latch == H, the shape clang -O2 leaves for
    // @tail_gcd: one block, phis fed by its own body) is NOT matched:
    // splitting the header's own body into latch-vs-exit needs loop info
    // this peephole does not have. Only the split TailCallElim shape with
    // a dedicated latch block fires.
    if (Latch == H)
      continue;
    for (Value *V : NextForArg)
      if (!V) {
        Bad = true;
        break;
      }
    if (Bad)
      continue;

    // The latch must branch unconditionally back to the header and contain
    // only pure value ops (plus phis) besides the branch. Anything with
    // side effects (stores, loads, calls, ...) vetoes.
    auto *Back = dyn_cast<BranchInst>(Latch->getTerminator());
    if (!Back || Back->isConditional() || Back->getSuccessor(0) != H)
      continue;
    bool Pure = true;
    for (Instruction &I : *Latch) {
      if (I.isTerminator())
        continue;
      if (isa<DbgInfoIntrinsic>(&I))
        continue;
      if (!isPureForLatch(I)) {
        Pure = false;
        break;
      }
      if (isa<PHINode>(&I)) {
        Pure = false;
        break;
      }
    }
    if (!Pure)
      continue;

    // The latch's next-values must be defined in the header or the latch
    // (the loop-carried computation), not elsewhere.
    for (Value *V : NextForArg) {
      auto *I = dyn_cast<Instruction>(V);
      if (!I)
        continue; // constants and arguments are fine
      if (I->getParent() != H && I->getParent() != Latch) {
        Pure = false;
        break;
      }
    }
    if (!Pure)
      continue;

    S.Entry = &E;
    S.Header = H;
    S.Latch = Latch;
    S.PhiForArg = std::move(PhiForArg);
    S.NextForArg = std::move(NextForArg);
    return true;
  }
  return false;
}

bool rewrite(Function &F, const LoopShape &S) {
  // The latch block becomes the recursive call block: move the needed
  // header body defs first, then append `r = call @self(next...); ret r`
  // (or the void form) after them, so use-def chains inside L stay valid.
  // Then route the entry guard's header edge to L and delete the header.
  BasicBlock *L = S.Latch;
  BasicBlock *H = S.Header;
  L->getTerminator()->eraseFromParent();

  // Carry provenance: prefer the latch's last value op.
  const Instruction *VASrc = nullptr;
  for (Instruction &I : llvm::reverse(*L)) {
    if (!I.isTerminator() && !I.isDebugOrPseudoInst() &&
        !I.getType()->isVoidTy()) {
      VASrc = &I;
      break;
    }
  }

  SmallVector<Value *, 4> Args(S.NextForArg.begin(), S.NextForArg.end());
  Type *RetTy = F.getReturnType();

  // Collect the header body defs the call transitively needs, plus every
  // header value the latch code reads: the latch stays in L, so its
  // header-phi operands must be rewritten to entry values as well. Header
  // PHIs are replaced by their entry values (the loop is entered once).
  SmallPtrSet<Instruction *, 16> Moved;
  SmallVector<Value *, 16> Work(Args.begin(), Args.end());
  for (Instruction &LI : *L)
    for (Value *Op : LI.operands())
      Work.push_back(Op);
  SmallVector<Instruction *, 16> ToMove;
  DenseMap<Instruction *, Value *> PhiRepl;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    auto *I = dyn_cast<Instruction>(V);
    if (!I || I->getParent() != H || Moved.count(I))
      continue;
    if (auto *PN = dyn_cast<PHINode>(I)) {
      Value *Repl = PN->getIncomingValueForBlock(S.Entry);
      PhiRepl[I] = Repl;
      Work.push_back(Repl);
      continue;
    }
    Moved.insert(I);
    ToMove.push_back(I);
    for (Value *Op : I->operands())
      Work.push_back(Op);
  }
  // Insert moved defs before the first non-phi of L (keep L's own order).
  Instruction *Anchor = L->getFirstNonPHI();
  for (Instruction *I : llvm::reverse(ToMove))
    I->moveBefore(*L, Anchor->getIterator());
  // Rewrite moved operands: header phis -> entry values.
  SmallVector<Instruction *, 16> Fix;
  for (Instruction &FI : *L)
    Fix.push_back(&FI);
  for (Instruction *I : Fix)
    for (unsigned o = 0, e = I->getNumOperands(); o != e; ++o) {
      Value *V = I->getOperand(o);
      auto It = PhiRepl.find(dyn_cast<Instruction>(V));
      if (It != PhiRepl.end())
        I->setOperand(o, It->second);
    }
  for (Value *&A : Args) {
    auto It = PhiRepl.find(dyn_cast<Instruction>(A));
    if (It != PhiRepl.end())
      A = It->second;
  }

  // Append the call + ret after the (fixed) latch code.
  IRBuilder<> BC(L);
  CallInst *Call = BC.CreateCall(&F, Args);
  if (VASrc)
    copyVA(VASrc, Call);
  if (RetTy->isVoidTy())
    BC.CreateRetVoid();
  else
    BC.CreateRet(Call);

  // Route the entry guard's header edge to the call block.
  auto *Guard = cast<BranchInst>(S.Entry->getTerminator());
  for (unsigned i = 0; i < 2; ++i)
    if (Guard->getSuccessor(i) == H)
      Guard->setSuccessor(i, L);

  // Every remaining use of a header-defined value outside L reads the
  // recursion result: the call returns exactly what the loop would have
  // computed. Uses inside L are already valid (moved defs dominate, phis
  // rewritten to entry values above) and must NOT be touched. Void
  // functions have no result to forward, so undef outside uses out (those
  // blocks are unreachable after the guard reroute and cleanup removes
  // them).
  SmallVector<Instruction *, 16> HeaderDefs;
  for (Instruction &I : *H)
    HeaderDefs.push_back(&I);
  for (Instruction *D : HeaderDefs) {
    if (D->getType()->isVoidTy() || Moved.count(D))
      continue;
    SmallVector<Use *, 8> Outside;
    for (Use &U : D->uses()) {
      auto *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI || UI->getParent() != L)
        Outside.push_back(&U);
    }
    if (Outside.empty())
      continue;
    if (RetTy->isVoidTy()) {
      for (Use *U : Outside)
        U->set(llvm::UndefValue::get(D->getType()));
      continue;
    }
    for (Use *U : Outside)
      U->set(Call);
  }

  // Fix PHIs in other blocks (exits) that reference the header block.
  for (BasicBlock &BB : F)
    for (PHINode &PN : BB.phis()) {
      bool HasH = false;
      for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i)
        if (PN.getIncomingBlock(i) == H)
          HasH = true;
      if (HasH)
        PN.replaceIncomingBlockWith(H, L);
    }

  // The header is now unreachable: drop its references and erase it.
  H->dropAllReferences();
  H->eraseFromParent();

  return true;
}

} // namespace

PreservedAnalyses revenant::UntailRecPass::run(Function &F,
                                               FunctionAnalysisManager &) {
  // Never turn an existing recursive call into a loop.
  if (hasSelfCall(F))
    return PreservedAnalyses::all();
  LoopShape S;
  if (!matchLoop(F, S))
    return PreservedAnalyses::all();
  rewrite(F, S);
  return PreservedAnalyses::none();
}
