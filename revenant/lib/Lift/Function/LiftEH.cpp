//===- LiftEH.cpp - exception-region lifting for the function lifter - C++ -*-===//
//
// Part of the Revenant LLVM fork. See LiftEH.h.
//
// Reference shapes come from clang -cc1 on this box
// (x86_64-pc-windows-msvc, -fexceptions):
// - C++ try/catch: `invoke T callee() to label %normal unwind label %dispatch`
//   with `%dispatch: catchswitch within none [label %catch] unwind to caller`,
//   `%catch: catchpad within %sw [ptr @typeinfo, i32 0, ptr null]`,
//   `catchret from %pad to label %cont`. Personality `__CxxFrameHandler3`.
// - SEH __try/__except: same skeleton but `catchpad within %sw [ptr null]`
//   and `llvm.eh.exceptioncode` reads in the handler. Personality
//   `__C_specific_handler`.
// - Unwind-only (dtor/unwind-map, Rust drop glue): `invoke ... unwind
//   label %cleanup` with `%cleanup: cleanuppad within none []` and
//   `cleanupret from %pad unwind to caller`.
// - MinGW LSDA: `invoke ... unwind label %lpad` with `%lpad: landingpad
//   {ptr, i32} cleanup` (plus catch clauses when the action is nonzero)
//   and `resume`.
//
// Firing rule: a direct `call` (not a declare-only intrinsic, not already
// an invoke) whose !revenant.va lies strictly inside a decoded try range
// [Begin, End) becomes an invoke to a fresh dispatch block. The normal
// edge splits the caller's block so fallthrough stays identical; pads only
// run on unwind. Calls outside every range, and ranges with no handler or
// cleanup address, never fire.
//
//===----------------------------------------------------------------------===//

#include "LiftEH.h"

#include "revenant/IR/Contract.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace revenant::lift {
namespace {

/// !revenant.va of I, if present.
std::optional<uint64_t> vaOf(const Instruction &I) {
  return ir::getInstructionVA(I);
}

/// True when VA lies strictly inside [Begin, End).
bool inRange(uint64_t VA, uint64_t Begin, uint64_t End) {
  return Begin <= VA && VA < End;
}

/// Personality declaration for Name, creating `declare i32 @Name(...)`.
Function *personalityDecl(Module *M, StringRef Name) {
  if (Function *F = M->getFunction(Name)) {
    if (F->isDeclaration())
      return F;
    return nullptr;
  }
  FunctionType *FTy =
      FunctionType::get(Type::getInt32Ty(M->getContext()), true);
  return Function::Create(FTy, GlobalValue::ExternalLinkage, Name, M);
}

StringRef personalityFor(EHModel Model) {
  switch (Model) {
  case EHModel::MSVC:
    return "__CxxFrameHandler3";
  case EHModel::SEH:
    return "__C_specific_handler";
  case EHModel::Itanium:
    return "__gxx_personality_seh0";
  case EHModel::Rust:
    return "__C_specific_handler";
  case EHModel::None:
    return "";
  }
  return "";
}

/// Index of the try range containing VA, or -1.
int rangeIndex(const FunctionEH &EH, uint64_t VA) {
  for (size_t N = 0; N < EH.TryRegions.size(); ++N) {
    const EHTryRegion &T = EH.TryRegions[N];
    if (inRange(VA, T.Begin.Value, T.End.Value))
      return (int)N;
  }
  return -1;
}

} // namespace

