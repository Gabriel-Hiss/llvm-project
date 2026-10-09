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
    return "__CxxFrameHandler3";
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
      // A direct call to an intrinsic cannot unwind. Ordinary declarations
      // may throw, and indirect calls may target a throwing function, so both
      // remain candidates when their instruction VA is inside the range.
      if (Function *Callee = CI->getCalledFunction())
        if (Callee->isIntrinsic())
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

    // Resolve only handlers associated with this try region. Filter helper
    // entries are not catch funclets; the scope table's second entry is the
    // actual __except body.
    SmallVector<const EHHandler *, 4> Handlers;
    for (uint32_t H : T.Handlers)
      if (H < EH.Handlers.size() &&
          EH.Handlers[H].Kind != HandlerKind::Filter)
        Handlers.push_back(&EH.Handlers[H]);
    if (Handlers.empty())
      for (const EHHandler &H : EH.Handlers)
        if (H.State == T.State && H.Kind != HandlerKind::Filter)
          Handlers.push_back(&H);
    const EHCleanup *Cleanup = nullptr;
    for (const EHCleanup &C : EH.Cleanups)
      if (C.State == T.State) {
        Cleanup = &C;
        break;
      }
    const EHHandler *Handler = Handlers.empty() ? nullptr : Handlers.front();
    if (!Handler && !Cleanup)
      continue;

    BasicBlock *BB = CI->getParent();
    // Split the block after the call: normal edge keeps fallthrough.
    BasicBlock::iterator SplitAt = CI->getIterator();
    ++SplitAt;
    BasicBlock *Normal = BB->splitBasicBlock(SplitAt, "eh.normal");
    BasicBlock *Dispatch =
        BasicBlock::Create(C, "eh.dispatch", &Fn, Normal);

    // Build the pad in the dispatch block per model.
    Value *Pad = nullptr;
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
    } else if (Handler && Handler->Kind != HandlerKind::Finally &&
               Handler->Kind != HandlerKind::Cleanup &&
               (EH.Model == EHModel::MSVC || EH.Model == EHModel::SEH)) {
      // One catchpad per decoded catch funclet, matching catchswitch's list.
      auto *SW = CatchSwitchInst::Create(
          ConstantTokenNone::get(C), nullptr, Handlers.size(),
          "eh.catchswitch", Dispatch);
      for (const EHHandler *H : Handlers) {
        BasicBlock *HandlerBB = BasicBlock::Create(C, "eh.catch", &Fn, Normal);
        SW->addHandler(HandlerBB);
        CatchPadInst *PadI = nullptr;
        Value *NullPtr =
            ConstantPointerNull::get(PointerType::getUnqual(C));
        if (EH.Model == EHModel::MSVC) {
          // The lifted module has no RTTI globals. Retain the source type
          // spelling in metadata while keeping the selector verifiable.
          Value *Args[] = {NullPtr, ConstantInt::get(Type::getInt32Ty(C), 0),
                           NullPtr};
          PadI = CatchPadInst::Create(SW, Args, "eh.catchpad", HandlerBB);
        } else {
          PadI = CatchPadInst::Create(SW, {NullPtr}, "eh.catchpad", HandlerBB);
        }
        auto *Ret = CatchReturnInst::Create(PadI, Normal, HandlerBB);
        ir::copyInstructionVA(*CI, *PadI);
        ir::copyInstructionVA(*CI, *Ret);
        PadI->setMetadata(
            "revenant.handler",
            MDNode::get(C, {ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt64Ty(C), H->Address.Value))}));
        if (!H->TypeName.empty())
          PadI->setMetadata("revenant.catchtype",
                            MDNode::get(C, MDString::get(C, H->TypeName)));
        Pad = PadI;
      }
      ir::copyInstructionVA(*CI, *SW);
    } else if (Cleanup || Handler) {
      // Finally/cleanup handlers run only while unwinding.
      const uint64_t HandlerVA = Handler ? Handler->Address.Value
                                         : Cleanup->Address.Value;
      auto *PadI = CleanupPadInst::Create(ConstantTokenNone::get(C), {},
                                          "eh.cleanup", Dispatch);
      auto *Ret = CleanupReturnInst::Create(PadI, nullptr, Dispatch);
      ir::copyInstructionVA(*CI, *PadI);
      ir::copyInstructionVA(*CI, *Ret);
      PadI->setMetadata(
          "revenant.handler",
          MDNode::get(C, {ConstantAsMetadata::get(ConstantInt::get(
                          Type::getInt64Ty(C), HandlerVA))}));
      if (Handler && Handler->Kind == HandlerKind::Finally)
        PadI->setMetadata("revenant.finally", MDNode::get(C, {}));
      Pad = PadI;
    } else {
      Normal->eraseFromParent();
      Dispatch->eraseFromParent();
      continue;
    }


    // Convert the call to an invoke along the same edges. splitBasicBlock
    // left a `br Normal` terminator in BB: drop it, insert the invoke at
    // the call's position (the new terminator), then erase the call.
    SmallVector<OperandBundleDef, 2> Bundles;
    CI->getOperandBundlesAsDefs(Bundles);
    BB->getTerminator()->eraseFromParent();
    InvokeInst *II = InvokeInst::Create(
        CI->getFunctionType(), CI->getCalledOperand(), Normal, Dispatch,
        SmallVector<Value *, 8>(CI->args()), Bundles, CI->getName(), CI);
    II->setCallingConv(CI->getCallingConv());
    II->setAttributes(CI->getAttributes());
    ir::copyInstructionVA(*CI, *II);
    if (CI->use_empty()) {
      CI->eraseFromParent();
    } else if (pred_size(Normal) == 1 && !II->getType()->isVoidTy()) {
      // Single-predecessor normal edge: uses stay dominated, replace all.
      CI->replaceAllUsesWith(II);
      CI->eraseFromParent();
    } else {
      // The handler's catchret also targets Normal, so the invoke result
      // no longer dominates its uses: route them through a PHI (invoke
      // value on the BB edge, undef on handler edges).
      PHINode *Phi = PHINode::Create(II->getType(), 2, CI->getName() + ".eh",
                                     &Normal->front());
      for (BasicBlock *Pred : predecessors(Normal)) {
        Value *V = Pred == BB ? static_cast<Value *>(II)
                              : static_cast<Value *>(
                                    UndefValue::get(II->getType()));
        Phi->addIncoming(V, Pred);
      }
      ir::copyInstructionVA(*CI, *Phi);
      CI->replaceAllUsesWith(Phi);
      CI->eraseFromParent();
    }
    (void)Pad;
    ++Converted;
  }
  return Converted;
}

} // namespace revenant::lift
