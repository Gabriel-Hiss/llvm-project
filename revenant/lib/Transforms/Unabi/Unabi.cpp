//===- Unabi.cpp - undo Win64 ABI lowering ----------------------------===//
//
// Part of the Revenant LLVM fork.
//
// Module pass: changing a signature needs a new Function object, which a
// FunctionPassManager pass cannot erase safely mid-iteration (LLVM's own
// ArgPromotion and DeadArgElim are module passes for the same reason).
//
// The lifter emits the flat ABI form: every integer register arrives as i64
// (sigs over-approximates widths because one stray 64-bit read widens the
// whole parameter), floats as f64, pointers as either i64 or ptr, hidden
// sret slots as plain pointer params with void returns, small returned
// structs coerced to i64 in RAX, methods with no `this`, ctors returning
// their own receiver, and varargs without `...`. This pass rewrites each
// annotated function (plus its direct call sites, which it owns because
// they name the old type) back toward the clang-cl ir-pre shape for the
// same source construct.
//
// Trust model: the "revenant.sig" attribute is trusted the way sigs are
// trusted. Value rewrites (integer narrowing, i64-to-ptr, arg-promotion
// undo) assume the annotation names the true source type. Shape rewrites
// are additionally guarded on the IR itself: the sret slot may only feed
// stores (any load, escape, or non-constant field addressing bails), a `t`
// return must provably hand back param 0 on every path (else no-fire),
// pair bitcopies require the struct store size to equal the old return
// width, and any indirect use, musttail call, callbr, intrinsic, opaque
// stub, or pre-existing sret/byval attribute bails the whole function out.
// Consumed attributes are removed on success and kept on no-fire so a
// re-run is a no-op either way. V (struct-by-pointer, callee owns a copy)
// keeps a plain ptr: ir-pre carries no byval marker on Win64, and the
// statically-linked slice plugin cannot add param attributes (nodes it
// interns are invisible to the in-process verifier), so ownership stays
// with sigs for now (open question with MuseSigs2).
//
// Provenance: every created instruction copies !revenant.va from the
// instruction it replaces (call-site argument shuffles copy from the call
// itself); !revenant.opaque is never created, moved, or dropped because
// opaque-stub calls bail before any rewrite. Debug locations are copied
// the same way. The new function keeps the old linkage, visibility,
// alignment, address space, calling convention, unnamed-addr, DLL storage,
// attributes (minus consumed "revenant.sig"), comdat, personality, prefix
// data, and section/gc names. Entry-block conversions sit after any static
// alloca so placement stays valid for later mem2reg.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unabi/Unabi.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Debug.h"
#include <cassert>

using namespace llvm;

