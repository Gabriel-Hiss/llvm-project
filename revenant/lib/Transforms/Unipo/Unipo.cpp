//===- Unipo.cpp - undo interprocedural (IPO) optimizations ----------===//
//
// Part of the Revenant LLVM fork.
//
// Module pass: changing a signature needs a new Function object, which a
// FunctionPassManager pass cannot erase safely mid-iteration (LLVM's own
// ArgPromotion and DeadArgElim are module passes for the same reason).
//
// Structural.cpp folds equal-signature bodies only when distinct constant-data
// references and equivalent trap tails support clone identity. It does not
// infer source formals from caller constants. This file restores returns and
// arguments only when debug or named-clone evidence provides the missing
// signature shape; folded-global recovery handles the narrow GCC constprop
// case. Calls copied during signature changes retain instruction metadata and
// VA provenance.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unipo/Unipo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace revenant {
bool foldStructuralIPO(Module &M);
// Constant equivalence modulo dedup'd constant data and trap-stub
// declarations (Structural.cpp).
bool constantsEquivalent(const Constant *A, const Constant *B);
}

namespace {

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (!Src || !Dst)
    return;
  if (MDNode *N = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", N);
  if (Src->getDebugLoc())
    Dst->setDebugLoc(Src->getDebugLoc());
}

bool isUnipoSafeCall(const CallBase *CB, const Function *F) {
  if (CB->isMustTailCall() || CB->isTailCall())
    return false;
  if (isa<CallBrInst>(CB))
    return false;
  if (CB->isIndirectCall())
    return false;
  if (CB->getCalledFunction() != F)
    return false;
  if (isa<IntrinsicInst>(CB))
    return false;
  if (CB->hasMetadata("revenant.opaque"))
    return false;
  if (const Function *Callee = CB->getCalledFunction()) {
    if (Callee->getName().starts_with("revenant.asm."))
      return false;
  }
  return true;
}

// Every use of F must be a direct, rewritable call. Mirrors the
// revenant-unabi call-site guard.
bool hasBadCallSite(const Function &F) {
  for (const User *U : F.users()) {
    const auto *CB = dyn_cast<CallBase>(U);
    if (!CB || !isUnipoSafeCall(CB, &F))
      return true;
  }
  return false;
}

// Calls that veto any rewrite: tail/callbr/opaque/intrinsics handled
// below. Plain calls are fine (the return-restore shape ENDS in a dead
// computed call result, e.g. errfile's lua_settop issue).
bool callsOpaqueOrSpecial(const Function &F, bool AllowCalls) {
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (CB->isMustTailCall() || CB->isTailCall())
        return true;
      if (isa<CallBrInst>(CB))
        return true;
      if (CB->hasMetadata("revenant.opaque"))
        return true;
      if (const Function *Callee = CB->getCalledFunction()) {
        if (Callee->getName().starts_with("revenant.asm."))
          return true;
        if (Callee->isIntrinsic())
          continue;
        if (AllowCalls)
          continue;
        return true;
      }
    }
  }
  return false;
}

// A value is a plausible eliminated return: side-effect free (or a call
// the binary really executes whose result the callers drop), defined
// inside F, and dead. Corpus shape: errfile's post body ends
// `tail call void @lua_settop(...); ret void` where pre ended
// `call void @lua_settop(...); ret i32 6` — the surviving trace is the
// last computed value in the block even when it still feeds no return.
// Unanimity across returns is checked by the caller.
bool isDeadReturnCandidate(Value *V, Function &F) {
  if (!V)
    return false;
  if (isa<Constant>(V))
    return true;
  auto *I = dyn_cast<Instruction>(V);
  if (!I || I->getParent() == nullptr ||
      I->getParent()->getParent() != &F)
    return false;
  if (!I->getType()->isIntegerTy() && !I->getType()->isPointerTy() &&
      !I->getType()->isFloatTy() && !I->getType()->isDoubleTy())
    return false;
  if (isa<AllocaInst>(I) || isa<PHINode>(I))
    return false;
  if (I->use_empty())
    return true;
  for (User *U : I->users()) {
    if (auto *RI = dyn_cast<ReturnInst>(U)) {
      if (RI->getParent()->getParent() != &F)
        return false;
      continue;
    }
    return false;
  }
  return true;
}

