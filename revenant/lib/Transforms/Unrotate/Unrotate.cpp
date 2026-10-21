//===- Unrotate.cpp - undo loop rotation ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unrotate/Unrotate.h"

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

// Normalized operand expression, mirroring unopt's `Expr` (expr.rs): unfold
// a value through pure in-block defs so guard/latch conditions compare by
// structure instead of by SSA name.
struct Expr {
  enum class Kind { Bad, Const, Leaf, Op, Cmp } K = Kind::Bad;
  // Const: APInt value. Leaf: the opaque value itself (unnamed values have
  // no usable name). Op: (opcode, operands). Cmp: (predicate, lhs, rhs).
  APInt ConstVal{64, 0};
  const Value *LeafPtr = nullptr;
  unsigned Opcode = 0; // Instruction opcode for Op, CmpInst::Predicate for Cmp.
  std::vector<Expr> Kids;

  bool operator==(const Expr &O) const {
    if (K != O.K)
      return false;
    switch (K) {
    case Kind::Bad:
      return true;
    case Kind::Const:
      return ConstVal == O.ConstVal;
    case Kind::Leaf:
      return LeafPtr == O.LeafPtr;
    case Kind::Op:
      return Opcode == O.Opcode && Kids == O.Kids;
    case Kind::Cmp:
      return Opcode == O.Opcode && Kids == O.Kids;
    }
    return false;
  }
};

constexpr unsigned MaxDepth = 8;

bool isPureForChain(const Instruction &I) {
  // Mirrors unopt's `is_pure`: side-effect free, non-speculative value ops.
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
    return true;
  default:
    return false;
  }
}

using DefMap = DenseMap<const Value *, const Instruction *>;

// Fill `Ds` with value-defining instructions of block BB (phi through last).
void buildDefs(BasicBlock *BB, DefMap &Ds) {
  for (Instruction &I : *BB)
    if (!I.getType()->isVoidTy())
      Ds[&I] = &I;
  // Arguments and constants are leaves: no entry, same as unopt where
  // unknown names stay `Leaf`.
}

Expr exprOfImpl(const Value *V, const DefMap &Ds, unsigned Depth);

Expr exprOf(const Value *V, const DefMap &Ds) {
  return exprOfImpl(V, Ds, 0);
}

Expr makeLeaf(const Value *V) {
  Expr E;
  if (auto *C = dyn_cast<ConstantInt>(V)) {
    E.K = Expr::Kind::Const;
    E.ConstVal = C->getValue();
    return E;
  }
  E.K = Expr::Kind::Leaf;
  E.LeafPtr = V;
  return E;
}

Expr exprOfImpl(const Value *V, const DefMap &Ds, unsigned Depth) {
  if (Depth >= MaxDepth)
    return makeLeaf(V);
  if (isa<ConstantInt>(V))
    return makeLeaf(V);
  auto It = Ds.find(V);
  if (It == Ds.end())
    return makeLeaf(V);
  const Instruction *I = It->second;
  Expr E;
  if (auto *BO = dyn_cast<BinaryOperator>(I)) {
    // Constant-fold add/sub/mul of two constants like fold_bin does.
    if (auto *C0 = dyn_cast<ConstantInt>(BO->getOperand(0)))
      if (auto *C1 = dyn_cast<ConstantInt>(BO->getOperand(1))) {
        APInt R(64, 0);
        bool Folded = true;
        switch (BO->getOpcode()) {
        case Instruction::Add:
          R = C0->getValue() + C1->getValue();
          break;
        case Instruction::Sub:
          R = C0->getValue() - C1->getValue();
          break;
        case Instruction::Mul:
          R = C0->getValue() * C1->getValue();
          break;
        default:
          Folded = false;
          break;
        }
        if (Folded) {
          E.K = Expr::Kind::Const;
          E.ConstVal = R.sextOrTrunc(64);
          return E;
        }
      }
    E.K = Expr::Kind::Op;
    E.Opcode = BO->getOpcode();
    E.Kids.push_back(exprOfImpl(BO->getOperand(0), Ds, Depth + 1));
    E.Kids.push_back(exprOfImpl(BO->getOperand(1), Ds, Depth + 1));
    return E;
  }
  if (auto *CI = dyn_cast<CastInst>(I)) {
    E.K = Expr::Kind::Op;
    E.Opcode = CI->getOpcode();
    E.Kids.push_back(exprOfImpl(CI->getOperand(0), Ds, Depth + 1));
    return E;
  }
  if (auto *SI = dyn_cast<SelectInst>(I)) {
    E.K = Expr::Kind::Op;
    E.Opcode = SI->getOpcode();
    for (const Value *Op : SI->operands())
      E.Kids.push_back(exprOfImpl(Op, Ds, Depth + 1));
    return E;
  }
  if (auto *GI = dyn_cast<GetElementPtrInst>(I)) {
    E.K = Expr::Kind::Op;
    E.Opcode = GI->getOpcode();
    for (const Value *Op : GI->operands())
      E.Kids.push_back(exprOfImpl(Op, Ds, Depth + 1));
    return E;
  }
  if (auto *CI = dyn_cast<CmpInst>(I)) {
    E.K = Expr::Kind::Cmp;
    E.Opcode = CI->getPredicate();
    E.Kids.push_back(exprOfImpl(CI->getOperand(0), Ds, Depth + 1));
    E.Kids.push_back(exprOfImpl(CI->getOperand(1), Ds, Depth + 1));
    return E;
  }
  return makeLeaf(V);
}

