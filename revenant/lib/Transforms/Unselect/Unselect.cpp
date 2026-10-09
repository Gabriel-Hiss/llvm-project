//===- Unselect.cpp - undo if-conversion and predicate canonicalization -===//
//
// Part of the Revenant LLVM fork.
//
// See Unselect.h for the corpus shapes. Two rewrites:
//
// A. Select/if-conversion undo. A `select` in a block whose condition comes
//    from an `icmp`, where both arms are cheap (constant / argument / load
//    / pure arithmetic already in the block), becomes a diamond:
//
//      %s = select i1 %c, T, F          entry: br %c, then, else
//      use %s                     -->   then:  br merge
//                                       else:  br merge
//                                       merge: %s = phi [T, then], [F, else]
//
//    Alive2 sketch: select is `c ? T : F`; the diamond computes the same
//    value along each path, and the phi picks the path taken, so the merge
//    value equals `c ? T : F` on every execution. The rewrite is done only
//    when T/F have no side effects (no calls, loads stay in place) and the
//    select does not feed a loop phi (unrotated loops keep selects), so no
//    new faults or ordering hazards appear: speculation removal can only
//    remove faults, never add them.
//
// B. Min/max/abs intrinsic expansion. `llvm.smax/smin/umax/umin` and
//    `llvm.abs` (plus fmax/fmin/fabs on the float side) expand to the
//    compare+select the -O0 frontend emitted before InstCombine folded
//    them:
//
//      %m = call @llvm.smax.i32(%a, %b)
//        --> %c = icmp sgt i32 %a, %b; %m = select i1 %c, i32 %a, i32 %b
//
//    The expansion is a pure semantic unfolding of the intrinsic
//    definition (LangRef), so it is equivalence by construction. The
//    select left behind is then a candidate for rewrite A in a later
//    fixed-point iteration when its arms are cheap.
//
// C. Predicate canonicalization undo. InstCombine normalizes compares:
//    swapped operands (`icmp ult %b, %a` from source `icmp ugt %a, %b`),
//    inverted predicates feeding selects (`%.not` + swapped arms), and
//    poison-generating flag churn (`samesign`, `nneg` zext). The pass
//    drops `samesign`/`nneg` markers back to plain predicates and folds
//    double inversions (`xor i1 %c, true` feeding a select swaps arms
//    instead), matching the straightforward -O0 spelling. Dropping flags
//    only removes `poison` refinement, so it is refinement-free.
//
// Provenance: every created instruction copies `!revenant.va` and the
// debug location from the instruction it replaces. `!revenant.opaque`
// calls are never touched, `!revenant.region` is never dropped (new
// blocks get no region of their own; structural annotations still
// describe the same CFG region).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unselect/Unselect.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace {

unsigned vaKind(LLVMContext &Ctx) {
  static unsigned K = 0;
  if (!K)
    K = Ctx.getMDKindID("revenant.va");
  return K;
}

bool hasOpaqueMarker(const Instruction &I) {
  return I.getMetadata(I.getContext().getMDKindID("revenant.opaque")) !=
         nullptr;
}

void copyProvenance(const Instruction &Src, Instruction &Dst) {
  if (MDNode *N = Src.getMetadata(vaKind(Src.getContext())))
    Dst.setMetadata(vaKind(Dst.getContext()), N);
  Dst.setDebugLoc(Src.getDebugLoc());
}

// Diamond sides are empty: arms are referenced from the merge phi, never
// cloned or re-evaluated. Any pure value (including selects and phis from
// earlier unselect rounds) is safe; the split preserves dominance for
// every definition above the select. Loads/calls stay out.
bool isCheapForDiamond(const Value *V, const BasicBlock *Entry) {
  if (isa<Constant>(V) || isa<Argument>(V))
    return true;
  auto *I = dyn_cast<Instruction>(V);
  if (!I || I->getParent() != Entry)
    return true; // defined elsewhere: just a use, no duplication
  if (hasOpaqueMarker(*I))
    return false;
  switch (I->getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::BitCast:
  case Instruction::ICmp:
  case Instruction::FCmp:
  case Instruction::Select:
  case Instruction::PHI:
    return !I->mayHaveSideEffects();
  default:
    return false;
  }
}