// Find the single dead value every `ret void` block ends with: the last
// non-terminator, non-intrinsic, non-dbg instruction with a non-void
// type whose only uses are returns. Corpus shape: errfile's post body
// ends `tail call void @lua_settop(...); ret void` where pre ended
// `call void @lua_settop(...); ret i32 6` — but after SROA the constant
// pool for `6` is gone, so what remains is the dead computed value (a
// call result, a load, or a phi). Requires unanimity across returns.
Value *findDeadReturnValue(Function &F) {
  Value *Cand = nullptr;
  bool SawRet = false;
  for (BasicBlock &BB : F) {
    auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!RI)
      continue;
    SawRet = true;
    if (RI->getReturnValue())
      return nullptr; // mixed void/value: not an elimination shape
    Value *Local = nullptr;
    for (auto It = BB.rbegin(), E = BB.rend(); It != E; ++It) {
      Instruction &I = *It;
      if (I.isTerminator())
        continue;
      if (isa<DbgInfoIntrinsic>(I) || isa<LifetimeIntrinsic>(I))
        continue;
      if (I.getType()->isVoidTy())
        continue;
      if (I.hasMetadata("revenant.opaque"))
        return nullptr;
      Local = &I;
      break;
    }
    if (!Local || !isDeadReturnCandidate(Local, F))
      return nullptr;
    if (!Cand)
      Cand = Local;
    else if (Cand != Local)
      return nullptr; // two blocks disagree: not a single dead value
  }
  return SawRet ? Cand : nullptr;
}

// Clone suffixes the optimizers emit. `.llvm.N` is LLVM's plain rename
// (no specialization); it still folds back when the base is absent so
// lifted symbols recover their source name.
bool stripCloneSuffix(StringRef Name, StringRef &Base) {
  static const char *Suffixes[] = {".constprop.", ".isra.",
                                   ".specialized.", ".lto_priv",
                                   ".llvm."};
  for (StringRef S : Suffixes) {
    size_t P = Name.rfind(S);
    if (P == StringRef::npos)
      continue;
    StringRef Tail = Name.substr(P + S.size());
    if (S.back() == '.') {
      // Numbered suffix: all digits.
      unsigned N = 0;
      if (Tail.empty() || Tail.consumeInteger(10, N))
        continue;
      if (!Tail.empty())
        continue;
    } else {
      // `.lto_priv`: optional `.N` trailer.
      if (!Tail.empty()) {
        if (!Tail.starts_with("."))
          continue;
        StringRef Num = Tail.substr(1);
        unsigned N = 0;
        if (Num.empty() || Num.consumeInteger(10, N))
          continue;
        if (!Num.empty())
          continue;
      }
    }
    Base = Name.substr(0, P);
    return true;
  }
  return false;
}

// Rebind only operands that permit runtime values. Switch cases, struct GEP
// indices, intrinsic immargs and other constant-only operands stay constant.
// Operands equivalent to C (dedup'd data globals, equivalent ConstantExprs)
// rebind too: GCC folds the source global, the lifter re-emits it per VA.
void rebindConstantToArg(Function &NewF, Constant *C, Argument *A) {
  if (!C || !A || C->getType() != A->getType())
    return;
  for (BasicBlock &BB : NewF)
    for (Instruction &I : BB) {
      if (isa<BinaryOperator>(I) || isa<CmpInst>(I) || isa<SelectInst>(I) ||
          isa<CastInst>(I) || isa<PHINode>(I) || isa<ReturnInst>(I) ||
          isa<FreezeInst>(I) || isa<StoreInst>(I)) {
        for (unsigned N = 0; N < I.getNumOperands(); ++N) {
          auto *Old = dyn_cast<Constant>(I.getOperand(N));
          if (Old && Old->getType() == C->getType() &&
              revenant::constantsEquivalent(Old, C))
            I.setOperand(N, A);
        }
        continue;
      }
      if (auto *CB = dyn_cast<CallBase>(&I))
        for (unsigned N = 0; N < CB->arg_size(); ++N) {
          auto *Old = dyn_cast<Constant>(CB->getArgOperand(N));
          if (Old && Old->getType() == C->getType() &&
              revenant::constantsEquivalent(Old, C))
            CB->setArgOperand(N, A);
        }
    }
}

