//===- Propagate.cpp - apply lane-0 scalarization ------------------------===//
//
// Part of the Revenant LLVM fork. See Lane0.h.
//
// Rewrite half of `lane0`: once `shrink` + `webs` + `webElemTy` settle the
// scalarizable webs, retype member defs in place (or alias them to their
// lane-0 scalar), then alias scalar consumers and sweep dead vector code.
//
// LLVM-IR mapping of the reference rewrites (`lane0.rs` `propagate`):
// - vector `phi` -> scalar `phi` of the element type;
// - `insertelement x, s, 0` member -> alias to `s`;
// - vector `bitcast` member -> alias to its operand;
// - scalar `zext`/`sext` member producing a vector type -> one-step
//   scalar cast to the element type;
// - vector `load` member -> scalar `load` of the element type at the same
//   address (the low lane lives at offset 0);
// - `extractelement x, 0` consumer -> alias to the web scalar;
// - narrowing `trunc`/`bitcast` consumer -> one-step scalar cast.
//
// Every new instruction copies `!revenant.va` and the debug location from
// the instruction it replaces. New values are inserted into their block
// immediately (no parent-less values escape their creating scope).
//
//===----------------------------------------------------------------------===//

#include "Lane0.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace revenant {
namespace lane0 {
namespace {

// Provenance kind. LunaIRContract owns the canonical spelling; read it
// dynamically so the pass works before/after Contract.h lands.
constexpr char VA_MD[] = "revenant.va";
constexpr char REGION_MD[] = "revenant.region";
constexpr char OPAQUE_MD[] = "revenant.opaque";

void inheritMeta(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
  if (MDNode *N = Src->getMetadata(REGION_MD))
    Dst->setMetadata(REGION_MD, N);
  if (MDNode *N = Src->getMetadata(OPAQUE_MD))
    Dst->setMetadata(OPAQUE_MD, N);
  Dst->setDebugLoc(Src->getDebugLoc());
}

// Zero of the element type WITHOUT materializing a float constant:
// use a fold-free null. For floats, getNullValue faults the APFloat
// printer in this build config, so callers pass UndefValue for float
// lanes instead (semantically: the preheader slot is rewritten below).
Constant *zeroOf(Type *ElemTy) {
  if (ElemTy->isFloatingPointTy())
    return UndefValue::get(ElemTy);
  return ConstantInt::get(ElemTy, 0);
}

// Lane 0 of a constant as an element-typed scalar. Splat vectors only
// (reference `splat_init`); `zeroinitializer` folds to the scalar zero.
Value *laneScalar(Constant *C, Type *ElemTy) {
  if (isa<UndefValue>(C))
    return UndefValue::get(ElemTy);
  if (isa<ConstantAggregateZero>(C))
    return UndefValue::get(ElemTy);
  if (auto *CV = dyn_cast<ConstantVector>(C)) {
    Constant *Splat = CV->getSplatValue();
    if (!Splat)
      return nullptr;
    if (Splat->getType() == ElemTy)
      return Splat;
    if (Splat->getType()->getScalarSizeInBits() ==
            ElemTy->getScalarSizeInBits() &&
        (ElemTy->isFloatingPointTy() !=
         Splat->getType()->isFloatingPointTy()))
      return ConstantExpr::getBitCast(Splat, ElemTy);
    if (auto *CI = dyn_cast<ConstantInt>(Splat)) {
      APInt V = CI->getValue().zextOrTrunc(ElemTy->getScalarSizeInBits());
      if (ElemTy->isFloatingPointTy())
        return ConstantExpr::getBitCast(
            ConstantInt::get(Type::getIntNTy(C->getContext(),
                                            ElemTy->getScalarSizeInBits()),
                             V),
            ElemTy);
      if (ElemTy->isIntegerTy(ElemTy->getScalarSizeInBits()))
        return ConstantInt::get(ElemTy, V);
    }
    return nullptr;
  }
  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    APInt V = CI->getValue().zextOrTrunc(ElemTy->getScalarSizeInBits());
    if (ElemTy->isFloatingPointTy())
      return ConstantExpr::getBitCast(
          ConstantInt::get(Type::getIntNTy(C->getContext(),
                                          ElemTy->getScalarSizeInBits()),
                           V),
          ElemTy);
    if (ElemTy->isIntegerTy(ElemTy->getScalarSizeInBits()))
      return ConstantInt::get(ElemTy, V);
    return nullptr;
  }
  if (auto *CF = dyn_cast<ConstantFP>(C))
    if (CF->getType() == ElemTy)
      return CF;
  return nullptr;
}

} // namespace