// The select condition must come from a compare (or a negated compare)
// defined in the same block: that is the if-conversion signature.
Value *normalizeCond(Value *C, bool &Inverted) {
  Inverted = false;
  if (auto *I = dyn_cast<Instruction>(C)) {
    if (I->getOpcode() == Instruction::ICmp ||
        I->getOpcode() == Instruction::FCmp)
      return C;
    // `xor i1 %c, true` (InstCombine's canonical not): fold into arms.
    if (I->getOpcode() == Instruction::Xor)
      if (auto *K = dyn_cast<ConstantInt>(I->getOperand(1)))
        if (K->isOne()) {
          Inverted = true;
          return I->getOperand(0);
        }
  }
  return nullptr;
}

// A select that directly feeds a loop-carried phi must keep its form:
// unrotating relies on selects staying selects inside loop headers.
bool feedsLoopPhi(SelectInst *SI) {
  for (User *U : SI->users())
    if (auto *PN = dyn_cast<PHINode>(U))
      if (PN->getParent() == SI->getParent())
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
          if (PN->getIncomingBlock(i) == SI->getParent())
            return true;
  return false;
}

// Rewrite one select into a diamond. Returns true on success.
bool unselectOne(SelectInst *SI) {
  BasicBlock *Entry = SI->getParent();
  // The split moves the terminator verbatim to the merge block, so any
  // terminator is fine, including a fork from an earlier unselect round.
  // Only EH pads (whose semantics the split would disturb) and blocks
  // with phis (whose order a split would disturb) are excluded.
  if (!Entry->phis().empty())
    return false;
  // The select must not be across exception or loop-latch edges.
  if (Entry->isEHPad() || Entry->isLandingPad())
    return false;

  bool Inverted = false;
  Value *Cond = normalizeCond(SI->getCondition(), Inverted);
  if (!Cond)
    return false;
  // Cond dominates the fork by SSA construction (defined above the select
  // or in a dominating block; the split only cuts below it), so the fork
  // reuses the value in place. No same-block requirement: chained selects
  // converted bottom-up leave their conditions in the parent block.
  Value *T = SI->getTrueValue();
  Value *F = SI->getFalseValue();
  if (Inverted)
    std::swap(T, F);
  if (!isCheapForDiamond(T, Entry) || !isCheapForDiamond(F, Entry))
    return false;
  if (feedsLoopPhi(SI))
    return false;
  // Do not duplicate values defined after the select.
  for (Value *V : {T, F}) {
    if (auto *I = dyn_cast<Instruction>(V))
      if (I->getParent() == Entry) {
        bool After = false;
        for (auto &J : *Entry) {
          if (&J == SI)
            After = true;
          if (&J == I && After)
            return false;
        }
      }
  }

  LLVMContext &Ctx = Entry->getContext();
  Function *Fn = Entry->getParent();

  // Split: Entry | Then | Else | Merge. Merge starts at the select.
  BasicBlock::iterator SplitIt(SI);
  BasicBlock *Merge =
      Entry->splitBasicBlock(SplitIt, Entry->getName() + ".merge");
  // Entry now ends with `br Merge`; replace with the conditional fork.
  Entry->getTerminator()->eraseFromParent();
  BasicBlock *Then =
      BasicBlock::Create(Ctx, Entry->getName() + ".t", Fn, Merge);
  BasicBlock *Else =
      BasicBlock::Create(Ctx, Entry->getName() + ".f", Fn, Merge);
  BranchInst *Fork = BranchInst::Create(Then, Else, Cond, Entry);
  copyProvenance(*SI, *Fork);
  BranchInst *GotoT = BranchInst::Create(Merge, Then);
  copyProvenance(*SI, *GotoT);
  BranchInst *GotoF = BranchInst::Create(Merge, Else);
  copyProvenance(*SI, *GotoF);

  // Replace the select with the merge phi.
  PHINode *Phi = PHINode::Create(SI->getType(), 2, SI->getName() + ".if",
                                 Merge->getFirstInsertionPt());
  Phi->addIncoming(T, Then);
  Phi->addIncoming(F, Else);
  copyProvenance(*SI, *Phi);
  SI->replaceAllUsesWith(Phi);
  // TheFork uses Cond from Entry; the value stays live across the split.
  (void)Inverted;
  SI->eraseFromParent();
  return true;
}