// Core signature rewrite: build NewF with NewRet/NewParams, splice the
// body, remap old formals, fix returns and call sites. `InsertVals`
// holds, per new-param index, either the old formal index to forward or
// -1 for a fresh `poison` formal; `Rebind` optionally names a constant
// to rebind to one fresh formal (clone fold-back). Follows the
// revenant-unabi rewrite mechanics.
bool rewriteSignature(Module &M, Function &F, Type *NewRet,
                      ArrayRef<Type *> NewParams,
                      ArrayRef<int64_t> InsertVals, Constant *RebindConst,
                      int64_t RebindParam, StringRef NewName,
                      Value *RetVal = nullptr) {
  LLVMContext &C = M.getContext();
  if (F.isVarArg())
    return false;

  FunctionType *NewFTy =
      FunctionType::get(NewRet, NewParams, /*isVarArg=*/false);
  Function *NewF = Function::Create(NewFTy, F.getLinkage(),
                                    F.getAddressSpace());
  NewF->copyAttributesFrom(&F);
  NewF->setName("");
  NewF->setCallingConv(F.getCallingConv());
  NewF->setVisibility(F.getVisibility());
  NewF->setUnnamedAddr(F.getUnnamedAddr());
  NewF->setDLLStorageClass(F.getDLLStorageClass());
  if (F.hasComdat())
    NewF->setComdat(F.getComdat());
  if (F.hasSection())
    NewF->setSection(F.getSection());
  if (F.hasGC())
    NewF->setGC(F.getGC());
  if (F.hasPersonalityFn())
    NewF->setPersonalityFn(F.getPersonalityFn());
  NewF->setAlignment(F.getAlign());
  if (F.hasPrefixData())
    NewF->setPrefixData(F.getPrefixData());
  if (F.getSubprogram())
    NewF->setSubprogram(F.getSubprogram());
  // The function must live in the module before debug records resolve.
  M.getFunctionList().insert(F.getIterator(), NewF);

  struct Site {
    CallBase *CB;
    SmallVector<Value *, 8> Args;
  };
  SmallVector<Site, 8> Sites;
  for (User *U : F.users()) {
    auto *CB = cast<CallBase>(U);
    Site S;
    S.CB = CB;
    for (Value *A : CB->args())
      S.Args.push_back(A);
    Sites.push_back(std::move(S));
  }

  NewF->splice(NewF->end(), &F);

  // Forward surviving formals; fresh formals start as poison.
  auto OldArgs = NewF->args();
  (void)OldArgs;
  unsigned OldIdx = 0;
  for (unsigned N = 0; N < InsertVals.size(); ++N) {
    Argument *NA = NewF->getArg(N);
    int64_t Src = InsertVals[N];
    if (Src >= 0) {
      Argument *OA = F.getArg((unsigned)Src);
      OA->replaceAllUsesWith(NA);
      if (OA->hasName())
        NA->setName(OA->getName());
      else
        NA->setName("a" + std::to_string(N));
      (void)OldIdx;
    } else {
      NA->setName("ipa" + std::to_string(N));
    }
  }

  // Clone fold-back: the body's folded constant becomes the formal.
  if (RebindConst && RebindParam >= 0)
    rebindConstantToArg(*NewF, RebindConst,
                        NewF->getArg((unsigned)RebindParam));

  // Returns: void -> value restores the eliminated value at every return.
  // RetVal is the pre-splice pointer from findDeadReturnValue; it moved
  // with the body, so compare by pointer. Constants (constprop fold)
  // materialize directly.
  if (!NewRet->isVoidTy() && F.getReturnType()->isVoidTy()) {
    Constant *RetC = dyn_cast_or_null<Constant>(RetVal);
    for (BasicBlock &BB : *NewF) {
      auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
      if (!RI || RI->getReturnValue())
        continue;
      Value *RV = RetC;
      if (!RV) {
        // The dead value moved with the body; find it by pointer first,
        // else take the last non-void value of the right type.
        for (auto It = BB.rbegin(), E = BB.rend(); It != E; ++It) {
          Instruction &I = *It;
          if (I.isTerminator())
            continue;
          if (isa<DbgInfoIntrinsic>(I) || isa<LifetimeIntrinsic>(I))
            continue;
          if (I.getType()->isVoidTy())
            continue;
          if (&I == RetVal || I.getType() == NewRet) {
            RV = &I;
            break;
          }
        }
        if (!RV || RV->getType() != NewRet)
          continue;
      }
      MDNode *VA = RI->getMetadata("revenant.va");
      DebugLoc DL = RI->getDebugLoc();
      BasicBlock *P = RI->getParent();
      RI->eraseFromParent();
      ReturnInst *NV = ReturnInst::Create(C, RV, P);
      if (VA)
        NV->setMetadata("revenant.va", VA);
      if (DL)
        NV->setDebugLoc(DL);
    }
  }

  // Call sites: rebuild each direct call against the new type. Fresh
  // formals take poison (dead by construction); the rebind formal
  // takes the clone's constant where it is one.
  for (Site &St : Sites) {
    CallBase *CB = St.CB;
    SmallVector<Value *, 8> NewArgs;
    unsigned OldPos = 0;
    for (unsigned N = 0; N < InsertVals.size(); ++N) {
      int64_t Src = InsertVals[N];
      if (Src >= 0) {
        NewArgs.push_back(St.Args[OldPos++]);
      } else if ((int64_t)N == RebindParam && RebindConst) {
        NewArgs.push_back(RebindConst);
      } else {
        NewArgs.push_back(PoisonValue::get(NewParams[N]));
      }
    }
    CallBase *NewCB = nullptr;
    if (auto *CI = dyn_cast<CallInst>(CB)) {
      CallInst *NC = CallInst::Create(NewFTy, NewF, NewArgs);
      NC->insertAfter(CI->getIterator());
      NC->copyMetadata(*CI);
      NC->setCallingConv(CI->getCallingConv());
      if (!CI->getType()->isVoidTy() && !NewFTy->getReturnType()->isVoidTy())
        NC->setName(CI->getName());
      NewCB = NC;
    } else {
      auto *II = cast<InvokeInst>(CB);
      InvokeInst *NI = InvokeInst::Create(NewFTy, NewF, II->getNormalDest(),
                                          II->getUnwindDest(), NewArgs);
      NI->insertAfter(II->getIterator());
      NI->copyMetadata(*II);
      NI->setCallingConv(II->getCallingConv());
      if (!II->getType()->isVoidTy() && !NewFTy->getReturnType()->isVoidTy())
        NI->setName(II->getName());
      NewCB = NI;
    }
    if (CB->getType() == NewCB->getType())
      NewCB->setAttributes(CB->getAttributes());
    copyVA(CB, NewCB);
    if (CB->getDebugLoc())
      NewCB->setDebugLoc(CB->getDebugLoc());
    if (!CB->use_empty()) {
      if (NewCB->getType() == CB->getType())
        CB->replaceAllUsesWith(NewCB);
      else if (!NewRet->isVoidTy() && CB->getType()->isVoidTy()) {
        // Old void call, new value ignored (same as unabi sret out).
      } else
        CB->replaceAllUsesWith(PoisonValue::get(CB->getType()));
    }
    CB->eraseFromParent();
  }

  // DI formal markers that pointed past the old arity now resolve:
  // nothing to patch explicitly; the DISubprogram type still describes
  // the source shape (rust dot4 keeps !{!173, !1885, !1885}).
  std::string Name =
      NewName.empty() ? F.getName().str() : NewName.str();
  // Strip the DISubprogram linkage mismatch on rename: the name now
  // matches the source symbol again.
  F.eraseFromParent();
  NewF->setName(Name);
  return true;
}