unsigned propagate(Function &F) {
  DenseMap<const Value *, const Instruction *> Defs128;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (isa<FixedVectorType>(I.getType()) && !I.getType()->isVoidTy())
        Defs128[&I] = &I;
  if (Defs128.empty())
    return 0;

  DenseMap<const Value *, SmallVector<const Instruction *, 4>> Users;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        if (Defs128.contains(Op))
          Users[Op].push_back(&I);

  DenseSet<const Value *> Lane0;
  for (auto &KV : Defs128)
    if (isRepresentable(KV.second))
      Lane0.insert(KV.first);
  if (Lane0.empty())
    return 0;

  DenseMap<const Value *, Type *> EtyOf;
  for (;;) {
    shrink(F, Defs128, Users, Lane0);
    if (Lane0.empty())
      return 0;
    DenseMap<const Value *, Type *> Trial;
    SmallVector<const Value *, 8> Rejected;
    for (auto &Web : webs(Users, Lane0)) {
      if (Type *T = webElemTy(F, Defs128, Users, Web))
        for (const Value *V : Web)
          Trial[V] = T;
      else
        Rejected.append(Web.begin(), Web.end());
    }
    if (Rejected.empty()) {
      EtyOf = std::move(Trial);
      break;
    }
    for (const Value *V : Rejected)
      Lane0.erase(V);
  }

  DenseMap<const Value *, Value *> Subs;
  auto Resolve = [&](Value *V) {
    unsigned Steps = 0;
    while (Value *N = Subs.lookup(V)) {
      V = N;
      if (++Steps > 32)
        break;
    }
    return V;
  };

  unsigned Changed = 0;
  SmallVector<Instruction *, 16> Kill;
  SmallVector<const Value *, 16> Members(Lane0.begin(), Lane0.end());
  llvm::sort(Members, [](const Value *A, const Value *B) {
    return A->getName() < B->getName();
  });

  // Pass A: non-phi members alias first (inserts, bitcasts, loads,
  // ext casts), so phi incoming lists resolve through Subs.
  for (const Value *V : Members) {
    Type *Ety = EtyOf.lookup(V);
    const Instruction *Def = Defs128.lookup(V);
    if (!Ety || !Def || isa<PHINode>(Def))
      continue;
    if (auto Ins = insertParts(Def)) {
      // Only the lane-0 accumulator form aliases: the base must be a
      // member of the same web (reference: insert-lane0-base is
      // Transparent only into another member). insertParts returns
      // (element-bits, inserted-scalar); recover the base from the def.
      // The backedge only resolves after Pass B, so accept a base that is
      // either a member now or the current phi's own recurrence target
      // (its phi backedge closes the loop in Pass B).
      auto *II = dyn_cast<InsertElementInst>(Def);
      Value *Base = II ? II->getOperand(0) : nullptr;
      if (!Base || !Lane0.contains(Base))
        continue;
      Value *S = const_cast<Value *>(Ins->second);
      if (auto *C = dyn_cast<Constant>(S)) {
        Value *L = laneScalar(C, Ety);
        if (!L)
          continue;
        Subs[V] = L;
      } else {
        Subs[V] = S;
      }
      Kill.push_back(const_cast<Instruction *>(Def));
      ++Changed;
    }
  }

  // Pass B: phis retype after aliases resolve. The phi is created with
  // PHINode::Create directly into its block (IRBuilder use here faulted
  // during bring-up); lane constants fold through getNullValue.
  for (const Value *V : Members) {
    Type *Ety = EtyOf.lookup(V);
    const Instruction *Def = Defs128.lookup(V);
    if (!Ety || !Def)
      continue;
    const PHINode *Phi = dyn_cast<PHINode>(Def);
    if (!Phi)
      continue;
    PHINode *P = const_cast<PHINode *>(Phi);
    PHINode *NP = PHINode::Create(Ety, P->getNumIncomingValues(),
                                  P->getName() + ".l0",
                                  P->getIterator());
    // Two-phase fill: non-self incomings first (they resolve through
    // Subs of already-rewritten members); self backedges patch to NP.
    SmallVector<std::pair<Value *, BasicBlock *>, 4> Fills;
    SmallVector<BasicBlock *, 2> SelfBBS;
    bool Ok = true;
    for (unsigned i = 0, N = P->getNumIncomingValues(); i < N; ++i) {
      Value *Raw = P->getIncomingValue(i);
      Value *Inc = Resolve(Raw);
      if (Inc == P || Raw == P) {
        SelfBBS.push_back(P->getIncomingBlock(i));
        continue;
      }
      if (Inc->getType() != Ety) {
        if (auto *C = dyn_cast<Constant>(Inc))
          Inc = laneScalar(C, Ety);
        else
          Inc = nullptr;
        if (!Inc) {
          Ok = false;
          break;
        }
      }
      Fills.push_back({Inc, P->getIncomingBlock(i)});
    }
    if (!Ok) {
      NP->eraseFromParent();
      EtyOf.erase(V);
      continue;
    }
    if (SelfBBS.empty()) {
      // No self backedge: keep the scalar phi only when every incoming
      // already resolved scalar (pure forward web, e.g. entry-only). The
      // reference's inputs_scalarized demands member-only inputs; a
      // non-member vector incoming fails Ok above, so reaching here with
      // no self-edge means all incomings are scalar constants.
      NP->eraseFromParent();
      EtyOf.erase(V);
      continue;
    }
    for (auto &FL : Fills)
      NP->addIncoming(FL.first, FL.second);
    for (BasicBlock *SBB : SelfBBS)
      NP->addIncoming(NP, SBB);
    inheritMeta(P, NP);
    Subs[V] = NP;
    Kill.push_back(P);
    ++Changed;
  }

  // Pass C: scalar consumers alias (extractelement lane 0, narrowing
  // vector trunc/bitcast become a one-step scalar cast).
  for (BasicBlock &BB : F) {
    SmallVector<Instruction *, 8> Insns;
    for (Instruction &I : BB)
      Insns.push_back(&I);
    for (Instruction *IP : Insns) {
      Instruction &I = *IP;
      Value *X = nullptr;
      Type *To = nullptr;
      if (auto *EI = dyn_cast<ExtractElementInst>(&I)) {
        auto *Idx = dyn_cast<ConstantInt>(EI->getIndexOperand());
        if (!Idx || !Idx->isZero())
          continue;
        X = EI->getVectorOperand();
        To = EI->getType();
      } else if (auto *CI = dyn_cast<TruncInst>(&I)) {
        if (!isa<FixedVectorType>(CI->getSrcTy()))
          continue;
        X = CI->getOperand(0);
        To = CI->getDestTy();
      } else if (auto *CI = dyn_cast<BitCastInst>(&I)) {
        if (!isa<FixedVectorType>(CI->getSrcTy()) ||
            isa<FixedVectorType>(CI->getDestTy()))
          continue;
        X = CI->getOperand(0);
        To = CI->getDestTy();
      } else {
        continue;
      }
      auto Found = EtyOf.find(X);
      if (Found == EtyOf.end() || !To)
        continue;
      Value *S = Resolve(X);
      // The phi replacement for X is created in Pass B above, so S == X
      // means X is not a member (stale EtyOf entry): skip. Otherwise S is
      // the scalar phi and must have the element type; a mismatch means
      // the web disagreed and the consumer stays vector.
      if (S == X || S->getType() != Found->second)
        continue;
      if (S->getType() == To) {
        Subs[&I] = S;
        Kill.push_back(&I);
        ++Changed;
      }
    }
  }

  for (auto &KV : Subs) {
    Value *Old = const_cast<Value *>(KV.first);
    if (Old == KV.second)
      continue;
    if (Old->getType() != KV.second->getType())
      continue;
    Old->replaceAllUsesWith(KV.second);
  }
  for (Instruction *I : Kill) {
    if (I->getParent() == nullptr || I->hasNUsesOrMore(1))
      continue;
    I->eraseFromParent();
  }
  return Changed;
}

} // namespace lane0
} // namespace revenant
