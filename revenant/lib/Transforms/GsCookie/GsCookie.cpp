//===- GsCookie.cpp - remove the MSVC /GS stack cookie sequence -------===//
//
// Part of the Revenant LLVM fork. See GsCookie.h for the contract.
//
// The pass removes three pieces of the /GS sequence:
//
//   * the prologue store of `cookie ^ stack pointer` into the frame,
//   * the epilogue compare of the reloaded cookie against the global,
//   * the `__security_check_cookie` call on the failure edge.
//
// The failure edge is folded into the success edge: the check calls
// `__report_gsfailure`, which aborts, so the edge never returns. The
// branch keeps its provenance metadata (`!revenant.va`). The failure
// block goes once nothing reaches it, and the success block joins the
// block of the check, as in the compiler's IR, which never split there.
//
// The instructions that only existed for the cookie (the xor, the
// compare, the loads of the cookie and of the frame slot) are removed
// with it, so the function reads like the compiler's own IR, which has
// no cookie at all.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/GsCookie/GsCookie.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

/// The attribute the pass sets on a function whose cookie it removed.
constexpr char GsCookieAttr[] = "rv.gscookie";

/// The stack pointer value of the cookie sequence: the ptrtoint of a
/// frame slot, or an arithmetic adjustment of one. A phi is never
/// followed, so the walk cannot cycle.
bool isStackPointerValue(Value *V) {
  if (isa<PtrToIntInst>(V))
    return true;
  auto *BO = dyn_cast<BinaryOperator>(V);
  if (!BO)
    return false;
  if (BO->getOpcode() != Instruction::Add &&
      BO->getOpcode() != Instruction::Sub)
    return false;
  return isStackPointerValue(BO->getOperand(0)) ||
         isStackPointerValue(BO->getOperand(1));
}

/// Delete an instruction and, recursively, the instructions that only
/// existed to feed it. Stores and terminators are never deleted this
/// way: a store has no users but a side effect.
void eraseIfDead(Instruction *I) {
  SmallVector<Instruction *, 8> Work;
  Work.push_back(I);
  while (!Work.empty()) {
    Instruction *Cur = Work.pop_back_val();
    if (!Cur->use_empty() || Cur->isTerminator() || isa<PHINode>(Cur))
      continue;
    SmallVector<Value *, 4> Operands(Cur->value_op_begin(), Cur->value_op_end());
    Cur->eraseFromParent();
    for (Value *Op : Operands)
      if (auto *OpI = dyn_cast<Instruction>(Op))
        if (!isa<PHINode>(OpI))
          Work.push_back(OpI);
  }
}

/// One /GS cookie sequence inside a function.
struct CookieSite {
  BinaryOperator *CheckXor = nullptr; ///< `stored ^ stack pointer`
  BinaryOperator *SetupXor = nullptr;  ///< `cookie ^ stack pointer`
  StoreInst *SetupStore = nullptr;     ///< the prologue store
  CallBase *CheckCall = nullptr;       ///< `__security_check_cookie`
  Value *CheckArg = nullptr;           ///< its argument (`stored ^ sp` again)
  ICmpInst *Cmp = nullptr;             ///< the cookie compare
  BranchInst *Br = nullptr;            ///< the branch on the compare
  BasicBlock *FailBB = nullptr;        ///< the block with the check call
  Value *SP = nullptr;                 ///< the stack pointer value
};