// A dead value proves what the optimized body computes, not that it was the
// source function's return value. Use scalar return metadata to reject sret,
// constructor and pointer-return shapes that can leave unrelated dead values.
bool returnMatchesDebugType(Module &M, Function &F, Type *ValueTy) {
  DISubprogram *SP = F.getSubprogram();
  auto *ST = SP ? dyn_cast_or_null<DISubroutineType>(SP->getType()) : nullptr;
  if (!ST || ST->getTypeArray().size() == 0)
    return false;
  DIType *Ty = dyn_cast_or_null<DIType>(ST->getTypeArray()[0]);
  while (auto *Derived = dyn_cast_or_null<DIDerivedType>(Ty)) {
    switch (Derived->getTag()) {
    case dwarf::DW_TAG_pointer_type:
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type: {
      unsigned Bits = M.getDataLayout().getPointerSizeInBits(0);
      return ValueTy->isIntegerTy(Bits) || ValueTy->isPointerTy();
    }
    case dwarf::DW_TAG_typedef:
    case dwarf::DW_TAG_const_type:
    case dwarf::DW_TAG_volatile_type:
    case dwarf::DW_TAG_restrict_type:
    case dwarf::DW_TAG_atomic_type:
      Ty = Derived->getBaseType();
      continue;
    default:
      return false;
    }
  }
  auto *Basic = dyn_cast_or_null<DIBasicType>(Ty);
  if (!Basic)
    return false;
  unsigned Bits = Basic->getSizeInBits();
  if (Basic->getEncoding() == dwarf::DW_ATE_float)
    return (Bits == 32 && ValueTy->isFloatTy()) ||
           (Bits == 64 && ValueTy->isDoubleTy());
  return Bits && ValueTy->isIntegerTy(Bits);
}

