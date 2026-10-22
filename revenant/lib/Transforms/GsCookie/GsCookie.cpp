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
// branch keeps its provenance metadata (`!revenant.va`) and its
// position in the block, so the block layout and the provenance of the
// surviving code do not change.
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
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

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
      CallBase *Call = nullptr;
      BasicBlock *Fail = nullptr;
      for (BasicBlock *Succ : Br->successors()) {
        for (Instruction &SI : *Succ) {
          auto *CB = dyn_cast<CallBase>(&SI);
          if (!CB || CB->arg_size() != 1)
            continue;
          if (CB->getArgOperand(0) != Xor)
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
  if (S.SetupStore) {
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
  if (S.Cmp->use_empty())
    eraseIfDead(S.Cmp);
  else if (S.CheckXor->use_empty())
    eraseIfDead(S.CheckXor);
  return true;
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
  }

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