// Rewrite leaves naming a PHI of block B to that PHI's `Pred`-incoming
// value's expression: the condition "as evaluated on first entry".
// Mirrors substitute_phi_inits in unrotate.rs.
bool substPhiInits(Expr &E, const DefMap &Ds, BasicBlock *Pred,
                   unsigned Depth) {
  if (Depth > MaxDepth)
    return false;
  if (E.K == Expr::Kind::Leaf) {
    auto It = Ds.find(E.LeafPtr);
    auto *PN = It == Ds.end() ? nullptr : dyn_cast<PHINode>(It->second);
    if (!PN)
      return true;
    const Value *Incoming = nullptr;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
      if (PN->getIncomingBlock(i) == Pred)
        Incoming = PN->getIncomingValue(i);
    if (!Incoming)
      return false;
    E = exprOfImpl(Incoming, Ds, 0);
    return substPhiInits(E, Ds, Pred, Depth + 1);
  }
  for (Expr &K : E.Kids)
    if (!substPhiInits(K, Ds, Pred, Depth + 1))
      return false;
  return true;
}

bool condAtInit(const Value *V, BasicBlock *B, BasicBlock *Pred, Expr &Out) {
  DefMap Ds;
  buildDefs(B, Ds);
  Out = exprOf(V, Ds);
  return substPhiInits(Out, Ds, Pred, 0);
}

// Defs needed to compute V inside B (pure instructions only), in RPO order.
// Mirrors cond_chain in unrotate.rs: phis are followed but not moved.
bool condChain(const Value *V, BasicBlock *B, DefMap &Ds,
               SmallVectorImpl<Instruction *> &Chain) {
  Ds.clear();
  buildDefs(B, Ds);
  SmallPtrSet<const Value *, 16> Seen;
  SmallVector<const Value *, 16> Stack{V};
  SmallPtrSet<Instruction *, 16> Need;
  while (!Stack.empty()) {
    const Value *Cur = Stack.pop_back_val();
    auto It = Ds.find(Cur);
    if (It == Ds.end())
      continue;
    Instruction *I = const_cast<Instruction *>(It->second);
    if (!Need.insert(I).second)
      continue;
    if (isa<PHINode>(I))
      continue;
    if (!isPureForChain(*I))
      return false;
    for (const Value *Op : I->operands()) {
      if (Seen.insert(Op).second)
        Stack.push_back(Op);
    }
  }
  for (Instruction &I : *B)
    if (Need.count(&I) && !isa<PHINode>(&I))
      Chain.push_back(&I);
  return true;
}

bool isInversePred(CmpInst::Predicate A, CmpInst::Predicate B) {
  using P = CmpInst::Predicate;
  switch (A) {
  case P::ICMP_EQ:
    return B == P::ICMP_NE;
  case P::ICMP_NE:
    return B == P::ICMP_EQ;
  case P::ICMP_SLT:
    return B == P::ICMP_SGE;
  case P::ICMP_SGE:
    return B == P::ICMP_SLT;
  case P::ICMP_SGT:
    return B == P::ICMP_SLE;
  case P::ICMP_SLE:
    return B == P::ICMP_SGT;
  case P::ICMP_ULT:
    return B == P::ICMP_UGE;
  case P::ICMP_UGE:
    return B == P::ICMP_ULT;
  case P::ICMP_UGT:
    return B == P::ICMP_ULE;
  case P::ICMP_ULE:
    return B == P::ICMP_UGT;
  default:
    return false;
  }
}

// Copy !revenant.va from Src to Dst when present.
void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
}

void copyVAToTerm(const Instruction *Src, Instruction *Dst) {
  copyVA(Src, Dst);
}