/// Find the cookie sequence of a function, if it has one.
bool findCookieSite(Function &F, CookieSite &S) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Xor = dyn_cast<BinaryOperator>(&I);
      if (!Xor || Xor->getOpcode() != Instruction::Xor)
        continue;
      if (!Xor->getType()->isIntegerTy())
        continue;
      Value *SP = nullptr;
      for (Value *Op : Xor->operands())
        if (isStackPointerValue(Op)) {
          SP = Op;
          break;
        }
      if (!SP)
        continue;
      // The epilogue xor feeds a compare that feeds a conditional
      // branch; one of the branch's successors calls a function with
      // the xor as its only argument.
      ICmpInst *Cmp = nullptr;
      for (User *U : Xor->users())
        if (auto *C = dyn_cast<ICmpInst>(U)) {
          Cmp = C;
          break;
        }
      if (!Cmp)
        continue;
      BranchInst *Br = nullptr;
      for (User *U : Cmp->users())
        if (auto *B = dyn_cast<BranchInst>(U))
          if (B->isConditional()) {
            Br = B;
            break;
          }
      if (!Br)
        continue;
      // The failure block passes the same xor, or (an unoptimized frame
      // reloads the cookie slot there) another xor of the stack pointer.
      auto IsCookieXor = [&](Value *V) {
        if (V == Xor)
          return true;
        auto *X = dyn_cast<BinaryOperator>(V);
        return X && X->getOpcode() == Instruction::Xor &&
               (isStackPointerValue(X->getOperand(0)) ||
                isStackPointerValue(X->getOperand(1)));
      };
      CallBase *Call = nullptr;
      BasicBlock *Fail = nullptr;
      for (BasicBlock *Succ : Br->successors()) {
        for (Instruction &SI : *Succ) {
          auto *CB = dyn_cast<CallBase>(&SI);
          if (!CB || CB->arg_size() != 1)
            continue;
          if (!IsCookieXor(CB->getArgOperand(0)))
            continue;
          Call = CB;
          Fail = Succ;
        }
      }
      if (!Call)
        continue;
      if (Br->getSuccessor(0) == Br->getSuccessor(1))
        continue;
      // The prologue: an xor with the same stack pointer value that is
      // stored into the frame.
      StoreInst *SetupStore = nullptr;
      BinaryOperator *SetupXor = nullptr;
      for (BasicBlock &B2 : F) {
        for (Instruction &I2 : B2) {
          auto *St = dyn_cast<StoreInst>(&I2);
          if (!St)
            continue;
          auto *SX = dyn_cast<BinaryOperator>(St->getValueOperand());
          if (!SX || SX->getOpcode() != Instruction::Xor || SX == Xor)
            continue;
          if (SX->getOperand(0) != SP && SX->getOperand(1) != SP)
            continue;
          SetupStore = St;
          SetupXor = SX;
        }
      }
      S.CheckXor = Xor;
      S.SetupXor = SetupXor;
      S.SetupStore = SetupStore;
      S.CheckCall = Call;
      S.CheckArg = Call->getArgOperand(0);
      S.Cmp = Cmp;
      S.Br = Br;
      S.FailBB = Fail;
      S.SP = SP;
      return true;
    }
  }
  return false;
}

/// Remove the cookie sequence a site describes.
bool removeCookie(CookieSite &S, unsigned &Changed) {
  BasicBlock *Succ0 = S.Br->getSuccessor(0);
  BasicBlock *Succ1 = S.Br->getSuccessor(1);
  BasicBlock *OkBB = (S.FailBB == Succ0) ? Succ1 : Succ0;
  if (OkBB == S.FailBB)
    return false;

  // The check never returns: the failure edge folds into the success
  // edge. The failure block loses the incoming block, the branch keeps
  // its provenance.
  S.FailBB->removePredecessor(S.Br->getParent());
  IRBuilder<> B(S.Br);
  BranchInst *NewBr = B.CreateBr(OkBB);
  NewBr->copyMetadata(*S.Br);
  S.Br->eraseFromParent();
  ++Changed;

  // The prologue store of the cookie goes first: its xor is not part of
  // the compare's operand chain, so the two cleanups cannot touch each
  // other's instructions.
  // Earlier erasures may take the slot address with them; track it weakly.
  WeakTrackingVH Slot;
  if (S.SetupStore) {
    Slot = S.SetupStore->getPointerOperand();
    S.SetupStore->eraseFromParent();
    ++Changed;
    if (S.SetupXor->use_empty())
      eraseIfDead(S.SetupXor);
  }

  // The check call, the compare and the xor go with it. The compare is
  // the root of the cleanup: erasing it takes the xor and the loads it
  // feeds with it.
  S.CheckCall->eraseFromParent();
  ++Changed;
  if (auto *Arg = dyn_cast<Instruction>(S.CheckArg); Arg && Arg != S.CheckXor)
    eraseIfDead(Arg);
  if (S.Cmp->use_empty())
    eraseIfDead(S.Cmp);
  else if (S.CheckXor->use_empty())
    eraseIfDead(S.CheckXor);
  // The frame slot only held the cookie; its loads went with the compare.
  if (auto *SlotI = dyn_cast_or_null<Instruction>(static_cast<Value *>(Slot)))
    eraseIfDead(SlotI);
  // The failure block is unreachable now, unless another check shares it,
  // and the success block is the straight continuation of the check's
  // block: the compiler's IR has neither split.
  if (pred_empty(S.FailBB) && !S.FailBB->isEntryBlock())
    DeleteDeadBlock(S.FailBB);
  MergeBlockIntoPredecessor(OkBB);
  return true;
}

