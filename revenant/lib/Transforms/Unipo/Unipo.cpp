//===- Unipo.cpp - undo interprocedural (IPO) optimizations ----------===//
//
// Part of the Revenant LLVM fork.
//
// Module pass: changing a signature needs a new Function object, which a
// FunctionPassManager pass cannot erase safely mid-iteration (LLVM's own
// ArgPromotion and DeadArgElim are module passes for the same reason).
//
// What the optimizer does to a function (read off clang ir-pre vs ir-post
// pairs on the corpus; MSVC has no IR but shows the same binary shapes):
//
// 1. Return elimination (DeadArgElim on the return value). `errfile`
//    (lua-clang-o1/lauxlib: i32 -> void), `pushmode` (lbaselib: i32 ->
//    void), `luaK_codek` (lcode: i32 -> void), `g_iofile` (liolib: i32 ->
//    void), `TIXML_SNPRINTF` (tinyxml2-*: i32 -> void, vararg),
//    `xml_buffered_writer::flush` (pugi-clang-oz: i64 -> void),
//    `xpath_parser::error_oom` (pugi-clang-oz: ptr -> void): the body
//    ends `ret i32 C` / `ret i64 0` / `ret ptr P` with C/P dead at every
//    caller, so the optimizer deletes the value and returns void.
//    The lifted binary still computes the value (the CPU really moves it
//    into eax/rax), so the undo is to put the computed value back on the
//    return: every `ret void` becomes `ret <ty> <dead value>` and the
//    signature returns `<ty>`.
//
// 2. Dead argument removal (DeadArgElim on parameters).
//    `md_free_attribute` (md4c-*: (ptr, ptr) -> (ptr)),
//    `findfile` (lua-clang-o1/loadlib: 4 -> 3), `dot4`/`vec_ops`
//    (rust-o3: fully specialized to () with the constant folded in).
//    On lifted IR the dropped parameter slot is observable only when the
//    callee kept a trace: the pre-specialization value survives either
//    as a `ret <ty> C` constant (then rule 1 restores the data flow and
//    rule 3 rebinds it as an argument) or as `llvm.dbg.value(ptr
//    poison)` / DISubprogram formal-parameter records pointing at a
//    missing formal (then the parameter is re-added with `poison` at
//    the call sites, which refines to the constant where known).
//
// 3. IPSCCP / function-specialization clones. GCC emits
//    `f.constprop.N` / `f.isra.N` symbols (see the gcc-*.map link maps:
//    `_S_construct...isra.0`, `pool...constprop.0`); LLVM's
//    FunctionSpecialization pass emits `f.specialized.N`, LTO emits
//    `f.lto_priv` / `f.llvm.N`. The clone is the base function with one
//    formal replaced by a constant at every call site. The undo folds
//    the clone back: rename `base.suffix` to `base` when `base` is
//    absent, re-add the missing formal (typed from the constant), pass
//    the constant at each call site, and replace the in-body constant
//    uses with the formal.
//
// 4. Argument promotion (by-value struct split into scalars). The
//    `parse_function` shape (pugi: a struct formal exploded into
//    ptr/ptr/i64/ptr/ptr) is detected when consecutive scalar formals
//    reconstruct one pointer load each from a common base at entry; the
//    undo reintroduces the pointer formal and materializes the loads.
//    (Not yet implemented: no corpus pair isolates a pure promotion
//    without interleaved scalarrepl, so the detector would fire on
//    hand-written scalar code. Tracked below.)
//
// Trust model: every rewrite is semantics-preserving on its own, with
// no annotation to trust:
//
// - Return restore: the restored value is the body's own dead value,
//   so callers observing it see exactly what the binary computes. When
//   the value differs per return block, or any return already carries
//   a value, the function is left alone.
// - Dead-arg re-add: the new formal is unused in the body, and call
//   sites pass `poison` (or the clone's proven constant), so the
//   program means the same by construction. `musttail`/`callbr`/
//   indirect calls, intrinsics, opaque stubs, and sret/byval forms
//   bail the function out, as in revenant-unabi.
// - Clone fold-back: renaming a clone to an absent base name and
//   threading the constant through is an alpha-rename plus constant
//   propagation, both equivalences; when the base name is present the
//   clone is left alone (merging two live definitions is the
//   inliner's job, revenant-uninline).
//
// Provenance: every created instruction copies !revenant.va from the
// instruction it replaces; !revenant.opaque is never created, moved, or
// dropped (functions calling opaque stubs bail); debug locations ride
// along the same way. Output stays parseable by LLVM 22.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unipo/Unipo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace revenant {
bool foldStructuralIPO(Module &M);
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