// Expand one min/max/abs intrinsic (or scmp/ucmp) to cmp+select.
// Returns true on success.
bool expandIntrinsic(CallInst *CI, Intrinsic::ID ID) {
  if (hasOpaqueMarker(*CI))
    return false;
  Function *F = CI->getCalledFunction();
  if (!F)
    return false;
  Type *Ty = CI->getType();
  if (!Ty->isIntegerTy() && !Ty->isFloatingPointTy())
    return false;
  Value *A = CI->getArgOperand(0);
  Value *B = CI->getNumOperands() > 2 ? CI->getArgOperand(1) : nullptr;

  CmpInst::Predicate Pred = CmpInst::BAD_ICMP_PREDICATE;
  bool IsMin = false, IsAbs = false, IsFloat = Ty->isFloatingPointTy();
  switch (ID) {
  case Intrinsic::smax:
    Pred = IsFloat ? CmpInst::FCMP_OGT : CmpInst::ICMP_SGT;
    break;
  case Intrinsic::smin:
    Pred = IsFloat ? CmpInst::FCMP_OLT : CmpInst::ICMP_SLT;
    IsMin = true;
    break;
  case Intrinsic::umax:
    Pred = CmpInst::ICMP_UGT;
    break;
  case Intrinsic::umin:
    Pred = CmpInst::ICMP_ULT;
    IsMin = true;
    break;
  case Intrinsic::abs:
    IsAbs = true;
    break;
  case Intrinsic::fabs:
    IsAbs = true;
    break;
  case Intrinsic::scmp:
  case Intrinsic::ucmp: {
    // Three-way compare from an if-ladder (corpus c-clang-o1 @cmp_signed:
    // `if (a<b) return -1; if (a>b) return 1; return 0`). Unfold to the
    // nested select the ladder lowers to; unselectOne turns each level
    // into a diamond in the same fixed-point run. Exact by LangRef.
    bool Signed = ID == Intrinsic::scmp;
    CmpInst::Predicate Lt =
        Signed ? CmpInst::ICMP_SLT : CmpInst::ICMP_ULT;
    CmpInst::Predicate Gt =
        Signed ? CmpInst::ICMP_SGT : CmpInst::ICMP_UGT;
    Value *MinusOne = ConstantInt::get(Ty, (uint64_t)(int64_t)-1, true);
    Value *Zero = ConstantInt::get(Ty, 0);
    Value *One = ConstantInt::get(Ty, 1);
    auto *CmpLt = CmpInst::Create(Instruction::ICmp, Lt, A, B,
                                  CI->getName() + ".lt", CI->getIterator());
    copyProvenance(*CI, *CmpLt);
    auto *CmpGt = CmpInst::Create(Instruction::ICmp, Gt, A, B,
                                  CI->getName() + ".gt", CI->getIterator());
    copyProvenance(*CI, *CmpGt);
    auto *Inner = SelectInst::Create(CmpGt, One, Zero,
                                     CI->getName() + ".hi", CI->getIterator());
    copyProvenance(*CI, *Inner);
    auto *Outer = SelectInst::Create(CmpLt, MinusOne, Inner,
                                     CI->getName() + ".sel", CI->getIterator());
    copyProvenance(*CI, *Outer);
    CI->replaceAllUsesWith(Outer);
    CI->eraseFromParent();
    return true;
  }
  default:
    return false;
  }
  (void)IsMin;

  BasicBlock *BB = CI->getParent();
  if (BB->phis().begin() != BB->phis().end()) {
    // Inserting before a phi-user is fine; inserting among phis is not.
    // Bail if the call sits before the first non-phi.
    if (CI->getIterator() == BB->begin())
      return false;
  }

  Instruction *Cmp = nullptr;
  Value *T = nullptr, *V = nullptr;
  if (IsAbs) {
    if (IsFloat) {
      // fabs: bit-clear expansion is a target idiom, not if-conversion.
      return false;
    }
    Value *Zero = ConstantInt::get(Ty, 0);
    Cmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_SLT, A, Zero,
                          CI->getName() + ".c");
    Value *Neg = BinaryOperator::Create(Instruction::Sub, Zero, A,
                                        CI->getName() + ".neg", CI->getIterator());
    copyProvenance(*CI, *cast<Instruction>(Neg));
    T = Neg;
    V = A;
  } else {
    Cmp = CmpInst::Create(IsFloat ? Instruction::FCmp : Instruction::ICmp,
                          Pred, A, B, CI->getName() + ".c");
    T = A;
    V = B;
  }
  Cmp->insertBefore(CI->getIterator());
  copyProvenance(*CI, *Cmp);
  auto *Sel = SelectInst::Create(Cmp, T, V, CI->getName() + ".sel",
                                 CI->getIterator());
  copyProvenance(*CI, *Sel);
  CI->replaceAllUsesWith(Sel);
  CI->eraseFromParent();
  return true;
}