namespace {

constexpr StringLiteral SigAttr = "revenant.sig";

struct ParamTok {
  enum Kind {
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    Ptr,
    Stack,
    This,
    Sret,
    ByVal,
    Vararg
  };
  Kind K = I64;
  // Sret/ByVal struct name (LLVM struct name, no quotes).
  std::string Name;
};

struct RetTok {
  enum Kind { Void, I8, I16, I32, I64, F32, F64, Ptr, Struct, Pair, ThisRet };
  Kind K = Void;
  // Struct/Pair name.
  std::string Name;
};

struct Sig {
  SmallVector<ParamTok, 8> Params;
  RetTok Ret;
  bool Vararg = false;
};

// Parse one param token; Offset receives the s0x<off> value (kept only for
// grep-ability of the home-space slot; the rewrite is positional).
bool parseParam(StringRef T, ParamTok &Out, uint64_t &Offset) {
  if (T == "r8" || T == "i8") {
    Out.K = ParamTok::I8;
    return true;
  }
  if (T == "r16" || T == "i16") {
    Out.K = ParamTok::I16;
    return true;
  }
  if (T == "r32" || T == "i32") {
    Out.K = ParamTok::I32;
    return true;
  }
  if (T == "r64" || T == "i64") {
    Out.K = ParamTok::I64;
    return true;
  }
  if (T == "f32") {
    Out.K = ParamTok::F32;
    return true;
  }
  if (T == "f64") {
    Out.K = ParamTok::F64;
    return true;
  }
  if (T == "p") {
    Out.K = ParamTok::Ptr;
    return true;
  }
  if (T == "T") {
    Out.K = ParamTok::This;
    return true;
  }
  if (T == "...") {
    Out.K = ParamTok::Vararg;
    return true;
  }
  if (T.consume_front("s0x")) {
    if (T.getAsInteger(16, Offset))
      return false;
    Out.K = ParamTok::Stack;
    return true;
  }
  if (T.consume_front("S")) {
    if (T.empty())
      return false;
    T.consume_front("%");
    if (T.empty())
      return false;
    Out.K = ParamTok::Sret;
    Out.Name = T.str();
    return true;
  }
  if (T.consume_front("V")) {
    if (T.empty())
      return false;
    T.consume_front("%");
    if (T.empty())
      return false;
    // Struct passed by pointer where the callee owns a copy. Win64 clang
    // passes these as plain pointers (ir-pre carries no byval marker),
    // so the rewrite keeps a plain ptr; the name only validates that the
    // annotation refers to a real struct type (checked in rewriteFunction).
    Out.K = ParamTok::ByVal;
    Out.Name = T.str();
    return true;
  }
  return false;
}

bool parseRet(StringRef T, RetTok &Out) {
  if (T == "v") {
    Out.K = RetTok::Void;
    return true;
  }
  if (T == "r8" || T == "i8") {
    Out.K = RetTok::I8;
    return true;
  }
  if (T == "r16" || T == "i16") {
    Out.K = RetTok::I16;
    return true;
  }
  if (T == "r32" || T == "i32") {
    Out.K = RetTok::I32;
    return true;
  }
  if (T == "r64" || T == "i64") {
    Out.K = RetTok::I64;
    return true;
  }
  if (T == "f32") {
    Out.K = RetTok::F32;
    return true;
  }
  if (T == "f64") {
    Out.K = RetTok::F64;
    return true;
  }
  if (T == "p") {
    Out.K = RetTok::Ptr;
    return true;
  }
  if (T == "t") {
    Out.K = RetTok::ThisRet;
    return true;
  }
  if (T.consume_front("S")) {
    if (T.empty())
      return false;
    T.consume_front("%");
    if (T.empty())
      return false;
    Out.K = RetTok::Struct;
    Out.Name = T.str();
    return true;
  }
  if (T.consume_front("P")) {
    if (T.empty())
      return false;
    T.consume_front("%");
    if (T.empty())
      return false;
    Out.K = RetTok::Pair;
    Out.Name = T.str();
    return true;
  }
  return false;
}

// "p p r32 => r32" or bare "r64 r64 s0x28 v" (last token is the return).
bool parseSig(StringRef S, Sig &Out) {
  SmallVector<StringRef, 16> Toks;
  S.split(Toks, ' ', -1, false);
  if (Toks.empty())
    return false;
  SmallVector<StringRef, 16> Params;
  StringRef Ret;
  bool HasArrow = false;
  for (unsigned I = 0; I < Toks.size(); ++I) {
    if (Toks[I] == "=>") {
      if (HasArrow)
        return false;
      HasArrow = true;
      Params.append(Toks.begin(), Toks.begin() + I);
      if (I + 2 != Toks.size())
        return false;
      Ret = Toks[I + 1];
      break;
    }
  }
  if (!HasArrow) {
    if (Toks.size() < 2)
      return false;
    Params.append(Toks.begin(), Toks.end() - 1);
    Ret = Toks.back();
  }
  for (StringRef T : Params) {
    if (T == "...") {
      if (Out.Vararg)
        return false;
      Out.Vararg = true;
      continue;
    }
    if (Out.Vararg)
      return false; // ... must be last
    ParamTok P;
    uint64_t Off = 0;
    if (!parseParam(T, P, Off))
      return false;
    Out.Params.push_back(std::move(P));
  }
  if (!parseRet(Ret, Out.Ret))
    return false;
  return true;
}

Type *paramType(LLVMContext &C, const ParamTok &P) {
  switch (P.K) {
  case ParamTok::I8:
    return Type::getInt8Ty(C);
  case ParamTok::I16:
    return Type::getInt16Ty(C);
  case ParamTok::I32:
    return Type::getInt32Ty(C);
  case ParamTok::I64:
    return Type::getInt64Ty(C);
  case ParamTok::F32:
    return Type::getFloatTy(C);
  case ParamTok::F64:
    return Type::getDoubleTy(C);
  case ParamTok::Ptr:
  case ParamTok::Stack:
  case ParamTok::This:
  case ParamTok::Sret:
  case ParamTok::ByVal:
    return PointerType::getUnqual(C);
  case ParamTok::Vararg:
    return nullptr;
  }
  return nullptr;
}

// Named struct lookup that works whether or not the type is used yet:
// scan the module's identified types. Accepts the name with or without a
// leading '%' (the sig grammar strips it, the module keeps it).
// Returns null when absent.
StructType *findStruct(Module &M, StringRef Name) {
  StringRef Bare = Name;
  Bare.consume_front("%");
  for (StructType *ST : M.getIdentifiedStructTypes()) {
    if (!ST->hasName())
      continue;
    StringRef Have = ST->getName();
    Have.consume_front("%");
    if (Have == Bare)
      return ST;
  }
  return nullptr;
}

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (!Src || !Dst)
    return;
  if (MDNode *N = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", N);
  if (Src->getDebugLoc())
    Dst->setDebugLoc(Src->getDebugLoc());
}
// Field addressing only: bitcast, GEP with all-constant indices. Collects
// every user reachable through those; anything else is recorded as itself
// so the caller can reject it.
void collectFieldUses(Value *V, SmallVectorImpl<User *> &Out) {
  SmallVector<Value *, 16> Work{V};
  SmallVector<Value *, 16> Seen;
  while (!Work.empty()) {
    Value *C = Work.pop_back_val();
    bool Known = false;
    for (Value *S : Seen)
      if (S == C) {
        Known = true;
        break;
      }
    if (Known)
      continue;
    Seen.push_back(C);
    for (User *U : C->users()) {
      if (auto *BC = dyn_cast<BitCastInst>(U)) {
        Out.push_back(BC);
        Work.push_back(BC);
      } else if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
        Out.push_back(GEP);
        if (GEP->hasAllConstantIndices())
          Work.push_back(GEP);
      } else {
        Out.push_back(U);
      }
    }
  }
}