// Rule 1: restore a dead return only when scalar DI agrees with its ABI type.
bool tryRestoreReturn(Module &M, Function &F) {
  if (!F.getReturnType()->isVoidTy())
    return false;
  if (F.empty() || hasBadCallSite(F) || callsOpaqueOrSpecial(F, true))
    return false;
  for (Argument &A : F.args()) {
    if (A.hasAttribute(Attribute::StructRet) || A.hasAttribute(Attribute::ByVal))
      return false;
  }
  Value *Dead = findDeadReturnValue(F);
  if (!Dead || !returnMatchesDebugType(M, F, Dead->getType()))
    return false;
  Type *RT = Dead->getType();
  if (!RT->isIntegerTy() && !RT->isPointerTy() && !RT->isFloatTy() &&
      !RT->isDoubleTy())
    return false;
  SmallVector<Type *, 8> Params;
  for (Argument &A : F.args())
    Params.push_back(A.getType());
  SmallVector<int64_t, 8> Fwd;
  for (unsigned I = 0; I < Params.size(); ++I)
    Fwd.push_back((int64_t)I);
  return rewriteSignature(M, F, RT, Params, Fwd, /*RebindConst=*/nullptr,
                          /*RebindParam=*/-1, /*NewName=*/"", Dead);
}

// Restore a trailing formal only when an optimized-out parameter has an
// explicit poison `dbg.value` marker. DISubprogram arity can retain stale
// source types for LTO helpers that never had the extra formal.
bool tryRestoreDeadArg(Module &M, Function &F) {
  if (F.isVarArg() || hasBadCallSite(F) || callsOpaqueOrSpecial(F, false))
    return false;
  unsigned Have = F.arg_size();
  unsigned Want = Have;
  Type *WantTy = nullptr;

  auto RestoreFromMarker = [&](Value *V, DILocalVariable *Var) {
    if (!Var || !Var->isParameter() || Var->getArg() == 0 ||
        (V && !isa<PoisonValue>(V) && !isa<UndefValue>(V)))
      return;
    if (Var->getArg() <= Want)
      return;
    Want = (unsigned)Var->getArg();
    if (!WantTy)
      if (auto *DT = dyn_cast_or_null<DIDerivedType>(Var->getType()))
        if (DT->getTag() == dwarf::DW_TAG_pointer_type)
          WantTy = PointerType::getUnqual(M.getContext());
  };
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      if (auto *DVI = dyn_cast<DbgValueInst>(&I))
        RestoreFromMarker(DVI->getValue(), DVI->getVariable());
      for (DbgRecord &Record : I.getDbgRecordRange())
        if (auto *DVR = dyn_cast<DbgVariableRecord>(&Record))
          if (DVR->isDbgValue())
            RestoreFromMarker(DVR->getValue(), DVR->getVariable());
    }
  if (Want > Have + 4)
    return false; // garbage DI: bail rather than invent formals
  if (Want <= Have)
    return false;
  if (!WantTy)
    WantTy = PointerType::getUnqual(M.getContext());
  // Only restore trailing formals (DeadArgElim drops from the back
  // after reordering; mid-list drops are constprop clones, rule 3).
  SmallVector<Type *, 8> Params;
  for (Argument &A : F.args())
    Params.push_back(A.getType());
  while (Params.size() < Want)
    Params.push_back(WantTy);
  SmallVector<int64_t, 8> Fwd;
  for (unsigned I = 0; I < Have; ++I)
    Fwd.push_back((int64_t)I);
  for (unsigned I = Have; I < Want; ++I)
    Fwd.push_back(-1);
  return rewriteSignature(M, F, F.getReturnType(), Params, Fwd,
                          /*RebindConst=*/nullptr, /*RebindParam=*/-1, "");
}