/// The check after the cleanup folded its failure edge away (the check call
/// does not return when it fails): `stored ^ sp` is still compared with or
/// subtracted from the cookie global, and the result only stays in the
/// return register. The compare goes, a return of it becomes undef, and so
/// does the prologue store of the same cookie.
bool removeFoldedCookie(Function &F, unsigned &Changed) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *Xor = dyn_cast<BinaryOperator>(&I);
      if (!Xor || Xor->getOpcode() != Instruction::Xor || !Xor->getType()->isIntegerTy(64))
        continue;
      Value *SP = nullptr;
      for (Value *Op : Xor->operands())
        if (isStackPointerValue(Op))
          SP = Op;
      Value *Stored = Xor->getOperand(Xor->getOperand(0) == SP ? 1 : 0);
      if (!SP || !isa<LoadInst>(Stored) || !Xor->hasOneUse())
        continue;
      auto *Check = dyn_cast<Instruction>(Xor->user_back());
      if (!Check || !(isa<ICmpInst>(Check) ||
                      (isa<BinaryOperator>(Check) && Check->getOpcode() == Instruction::Sub)))
        continue;
      auto *Cookie = dyn_cast<LoadInst>(Check->getOperand(Check->getOperand(0) == Xor ? 1 : 0));
      auto *G = Cookie ? dyn_cast<GlobalVariable>(Cookie->getPointerOperand()) : nullptr;
      if (!G || !llvm::all_of(Check->users(), [](User *U) { return isa<ReturnInst>(U); }))
        continue;
      // The prologue: `cookie ^ sp` stored to the frame.
      for (BasicBlock &B2 : F)
        for (Instruction &I2 : make_early_inc_range(B2))
          if (auto *St = dyn_cast<StoreInst>(&I2))
            if (auto *SX = dyn_cast<BinaryOperator>(St->getValueOperand());
                SX && SX->getOpcode() == Instruction::Xor &&
                (SX->getOperand(0) == SP || SX->getOperand(1) == SP) &&
                llvm::any_of(SX->operands(), [&](Value *Op) {
                  auto *L = dyn_cast<LoadInst>(Op);
                  return L && L->getPointerOperand() == G;
                })) {
              St->eraseFromParent();
              eraseIfDead(SX);
              ++Changed;
            }
      Check->replaceAllUsesWith(UndefValue::get(Check->getType()));
      eraseIfDead(Check);
      ++Changed;
      return true;
    }
  return false;
}

} // namespace

PreservedAnalyses revenant::GsCookiePass::run(Function &F,
                                              FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  unsigned Changed = 0;
  CookieSite Site;
  if (findCookieSite(F, Site)) {
    removeCookie(Site, Changed);
    // The marker: the source emitter uses it to drop what the cookie
    // left behind in the frame.
    F.addFnAttr(GsCookieAttr, "1");
  } else if (removeFoldedCookie(F, Changed)) {
    F.addFnAttr(GsCookieAttr, "1");
  }

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

PreservedAnalyses revenant::StackProtectorPass::run(Function &F,
                                                    FunctionAnalysisManager &) {
  if (F.isDeclaration() || !F.hasFnAttribute(GsCookieAttr) ||
      F.hasFnAttribute("revenant.o0"))
    return PreservedAnalyses::all();
  BasicBlock &Entry = F.getEntryBlock();
  IRBuilder<> B(&Entry, Entry.getFirstInsertionPt());
  Type *PtrTy = B.getPtrTy();
  AllocaInst *Slot = B.CreateAlloca(PtrTy, nullptr, "StackGuardSlot");
  Value *Guard = B.CreateIntrinsic(Intrinsic::stackguard, ArrayRef<Type *>{},
                                   ArrayRef<Value *>{});
  B.CreateIntrinsic(Intrinsic::stackprotector, ArrayRef<Type *>{},
                    ArrayRef<Value *>{Guard, Slot});
  return PreservedAnalyses::none();
}