unsigned liftEH(Function &Fn, const FunctionEH &EH) {
  if (EH.Model == EHModel::None || EH.TryRegions.empty())
    return 0;
  Module *M = Fn.getParent();
  if (!M)
    return 0;
  StringRef PersonalityName = personalityFor(EH.Model);
  if (PersonalityName.empty())
    return 0;

  // Collect candidate calls first: rewriting mutates the CFG.
  SmallVector<CallInst *, 8> Candidates;
  for (BasicBlock &BB : Fn)
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      // Intrinsics, inline asm and declarations without bodies never throw
      // across a recovered table range.
      Function *Callee = CI->getCalledFunction();
      if (!Callee || Callee->isIntrinsic() || Callee->isDeclaration())
        continue;
      auto VA = vaOf(*CI);
      if (!VA)
        continue;
      if (rangeIndex(EH, *VA) < 0)
        continue;
      Candidates.push_back(CI);
    }
  if (Candidates.empty())
    return 0;

  Function *Personality = personalityDecl(M, PersonalityName);
  if (!Personality)
    return 0;
  if (!Fn.hasPersonalityFn())
    Fn.setPersonalityFn(Personality);

  unsigned Converted = 0;
  LLVMContext &C = M->getContext();
  for (CallInst *CI : Candidates) {
    auto VA = vaOf(*CI);
    if (!VA)
      continue;
    int RI = rangeIndex(EH, *VA);
    if (RI < 0)
      continue;
    const EHTryRegion &T = EH.TryRegions[(size_t)RI];

    // Resolve the handler/cleanup addresses for this range: prefer the
    // range's own handler list, else any handler/cleanup in the entry.
    const EHHandler *Handler = nullptr;
    for (uint32_t H : T.Handlers)
      if (H < EH.Handlers.size()) {
        Handler = &EH.Handlers[H];
        break;
      }
    const EHCleanup *Cleanup =
        EH.Cleanups.empty() ? nullptr : &EH.Cleanups[0];
    if (!Handler && !Cleanup)
      continue;

    BasicBlock *BB = CI->getParent();
    // Split the block after the call: normal edge keeps fallthrough.
    BasicBlock::iterator SplitAt = CI->getIterator();
    ++SplitAt;
    BasicBlock *Normal = BB->splitBasicBlock(SplitAt, "eh.normal");
    // The split moved the call's successors; the call still ends BB.
    BasicBlock *Dispatch =
        BasicBlock::Create(C, "eh.dispatch", &Fn, Normal);

    // Build the pad in the dispatch block per model.
    Value *Pad = nullptr;
    BasicBlock *HandlerBB = nullptr;
    if (EH.Model == EHModel::Itanium) {
      // MinGW LSDA: landingpad + resume. Catch clauses come from the
      // type table when the action is nonzero; cleanup always present
      // for the unwind-only shape.
      auto *LP = LandingPadInst::Create(
          StructType::get(C, {PointerType::getUnqual(C), Type::getInt32Ty(C)}),
          0, "eh.lpad", Dispatch);
      LP->setCleanup(true);
      auto *Resume = ResumeInst::Create(LP, Dispatch);
      ir::copyInstructionVA(*CI, *LP);
      ir::copyInstructionVA(*CI, *Resume);
      (void)Handler;
      (void)Cleanup;
    } else if (Handler && (EH.Model == EHModel::MSVC ||
                           EH.Model == EHModel::SEH)) {
      // catchswitch/catchpad skeleton copied from clang -cc1 lowering.
      auto *SW = CatchSwitchInst::Create(nullptr, nullptr, 1, "eh.catchswitch",
                                         Dispatch);
      HandlerBB = BasicBlock::Create(C, "eh.catch", &Fn, Normal);
      SW->addHandler(HandlerBB);
      CatchPadInst *PadI = nullptr;
      if (EH.Model == EHModel::MSVC) {
        // [ptr @typeinfo-or-null, i32 0, ptr null]; the lifter has no
        // RTTI globals, so a null selector keeps the shape verifiable.
        Value *NullPtr =
            ConstantPointerNull::get(PointerType::getUnqual(C));
        Value *Args[] = {NullPtr, ConstantInt::get(Type::getInt32Ty(C), 0),
                         NullPtr};
        PadI = CatchPadInst::Create(SW, Args, "eh.catchpad", HandlerBB);
      } else {
        Value *NullPtr =
            ConstantPointerNull::get(PointerType::getUnqual(C));
        PadI = CatchPadInst::Create(SW, {NullPtr}, "eh.catchpad", HandlerBB);
      }
      auto *Ret = CatchReturnInst::Create(PadI, Normal, HandlerBB);
      ir::copyInstructionVA(*CI, *SW);
      ir::copyInstructionVA(*CI, *PadI);
      ir::copyInstructionVA(*CI, *Ret);
      // Record the source handler VA on the pad for the uneh pass.
      PadI->setMetadata(
          "revenant.handler",
          MDNode::get(C, {ConstantAsMetadata::get(ConstantInt::get(
                          Type::getInt64Ty(C), Handler->Address.Value))}));
      Pad = PadI;
    } else if (Cleanup) {
      // Unwind-only: cleanuppad/cleanupret to the normal edge.
      // cleanupret unwinds to caller: the pad runs dtors then resumes.
      auto *PadI = CleanupPadInst::Create(nullptr, {}, "eh.cleanup", Dispatch);
      auto *Ret = CleanupReturnInst::Create(PadI, nullptr, Dispatch);
      ir::copyInstructionVA(*CI, *PadI);
      ir::copyInstructionVA(*CI, *Ret);
      PadI->setMetadata(
          "revenant.handler",
          MDNode::get(C, {ConstantAsMetadata::get(ConstantInt::get(
                          Type::getInt64Ty(C), Cleanup->Address.Value))}));
      Pad = PadI;
    } else {
      Normal->eraseFromParent();
      Dispatch->eraseFromParent();
      continue;
    }

    // Convert the call to an invoke along the same edges.
    SmallVector<OperandBundleDef, 2> Bundles;
    CI->getOperandBundlesAsDefs(Bundles);
    InvokeInst *II = InvokeInst::Create(
        CI->getFunctionType(), CI->getCalledOperand(), Normal, Dispatch,
        SmallVector<Value *, 8>(CI->args()), Bundles, CI->getName(), BB);
    II->setCallingConv(CI->getCallingConv());
    II->setAttributes(CI->getAttributes());
    ir::copyInstructionVA(*CI, *II);
    CI->replaceAllUsesWith(II);
    CI->eraseFromParent();
    (void)Pad;
    ++Converted;
  }
  return Converted;
}

} // namespace revenant::lift