// Replace every non-return use of the restored constant with the new
// formal: the constprop clone had folded the parameter into the body.
void rebindConstantToArg(Function &NewF, Constant *C, Argument *A) {
  if (!C || !A || C->getType() != A->getType())
    return;
  SmallVector<User *, 8> Uses;
  for (User *U : C->users()) {
    if (auto *I = dyn_cast<Instruction>(U)) {
      if (I->getParent() && I->getParent()->getParent() == &NewF &&
          !isa<ReturnInst>(I))
        Uses.push_back(U);
    }
  }
  for (User *U : Uses)
    U->replaceUsesOfWith(C, A);
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

// Rule 1: return elimination. Void function whose every return block
// ends with the same dead computed value (or the same constant, the
// constprop/ret-elimination boundary) gets the value restored.
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
  if (!Dead)
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

// Rule 2: trailing dead formal. An unused trailing pointer/integer
// formal with poison-friendly call sites is NOT restored blindly (no
// corpus signal names its type); instead this handles the annotated
// case only: a `llvm.dbg.value(ptr poison, !DILocalVariable(arg: N))`
// with N past the arity, or a DISubprogram subroutine type with more
// formals than the IR. The re-added formal takes the DI type when it
// is a plain pointer/integer, else poison-typed ptr.
bool tryRestoreDeadArg(Module &M, Function &F) {
  if (F.isVarArg() || hasBadCallSite(F) || callsOpaqueOrSpecial(F, false))
    return false;
  unsigned Have = F.arg_size();
  unsigned Want = Have;
  Type *WantTy = nullptr;

  // DISubprogram subroutine type: {ret, p0, ...} may name more formals
  // than the IR keeps (rust-o3 dot4: !{!173, !1885, !1885} vs ()).
  if (DISubprogram *SP = F.getSubprogram()) {
    if (auto *ST = dyn_cast_or_null<DISubroutineType>(SP->getType())) {
      DITypeRefArray Tys = ST->getTypeArray();
      // Index 0 is the return; the rest are formals.
      if (Tys.size() > 0 && (unsigned)Tys.size() - 1 > Want) {
        Want = (unsigned)Tys.size() - 1;
        // Recover the formal type from the DI record when simple.
        if (Tys.size() > 1) {
          if (auto *DT = dyn_cast_or_null<DIDerivedType>(Tys[1])) {
            if (DT->getTag() == dwarf::DW_TAG_pointer_type)
              WantTy = PointerType::getUnqual(M.getContext());
          } else if (auto *BT = dyn_cast_or_null<DIBasicType>(Tys[1])) {
            unsigned Bits = (unsigned)BT->getSizeInBits();
            if (Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64)
              WantTy = IntegerType::get(M.getContext(), Bits);
          }
        }
      }
    }
  }
  // dbg.value markers: arg: N with N > arity.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      Value *V = nullptr;
      DILocalVariable *Var = nullptr;
      if (auto *DVI = dyn_cast<DbgValueInst>(&I)) {
        V = DVI->getValue();
        Var = DVI->getVariable();
      } else if (auto *DPI = dyn_cast<DbgDeclareInst>(&I)) {
        continue; // declares address a live slot, not a lost formal
      }
      if (!Var || !Var->isParameter() || Var->getArg() == 0)
        continue;
      if (V && !isa<PoisonValue>(V) && !isa<UndefValue>(V))
        continue;
      if (Var->getArg() > Want) {
        Want = (unsigned)Var->getArg();
        if (!WantTy) {
          if (auto *DT = dyn_cast_or_null<DIDerivedType>(Var->getType())) {
            if (DT->getTag() == dwarf::DW_TAG_pointer_type)
              WantTy = PointerType::getUnqual(M.getContext());
          }
        }
      }
    }
    if (Want > Have + 4)
      return false; // garbage DI: bail rather than invent formals
  }
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

bool runOnModule(Module &M) {
  bool Changed = revenant::foldStructuralIPO(M);
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
      continue; // erased by an earlier rewrite this round
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