// The sret slot may only feed stores through field addressing: plain
// stores, or llvm.memcpy/memmove/memset that write the whole slot (the
// common lowering for struct copies in ir-pre, e.g. vector iterators).
// Any load of the slot or of a derived field pointer, or any other use,
// bails.
bool sretSlotIsStoreOnly(Argument *A) {
  SmallVector<User *, 16> Uses;
  collectFieldUses(A, Uses);
  if (Uses.empty())
    return false; // slot never stored: not an sret shape
  for (User *U : Uses) {
    if (auto *SI = dyn_cast<StoreInst>(U)) {
      // Direct store through the slot (possibly after bitcast already
      // unfolded by collectFieldUses): the pointer operand must be the
      // slot or a field address derived from it.
      Value *P = SI->getPointerOperand()->stripPointerCasts();
      if (auto *GEP = dyn_cast<GetElementPtrInst>(P)) {
        if (!GEP->hasAllConstantIndices())
          return false;
        Value *Base = GEP->getPointerOperand()->stripPointerCasts();
        if (Base != A)
          return false;
        continue;
      }
      if (P != A)
        return false;
      continue;
    }
    if (auto *MI = dyn_cast<MemIntrinsic>(U)) {
      // memcpy/memmove write through operand 0, memset through operand 0
      // too; the slot must be the written pointer, not the source.
      Value *Dst = MI->getOperand(0)->stripPointerCasts();
      if (auto *GEP = dyn_cast<GetElementPtrInst>(Dst)) {
        if (!GEP->hasAllConstantIndices())
          return false;
        Dst = GEP->getPointerOperand()->stripPointerCasts();
      }
      if (Dst != A)
        return false;
      if (isa<MemTransferInst>(MI)) {
        // The slot must not also be the copy source (that would read it).
        Value *Src = MI->getOperand(1)->stripPointerCasts();
        if (Src == A)
          return false;
      }
      continue;
    }
    if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U))
      continue; // field address; the store through it is checked above
    return false;
  }
  return true;
}

// Every terminator return hands back P0 (through pointer casts only): the
// ctor/dtor shape.
bool returnsParam0(Function &F, Argument *P0) {
  if (F.empty())
    return false;
  bool Found = false;
  for (BasicBlock &BB : F) {
    auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!RI)
      continue;
    Found = true;
    if (!RI->getReturnValue())
      return false;
    if (RI->getReturnValue()->stripPointerCasts() != P0)
      return false;
  }
  return Found;
}