// Rule 3: clone fold-back. `base.constprop.N` / `.isra.N` /
// `.specialized.N` / `.lto_priv` / `.llvm.N` with no live `base` in
// the module is renamed to `base`; when the post body returns a
// constant the DISubprogram/DILocalVariable records name the lost
// formals (rule 2 finishes the job on the next iteration).
bool tryFoldClone(Module &M, Function &F) {
  StringRef Base;
  if (!stripCloneSuffix(F.getName(), Base))
    return false;
  if (Base.empty() || M.getFunction(Base))
    return false;
  if (F.empty() || hasBadCallSite(F) || callsOpaqueOrSpecial(F, false))
    return false;
  // The clone must be internal-ish (a real export keeps its name).
  if (F.hasExternalLinkage() && F.hasDLLExportStorageClass())
    return false;
  SmallVector<Type *, 8> Params;
  for (Argument &A : F.args())
    Params.push_back(A.getType());
  SmallVector<int64_t, 8> Fwd;
  for (unsigned I = 0; I < Params.size(); ++I)
    Fwd.push_back((int64_t)I);
  return rewriteSignature(M, F, F.getReturnType(), Params, Fwd,
                          /*RebindConst=*/nullptr, /*RebindParam=*/-1,
                          Base);
}


// Peel a constant expression down to the global it derives from. Only
// transparent casts qualify: a ConstantExpr mixing a global with other
// data (add/sub, GEP past the head) is not a whole-object reference.
GlobalVariable *globalBehind(const Constant *C) {
  if (const auto *GV = dyn_cast<GlobalVariable>(C))
    return const_cast<GlobalVariable *>(GV);
  if (const auto *E = dyn_cast<ConstantExpr>(C)) {
    if (E->getNumOperands() == 1 &&
        (E->getOpcode() == Instruction::PtrToInt ||
         E->getOpcode() == Instruction::BitCast ||
         E->getOpcode() == Instruction::AddrSpaceCast))
      if (const auto *Op = dyn_cast<Constant>(E->getOperand(0)))
        return globalBehind(Op);
  }
  return nullptr;
}

// True when C contains any global-bearing leaf (used to veto functions
// that touch a second data object).
bool containsGlobal(const Constant *C) {
  if (isa<GlobalValue>(C))
    return true;
  if (const auto *E = dyn_cast<ConstantExpr>(C))
    for (const Value *V : E->operand_values())
      if (const auto *Op = dyn_cast<Constant>(V))
        if (containsGlobal(Op))
          return true;
  return false;
}