// Try to de-rotate the self-loop headed at L. Returns true on change.
bool tryBlock(Function &F, BasicBlock *L) {
  auto *LatchBr = dyn_cast<BranchInst>(L->getTerminator());
  if (!LatchBr || !LatchBr->isConditional())
    return false;
  Value *CondV = LatchBr->getCondition();
  bool LoopOnTrue = LatchBr->getSuccessor(0) == L;
  bool LoopOnFalse = LatchBr->getSuccessor(1) == L;
  if (!LoopOnTrue && !LoopOnFalse)
    return false;
  BasicBlock *Exit = LoopOnTrue ? LatchBr->getSuccessor(1)
                                : LatchBr->getSuccessor(0);

  // Unique non-self predecessor G ending in a conditional branch to {L, X}.
  BasicBlock *G = nullptr;
  for (BasicBlock *P : predecessors(L)) {
    if (P == L)
      continue;
    if (G)
      return false; // more than one entry edge
    G = P;
  }
  if (!G)
    return false;
  auto *GuardBr = dyn_cast<BranchInst>(G->getTerminator());
  if (!GuardBr || !GuardBr->isConditional())
    return false;
  if (GuardBr->getSuccessor(0) != L && GuardBr->getSuccessor(1) != L)
    return false;
  bool GuardToLoopOnTrue = GuardBr->getSuccessor(0) == L;
  BasicBlock *GuardExit = GuardToLoopOnTrue ? GuardBr->getSuccessor(1)
                                            : GuardBr->getSuccessor(0);
  if (GuardExit != Exit)
    return false;
  // After the rewrite G no longer branches to X, so every X phi would keep
  // a stale G entry, and its L entry would have to equal the G value
  // whenever the guard skips the loop, which nothing here proves. Body
  // values can only leave the loop through X phis (L does not dominate X),
  // so an X without phis also means no live-outs to remap.
  if (!Exit->phis().empty())
    return false;

  // Prove guard == latch-at-init (same edge side) or guard == !latch-at-init.
  bool SameSide = GuardToLoopOnTrue == LoopOnTrue;
  Expr EG, EC;
  if (!condAtInit(GuardBr->getCondition(), G, nullptr, EG))
    return false;
  if (!condAtInit(CondV, L, G, EC))
    return false;
  bool Eq = false;
  if (SameSide) {
    Eq = EG == EC;
  } else if (EG.K == Expr::Kind::Cmp && EC.K == Expr::Kind::Cmp) {
    Eq = isInversePred((CmpInst::Predicate)EG.Opcode,
                       (CmpInst::Predicate)EC.Opcode) &&
         EG.Kids == EC.Kids;
  }
  if (!Eq)
    return false;

  // The latch condition chain must be pure.
  DefMap Ds;
  SmallVector<Instruction *, 8> Chain;
  if (!condChain(CondV, L, Ds, Chain))
    return false;
  SmallPtrSet<Instruction *, 8> ChainSet(Chain.begin(), Chain.end());

  // Split L: T keeps the phis plus the cond chain plus the latch branch,
  // B keeps the rest of the body and branches back to T. T takes L's name
  // so successors' phi predecessors stay valid; B is new.
  std::string LName = L->getName().str();
  BasicBlock *B =
      BasicBlock::Create(F.getContext(), LName + ".cond", &F, L->getNextNode());
  SmallVector<Instruction *, 8> MoveToB;
  for (Instruction &I : *L)
    if (!isa<PHINode>(&I) && !ChainSet.count(&I) && &I != LatchBr)
      MoveToB.push_back(&I);
  for (Instruction *I : MoveToB)
    I->moveBefore(*B, B->end());

  BranchInst *Back = BranchInst::Create(L, B);
  copyVAToTerm(LatchBr, Back);
  // Fix the latch branch: its loop edge now targets B (the body).
  auto *NewLatch = cast<BranchInst>(L->getTerminator());
  for (unsigned i = 0; i < 2; ++i)
    if (NewLatch->getSuccessor(i) == L)
      NewLatch->setSuccessor(i, B);
  // Fix PHI backedge predecessors: they named L (the body before the
  // split); the backedge now comes from B.
  for (Instruction &I : *L) {
    auto *PN = dyn_cast<PHINode>(&I);
    if (!PN)
      break;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
      if (PN->getIncomingBlock(i) == L)
        PN->setIncomingBlock(i, B);
  }
  // G jumps straight to the loop header.
  BranchInst *GotoT = BranchInst::Create(L, G);
  copyVAToTerm(GuardBr, GotoT);
  GuardBr->eraseFromParent();
  return true;
}

} // namespace

PreservedAnalyses revenant::UnrotatePass::run(Function &F, FunctionAnalysisManager &) {
  bool Changed = false;
  // Snapshot blocks: the rewrite inserts one block per latch.
  SmallVector<BasicBlock *, 16> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);
  for (BasicBlock *L : Blocks)
    Changed |= tryBlock(F, L);
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