// Undo InstCombine predicate churn: drop samesign/nneg back to the plain
// -O0 predicate, fold `xor i1 not` into select arms.
bool canonicalizePredicates(Function &F) {
  bool Changed = false;
  SmallVector<Instruction *, 16> Work;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      if (isa<ICmpInst>(I) || isa<SelectInst>(I) || isa<CastInst>(I))
        Work.push_back(&I);
    }
  for (Instruction *I : Work) {
    if (I->getParent() == nullptr || I->getIterator() == BasicBlock::iterator())
      continue;
    if (auto *CI = dyn_cast<ICmpInst>(I)) {
      if (hasOpaqueMarker(*CI))
        continue;
      // `samesign` is InstCombine's enrichment: -O0 never emits it.
      if (CI->hasSameSign()) {
        CI->setSameSign(false);
        Changed = true;
      }
    } else if (auto *ZC = dyn_cast<ZExtInst>(I)) {
      // `zext nneg` from a bool: back to plain zext.
      if (ZC->getSrcTy()->isIntegerTy(1) && ZC->hasNonNeg()) {
        ZC->setNonNeg(false);
        Changed = true;
      }
    } else if (auto *SI = dyn_cast<SelectInst>(I)) {
      if (hasOpaqueMarker(*SI))
        continue;
      // Double inversion: `select (xor %c, true), T, F` is
      // `select %c, F, T` before canonicalization; undo it so the arms
      // match the -O0 branch order.
      auto *X = dyn_cast<Instruction>(SI->getCondition());
      if (X && X->getOpcode() == Instruction::Xor &&
          X->getParent() == SI->getParent() && X->hasOneUse()) {
        if (auto *K = dyn_cast<ConstantInt>(X->getOperand(1)))
          if (K->isOne() && !hasOpaqueMarker(*X)) {
            Value *C = X->getOperand(0);
            Value *T = SI->getTrueValue();
            Value *Fv = SI->getFalseValue();
            auto *NS = SelectInst::Create(C, Fv, T, SI->getName());
            copyProvenance(*SI, *NS);
            NS->insertBefore(SI->getIterator());
            SI->replaceAllUsesWith(NS);
            // Leave the dead xor for DCE; only the select is ours.
            SI->eraseFromParent();
            Changed = true;
          }
      }
    }
  }
  return Changed;
}

bool expandIntrinsics(Function &F) {
  bool Changed = false;
  SmallVector<CallInst *, 16> Work;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *Callee = CI->getCalledFunction();
      if (!Callee || !Callee->isIntrinsic())
        continue;
      switch (Callee->getIntrinsicID()) {
      case Intrinsic::smax:
      case Intrinsic::smin:
      case Intrinsic::umax:
      case Intrinsic::umin:
      case Intrinsic::abs:
      case Intrinsic::scmp:
      case Intrinsic::ucmp:
        Work.push_back(CI);
        break;
      default:
        break;
      }
    }
  for (CallInst *CI : Work) {
    if (CI->getParent() == nullptr)
      continue;
    Function *Callee = CI->getCalledFunction();
    if (!Callee)
      continue;
    Changed |= expandIntrinsic(CI, Callee->getIntrinsicID());
  }
  return Changed;
}

bool unselectAll(Function &F) {
  bool Changed = false;
  SmallVector<SelectInst *, 32> Work;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *SI = dyn_cast<SelectInst>(&I))
        if (!hasOpaqueMarker(*SI))
          Work.push_back(SI);
  // Consumers first (reverse order): an outer select of a chain converts
  // while its inner-select arm still dominates the fork; the inner arm
  // then converts in the same sweep.
  for (auto It = Work.rbegin(); It != Work.rend(); ++It) {
    SelectInst *SI = *It;
    if (SI->getParent() == nullptr)
      continue;
    Changed |= unselectOne(SI);
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UnselectPass::run(Function &F,
                                              FunctionAnalysisManager &) {
  bool Changed = false;
  // Fixed point: intrinsic expansion creates selects that unselectOne
  // then turns into diamonds. Two rounds suffice (no nesting in practice).
  for (int Round = 0; Round < 3; ++Round) {
    bool R = false;
    R |= expandIntrinsics(F);
    R |= canonicalizePredicates(F);
    R |= unselectAll(F);
    Changed |= R;
    if (!R)
      break;
  }
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-unselect` name in Pipeline/Register.cpp, so the entry point
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
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnselect", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unselect") {
                    PM.addPass(revenant::UnselectPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