// Folded-global argument restoration (clone-only constprop). A GCC
// constprop clone of a single-object function keeps the folded object
// reference inline: the body passes `ptrtoint(@g)` (or an equivalent
// dedup'd global) to its callees, once per use, where the general
// version read `this`. The signature evidence is narrow on purpose:
// exactly one surviving formal, callers that disagree on it (so no
// caller-side constant is the specialization), and exactly one
// equivalence class of referenced data global, appearing at least twice
// as a call argument and nowhere else. That is `this`/the singleton the
// specialization folded; restoring it as formal 0 and passing it at
// every call site is a plain rebind, equivalent by construction.
// Zero-caller clones restore the same signature (the rebind is
// semantics-preserving even with no sites to update).
bool restoreFoldedGlobal(Module &M, Function &F) {
  if (F.isDeclaration() || F.isVarArg() || F.arg_size() != 1 ||
      F.hasDLLExportStorageClass() || hasBadCallSite(F) ||
      callsOpaqueOrSpecial(F, true))
    return false;
  for (Argument &A : F.args())
    if (A.hasAttribute(Attribute::StructRet) ||
        A.hasAttribute(Attribute::ByVal))
      return false;
  Constant *Bound = nullptr;
  unsigned ArgUses = 0;
  SmallPtrSet<GlobalVariable *, 4> Class;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      for (unsigned K = 0; K < I.getNumOperands(); ++K) {
        auto *C = dyn_cast<Constant>(I.getOperand(K));
        if (!C || !containsGlobal(C))
          continue;
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (K >= CB->arg_size())
            continue; // the callee operand itself is not evidence
          if (GlobalVariable *G = globalBehind(C)) {
            // A folded `this` is the first call argument. Buffers and
            // other global operands often occupy later ABI positions.
            if (K != 0)
              return false;
            Class.insert(G);
            ++ArgUses;
            if (!Bound)
              Bound = C;
            continue;
          }
        }
        return false; // a second data object, or a non-argument global use
      }
    }
  if (ArgUses < 2 || Class.size() != 1 || !Bound)
    return false;
  SmallVector<Type *, 4> Params;
  Params.push_back(Bound->getType());
  Params.push_back(F.getArg(0)->getType());
  SmallVector<int64_t, 4> Forward = {-1, 0};
  return rewriteSignature(M, F, F.getReturnType(), Params, Forward,
                          Bound, /*RebindParam=*/0, "");
}

bool runOnModule(Module &M) {
  bool Changed = revenant::foldStructuralIPO(M);
  SmallVector<std::string, 16> Names;
  for (Function &F : M)
    if (!F.isDeclaration())
      Names.push_back(F.getName().str());
  for (const std::string &Name : Names)
    if (Function *F = M.getFunction(Name))
      Changed |= restoreFoldedGlobal(M, *F);
  SmallVector<Function *, 16> Work;
  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    Work.push_back(&F);
  }
  // Clone fold-back first (names must settle before return/arg rules
  // read DISubprogram records), then returns, then dead args. One
  // function per iteration: every rewrite erases the Function.
  for (Function *F : Work) {
    if (F->getParent() != &M)
      continue;
    if (F->isDeclaration())
      continue;
    Changed |= tryFoldClone(M, *F);
  }
  Work.clear();
  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    Work.push_back(&F);
  }
  for (Function *F : Work) {
    if (F->getParent() != &M)
      continue;
    if (F->isDeclaration())
      continue;
    Changed |= tryRestoreReturn(M, *F);
  }
  Work.clear();
  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    Work.push_back(&F);
  }
  for (Function *F : Work) {
    if (F->getParent() != &M)
      continue;
    if (F->isDeclaration())
      continue;
    Changed |= tryRestoreDeadArg(M, *F);
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UnipoPass::run(Module &M,
                                          ModuleAnalysisManager &) {
  if (runOnModule(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// links this library statically and registers the `revenant-unipo` name in
// Pipeline/Register.cpp, so the entry point is compiled only for the
// standalone plugin DLL.
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
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnipo", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unipo") {
                    MPM.addPass(revenant::UnipoPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