// Bail when any user cannot be rewritten: non-call users (blockaddress or
// any address-taken use), musttail, callbr, non-direct calls, intrinsics,
// or opaque-stub callers.
bool hasBadCallSite(const Function &F) {
  for (const User *U : F.users()) {
    const auto *CB = dyn_cast<CallBase>(U);
    if (!CB)
      return true;
    if (CB->isMustTailCall() || CB->isTailCall())
      return true;
    if (isa<CallBrInst>(CB))
      return true;
    if (CB->isIndirectCall())
      return true;
    if (CB->getCalledFunction() != &F)
      return true;
    if (isa<IntrinsicInst>(CB))
      return true;
    if (const Function *Callee = CB->getCalledFunction()) {
      if (Callee->getName().starts_with("revenant.asm."))
        return true;
    }
    if (CB->hasMetadata("revenant.opaque"))
      return true;
  }
  return false;
}

// Conversion for a value of type OT to type NT, built before InsertPt.
// INTEGER SIGN: the lifter zero-extends on read, so narrowing truncates
// and widening (reachable when the annotation is wider than the lifted
// type, e.g. stack slots) zero-extends to match lift's ext_to. Sound only
// when the upper bits are dead, which the annotation asserts.
Instruction *buildConversion(Value *V, Type *NT, Instruction *InsertPt,
                             const Twine &Name) {
  Type *OT = V->getType();
  if (OT == NT)
    return nullptr;
  Instruction *I = nullptr;
  if (OT->isIntegerTy() && NT->isIntegerTy()) {
    unsigned OW = OT->getIntegerBitWidth();
    unsigned NW = NT->getIntegerBitWidth();
    if (OW == NW)
      return nullptr;
    I = CastInst::Create(OW < NW ? Instruction::ZExt : Instruction::Trunc,
                         V, NT, Name, InsertPt->getIterator());
  } else if (OT->isIntegerTy() && NT->isPointerTy()) {
    I = CastInst::Create(Instruction::IntToPtr, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isPointerTy() && NT->isIntegerTy()) {
    I = CastInst::Create(Instruction::PtrToInt, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isPointerTy() && NT->isPointerTy()) {
    I = CastInst::Create(Instruction::BitCast, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isFloatTy() && NT->isDoubleTy()) {
    I = CastInst::Create(Instruction::FPExt, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isDoubleTy() && NT->isFloatTy()) {
    I = CastInst::Create(Instruction::FPTrunc, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isIntegerTy(32) && NT->isFloatTy()) {
    I = CastInst::Create(Instruction::BitCast, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isIntegerTy(64) && NT->isDoubleTy()) {
    I = CastInst::Create(Instruction::BitCast, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isFloatTy() && NT->isIntegerTy(32)) {
    I = CastInst::Create(Instruction::BitCast, V, NT, Name,
                         InsertPt->getIterator());
  } else if (OT->isDoubleTy() && NT->isIntegerTy(64)) {
    I = CastInst::Create(Instruction::BitCast, V, NT, Name,
                         InsertPt->getIterator());
  }
  return I;
}

// Rewrite one annotated function. Returns true when the attribute was
// consumed (function replaced).
bool rewriteFunction(Module &M, Function &F, const Sig &S) {
  LLVMContext &C = M.getContext();

  // Positional rule: tokens (excluding ...) match the current arity.
  if (S.Params.size() != F.arg_size())
    return false;
  if (F.isVarArg())
    return false;
  // sret/byval already present: the lifter never emits them, something
  // else owns this function.
  for (Argument &A : F.args()) {
    if (A.hasAttribute(Attribute::StructRet) ||
        A.hasAttribute(Attribute::ByVal))
      return false;
  }
  if (hasBadCallSite(F))
    return false;
  // A tail call inside F returns through the caller's frame: changing F's
  // type would rewrite the callee call but the tail marker still pins the
  // old signature pair.
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->isMustTailCall() || CB->isTailCall())
          return false;
        if (isa<CallBrInst>(CB))
          return false;
      }
    }
  }
  if (F.hasPersonalityFn()) {
    if (isa<BlockAddress>(F.getPersonalityFn()->stripPointerCasts()))
      return false;
  }
  if (F.hasPrefixData()) {
    if (isa<BlockAddress>(F.getPrefixData()->stripPointerCasts()))
      return false;
  }

  // Locate the single sret slot, if any.
  int SretIdx = -1;
  for (unsigned I = 0; I < S.Params.size(); ++I) {
    if (S.Params[I].K == ParamTok::Sret) {
      if (SretIdx >= 0)
        return false;
      SretIdx = (int)I;
    }
  }
  bool WantSretRet = S.Ret.K == RetTok::Struct;
  if ((SretIdx >= 0) != WantSretRet)
    return false;
  StructType *SretTy = nullptr;
  if (SretIdx >= 0) {
    Type *ST = findStruct(M, S.Params[(unsigned)SretIdx].Name);
    Type *RT = findStruct(M, S.Ret.Name);
    SretTy = ST && ST->isStructTy() ? cast<StructType>(ST) : nullptr;
    StructType *RetTy = RT && RT->isStructTy() ? cast<StructType>(RT) : nullptr;
    if (!SretTy || !RetTy || RetTy != SretTy)
      return false;
    if (!F.getReturnType()->isVoidTy())
      return false;
  }

  // RAX-coerced small struct: current return is a single int whose width
  // equals the struct's store size (clang coerces e.g. {float,float} to
  // i64, see ?inputPortPos in node_graph.pre.ll, corpus audio-nodegraph).
  StructType *PairTy = nullptr;
  if (S.Ret.K == RetTok::Pair) {
    Type *PT = findStruct(M, S.Ret.Name);
    PairTy = PT && PT->isStructTy() ? cast<StructType>(PT) : nullptr;
    if (!PairTy || !F.getReturnType()->isIntegerTy())
      return false;
    uint64_t Bits = M.getDataLayout().getTypeStoreSizeInBits(PairTy);
    if (F.getReturnType()->getIntegerBitWidth() != Bits)
      return false;
  }

  // V params keep a plain ptr (ir-pre shape); the named struct must exist
  // so a typo'd annotation bails instead of rewriting blind.
  for (unsigned I = 0; I < S.Params.size(); ++I) {
    if (S.Params[I].K == ParamTok::ByVal) {
      Type *BT = findStruct(M, S.Params[I].Name);
      if (!BT || !BT->isStructTy())
        return false;
    }
  }

  // Ctor/dtor: every return hands back param 0 (annotated as plain ptr;
  // T is for methods with a real return, where param 0 is named `this`).
  bool WantThisRet = S.Ret.K == RetTok::ThisRet;
  Argument *P0 = F.arg_empty() ? nullptr : &*F.arg_begin();
  if (WantThisRet) {
    if (S.Params.empty() || S.Params[0].K != ParamTok::Ptr)
      return false;
    if (!P0 || !P0->getType()->isPointerTy())
      return false;
    if (!F.getReturnType()->isPointerTy())
      return false;
    if (!returnsParam0(F, P0))
      return false;
  }
  // Return compatibility with the current return: the body computes OLD,
  Type *OldRetTy = F.getReturnType();
  bool RetCompat = false;
  switch (S.Ret.K) {
  case RetTok::Void:
    RetCompat = OldRetTy->isVoidTy();
    break;
  case RetTok::ThisRet:
    RetCompat = OldRetTy->isPointerTy();
    break;
  case RetTok::I8:
  case RetTok::I16:
  case RetTok::I32:
  case RetTok::I64:
    RetCompat = OldRetTy->isIntegerTy();
    break;
  case RetTok::F32:
    RetCompat = OldRetTy->isFloatTy() || OldRetTy->isDoubleTy();
    break;
  case RetTok::F64:
    RetCompat = OldRetTy->isDoubleTy() || OldRetTy->isFloatTy();
    break;
  case RetTok::Ptr:
    RetCompat = OldRetTy->isPointerTy() || OldRetTy->isIntegerTy();
    break;
  case RetTok::Struct:
    RetCompat = OldRetTy->isVoidTy();
    break;
  case RetTok::Pair:
    RetCompat = OldRetTy->isIntegerTy();
    break;
  }
  if (!RetCompat)
    return false;
  Type *NewRet = nullptr;
  switch (S.Ret.K) {
  case RetTok::Void:
  case RetTok::ThisRet:
    NewRet = Type::getVoidTy(C);
    break;
  case RetTok::I8:
    NewRet = Type::getInt8Ty(C);
    break;
  case RetTok::I16:
    NewRet = Type::getInt16Ty(C);
    break;
  case RetTok::I32:
    NewRet = Type::getInt32Ty(C);
    break;
  case RetTok::I64:
    NewRet = Type::getInt64Ty(C);
    break;
  case RetTok::F32:
    NewRet = Type::getFloatTy(C);
    break;
  case RetTok::F64:
    NewRet = Type::getDoubleTy(C);
    break;
  case RetTok::Ptr:
    NewRet = PointerType::getUnqual(C);
    break;
  case RetTok::Struct:
    NewRet = SretTy;
    break;
  case RetTok::Pair:
    NewRet = PairTy;
    break;
  }

  // New parameter types, skipping the sret slot.
  SmallVector<Type *, 8> NewParams;
  SmallVector<unsigned, 8> OldToNew; // old idx -> new idx or UINT_MAX
  for (unsigned I = 0; I < S.Params.size(); ++I) {
    if ((int)I == SretIdx) {
      OldToNew.push_back(UINT_MAX);
      continue;
    }
    Type *T = paramType(C, S.Params[I]);
    if (!T)
      return false;
    OldToNew.push_back((unsigned)NewParams.size());
    NewParams.push_back(T);
  }

  // Sret slot guard: store-only through field addressing.
  Argument *SretArg = SretIdx >= 0 ? F.getArg((unsigned)SretIdx) : nullptr;
  if (SretArg && !sretSlotIsStoreOnly(SretArg))
    return false;

  // Build the new function shell first (no body yet). copyAttributesFrom
  // copies the old (opt-side, verifier-recognized) attribute list; the
  // consumed sig is stripped by rebuilding just the fn set, and old param
  // attributes are dropped (they belong to the old signature; lifter
  // output carries none). New param attributes are NOT added here: nodes
  // interned by this statically-linked plugin live in its own folding
  // sets, which the in-process verifier does not recognize (see V handling
  // below).
  FunctionType *NewFTy = FunctionType::get(NewRet, NewParams, S.Vararg);
  Function *NewF =
      Function::Create(NewFTy, F.getLinkage(), F.getAddressSpace());
  NewF->copyAttributesFrom(&F);
  // copyAttributesFrom copies the name too; drop it before inserting.
  NewF->setName("");
  if (F.hasComdat())
    NewF->setComdat(F.getComdat());
  if (F.hasGC())
    NewF->setGC(F.getGC());
  if (F.hasPersonalityFn())
    NewF->setPersonalityFn(F.getPersonalityFn());
  // The function must live in the module before attributes are set: every
  // AttributeList is interned in the module context.
  M.getFunctionList().insert(F.getIterator(), NewF);
  {
    AttrBuilder FnB(C, NewF->getAttributes().getFnAttrs());
    FnB.removeAttribute(SigAttr);
    AttributeSet Fn = AttributeSet::get(C, FnB);
    NewF->setAttributes(AttributeList::get(C, Fn,
                                           NewF->getAttributes().getRetAttrs(),
                                           {}));
  }
  // Name new args; `this` for param 0 of T-methods.
  for (unsigned I = 0; I < S.Params.size(); ++I) {
    if ((int)I == SretIdx)
      continue;
    Argument *NA = NewF->getArg(OldToNew[I]);
    if (S.Params[I].K == ParamTok::This && OldToNew[I] == 0)
      NA->setName("this");
    else if (F.getArg(I)->hasName())
      NA->setName(F.getArg(I)->getName());
    else
      NA->setName("a" + std::to_string(OldToNew[I]));
  }

  // Snapshot call sites with their argument lists before mutating.
  struct Site {
    CallBase *CB;
    SmallVector<Value *, 8> Args;
    SmallVector<AttributeSet, 8> ArgAttrs;
  };
  SmallVector<Site, 8> Sites;
  for (User *U : F.users()) {
    auto *CB = cast<CallBase>(U);
    Site S0;
    S0.CB = CB;
    for (Value *A : CB->args())
      S0.Args.push_back(A);
    Sites.push_back(std::move(S0));
  }

  // Splice the body across; F is now a declaration. NewF was already
  // inserted above (before param attributes were set).
  NewF->splice(NewF->end(), &F);

  // Map old params to converted values in the new body.
  for (unsigned I = 0; I < S.Params.size(); ++I) {
    if ((int)I == SretIdx)
      continue;
    Argument *OA = F.getArg(I);
    Argument *NA = NewF->getArg(OldToNew[I]);
    if (OA->getType() == NA->getType()) {
      OA->replaceAllUsesWith(NA);
      continue;
    }
    Instruction *First = &NewF->getEntryBlock().front();
    while (First && isa<AllocaInst>(First))
      First = First->getNextNode();
    if (!First)
      First = &NewF->getEntryBlock().front();
    // New param (narrow) -> old type (wide): uses in the moved body keep
    // working, the trunc/zext documents the restored width.
    Instruction *CV = buildConversion(NA, OA->getType(), First,
                                      (NA->getName() + ".unabi").str());
    if (!CV) {
      // Not convertible: roll back by moving the body home.
      F.splice(F.end(), NewF);
      NewF->eraseFromParent();
      return false;
    }
    copyVA(&NewF->getEntryBlock().front(), CV);
    OA->replaceAllUsesWith(CV);
  }
  // The old sret arg still feeds the stores (uses moved with the body).
  // Swap it for an entry alloca, then load it at each return.
  AllocaInst *SretSlot = nullptr;
  if (SretArg) {
    Instruction *First = &NewF->getEntryBlock().front();
    IRBuilder<> B(First);
    SretSlot = B.CreateAlloca(SretTy, nullptr, "sret.slot");
    copyVA(First, SretSlot);
    SretArg->replaceAllUsesWith(SretSlot);
  }

  // Return rewriting.
  if (SretTy) {
    for (BasicBlock &BB : *NewF) {
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
        IRBuilder<> B(RI);
        LoadInst *LV = B.CreateLoad(SretTy, SretSlot, "sret.ret");
        copyVA(RI, LV);
        Value *NV = LV;
        // Widen/narrow the loaded struct only when it already matches;
        // the slot type IS the return type, so this is a no-op move.
        B.CreateRet(NV);
        RI->eraseFromParent();
      }
    }
  } else if (PairTy) {
    for (BasicBlock &BB : *NewF) {
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
        Value *RV = RI->getReturnValue();
        if (!RV)
          continue;
        IRBuilder<> B(RI);
        // Bitcopy the int back to the struct: store + load. Sizes are
        // equal by the width check above. The stored integer holds
        // exactly the struct's bytes in Win64 little-endian coercion
        // order, so store(iN)-then-load(%struct) round-trips the value.
        AllocaInst *Tmp = B.CreateAlloca(RV->getType(), nullptr, "pair.tmp");
        copyVA(RI, Tmp);
        B.CreateStore(RV, Tmp);
        Value *BC =
            B.CreateBitCast(Tmp, PointerType::getUnqual(C), "pair.slot");
        copyVA(RI, cast<Instruction>(BC));
        LoadInst *LV = B.CreateLoad(PairTy, BC, "pair.ret");
        copyVA(RI, LV);
        B.CreateRet(LV);
        RI->eraseFromParent();
      }
    }
  } else if (WantThisRet) {
    // Returns hand back param 0 = new param 0; drop the value.
    SmallVector<ReturnInst *, 8> Rets;
    for (BasicBlock &BB : *NewF) {
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
        if (RI->getReturnValue())
          Rets.push_back(RI);
      }
    }
    for (ReturnInst *RI : Rets) {
      MDNode *VA = RI->getMetadata("revenant.va");
      DebugLoc DL = RI->getDebugLoc();
      BasicBlock *BB = RI->getParent();
      RI->eraseFromParent();
      ReturnInst *NV = ReturnInst::Create(C, nullptr, BB);
      if (VA)
        NV->setMetadata("revenant.va", VA);
      if (DL)
        NV->setDebugLoc(DL);
    }
  } else if (NewRet->isVoidTy()) {
    // unchanged void: nothing to do
  } else if (NewRet == OldRetTy) {
    // same type: nothing to do
  } else {
    // Value-return conversion at every return: the body computes the OLD
    // (wide) value, the new signature returns the narrow one.
    for (BasicBlock &BB : *NewF) {
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
        Value *RV = RI->getReturnValue();
        if (!RV)
          continue;
        if (RV->getType() == NewRet)
          continue;
        IRBuilder<> B(RI);
        Instruction *CV = buildConversion(RV, NewRet, RI, "ret.unabi");
        if (!CV)
          continue; // unreachable: types were checked convertible
        copyVA(RI, CV);
        B.CreateRet(CV);
        RI->eraseFromParent();
      }
    }
  }

  // Call sites: rebuild each direct call against the new type.
  for (Site &St : Sites) {
    CallBase *CB = St.CB;
    // New argument list in new-param order.
    SmallVector<Value *, 8> NewArgs;
    for (unsigned I = 0; I < S.Params.size(); ++I) {
      if ((int)I == SretIdx)
        continue;
      Value *A = St.Args[I];
      Type *NT = NewParams[OldToNew[I]];
      if (A->getType() == NT) {
        NewArgs.push_back(A);
        continue;
      }
      Instruction *CV = buildConversion(A, NT, CB, "arg.unabi");
      if (!CV) {
        // Incompatible call-site value (unreachable when the caller was
        // lifted from the same ABI): keep a poison of the right type so
        // the module stays valid; semantics were already broken here.
        NewArgs.push_back(PoisonValue::get(NT));
        continue;
      }
      copyVA(CB, CV);
      NewArgs.push_back(CV);
    }
    Value *SretOutSlot = nullptr;
    if (SretTy) {
      // Caller keeps its slot: the old sret argument value flows into a
      // store of the returned struct after the call.
      SretOutSlot = St.Args[(unsigned)SretIdx];
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
    if (SretTy) {
      // call void -> struct: store the value back to the caller's slot.
      if (auto *CI2 = dyn_cast<CallInst>(NewCB)) {
        auto Next = std::next(CI2->getIterator());
        IRBuilder<> B(CI2->getParent(), Next);
        StoreInst *ST = B.CreateStore(CI2, SretOutSlot);
        copyVA(CB, ST);
      } else {
        auto *NI2 = cast<InvokeInst>(NewCB);
        IRBuilder<> B(&NI2->getNormalDest()->front());
        StoreInst *ST = B.CreateStore(NI2, SretOutSlot);
        copyVA(CB, ST);
      }
      // Old call had no result; nothing to replace.
    } else if (CB->getType()->isVoidTy() && NewRet->isVoidTy()) {
      // nothing to replace
    } else if (CB->getType()->isVoidTy()) {
      // Old void call, new value ignored (unreachable: sret handled).
    } else if (NewRet->isVoidTy()) {
      // Old value dies with the call (this-returns at call sites).
      if (!CB->use_empty())
        CB->replaceAllUsesWith(PoisonValue::get(CB->getType()));
    } else {
      // Value-to-value: the caller body expects the OLD (wide) value,
      // the new call returns narrow. Convert back at the first use.
      if (!CB->use_empty()) {
        if (NewCB->getType() == CB->getType()) {
          CB->replaceAllUsesWith(NewCB);
        } else {
          auto Next = std::next(NewCB->getIterator());
          Instruction *Probe =
              Next == NewCB->getParent()->end() ? NewCB : &*Next;
          Instruction *Back =
              buildConversion(NewCB, CB->getType(), Probe, "retcall.unabi");
          if (Back && std::next(NewCB->getIterator()) !=
                          NewCB->getParent()->end()) {
            copyVA(CB, Back);
            CB->replaceAllUsesWith(Back);
          } else {
            // Unconvertible, or the call is a terminator (unreachable with
            // a used value in well-formed IR): keep the new value when
            // types already match, else poison the stale uses.
            if (Back)
              Back->eraseFromParent();
            if (NewCB->getType() == CB->getType())
              CB->replaceAllUsesWith(NewCB);
            else
              CB->replaceAllUsesWith(PoisonValue::get(CB->getType()));
          }
        }
      }
    }
    CB->eraseFromParent();
  }

  // Swap names: NewF takes F's name, F dies. The sig attribute was already
  // dropped when the shell was built, so a re-run is a no-op.
  std::string Name = F.getName().str();
  F.eraseFromParent();
  NewF->setName(Name);
  return true;
}

bool runOnModule(Module &M) {
  bool Changed = false;
  SmallVector<Function *, 8> Work;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    bool Found = false;
    for (Attribute A : F.getAttributes().getFnAttrs()) {
      if (A.isStringAttribute() && A.getKindAsString() == SigAttr) {
        Found = true;
        break;
      }
    }
    if (Found)
      Work.push_back(&F);
  }
  for (Function *F : Work) {
    StringRef AttrV;
    for (Attribute A : F->getAttributes().getFnAttrs()) {
      if (A.isStringAttribute() && A.getKindAsString() == SigAttr) {
        AttrV = A.getValueAsString();
        break;
      }
    }
    Sig S;
    if (!parseSig(AttrV, S))
      continue;
    Changed |= rewriteFunction(M, *F, S);
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UnabiPass::run(Module &M,
                                           ModuleAnalysisManager &) {
  if (runOnModule(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// links this library statically and registers the `revenant-unabi` name in
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
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnabi", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unabi") {
                    MPM.addPass(revenant::UnabiPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
