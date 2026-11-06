//===- LiftABI.cpp - pointer signatures and stored addresses -------------===//
#include "revenant/Transforms/LiftNormalize/LiftNormalize.h"
#include "revenant/IR/Contract.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include <algorithm>
#include <vector>
using namespace llvm;
using namespace llvm::PatternMatch;
namespace revenant {
namespace {
DIType *irDebugType(Type *Ty, DIBuilder &DIB) {
  if (Ty->isVoidTy())
    return nullptr;
  if (Ty->isPointerTy())
    return DIB.createPointerType(nullptr, 64);
  if (Ty->isIntegerTy())
    return DIB.createBasicType("integer", Ty->getIntegerBitWidth(),
                               dwarf::DW_ATE_signed);
  if (Ty->isFloatingPointTy())
    return DIB.createBasicType("float", Ty->getPrimitiveSizeInBits(),
                               dwarf::DW_ATE_float);
  return nullptr;
}

void updateSubprogramType(Function &F, DIBuilder &DIB,
                          ArrayRef<DIType *> PointerArgs,
                          bool PointerReturn) {
  DISubprogram *SP = F.getSubprogram();
  if (!SP || !SP->isDistinct())
    return;
  auto *OldType = SP->getType();
  DITypeRefArray OldTypes =
      OldType ? OldType->getTypeArray() : DITypeRefArray();
  SmallVector<Metadata *, 8> Types;
  if (PointerReturn)
    Types.push_back(DIB.createPointerType(nullptr, 64));
  else if (F.getReturnType()->isVoidTy())
    Types.push_back(nullptr);
  else
    Types.push_back(OldTypes.size() && OldTypes[0]
                        ? OldTypes[0]
                        : irDebugType(F.getReturnType(), DIB));
  for (Argument &A : F.args()) {
    unsigned I = A.getArgNo();
    if (I < PointerArgs.size() && PointerArgs[I])
      Types.push_back(PointerArgs[I]);
    else if (I + 1 < OldTypes.size() && OldTypes[I + 1])
      Types.push_back(OldTypes[I + 1]);
    else
      Types.push_back(irDebugType(A.getType(), DIB));
  }
  SP->replaceType(DIB.createSubroutineType(DIB.getOrCreateTypeArray(Types)));
}

void updateSubprogramType(Function &F, ArrayRef<bool> PointerArgs,
                          bool PointerReturn) {
  DISubprogram *SP = F.getSubprogram();
  if (!SP || !SP->getUnit())
    return;
  DIBuilder DIB(*F.getParent(), true, SP->getUnit());
  SmallVector<DIType *, 8> Types(PointerArgs.size(), nullptr);
  for (unsigned I = 0; I < PointerArgs.size(); ++I)
    if (PointerArgs[I])
      Types[I] = DIB.createPointerType(nullptr, 64);
  updateSubprogramType(F, DIB, Types, PointerReturn);
  DIB.finalize();
}

/// Convert V to Ty for a rebuilt call operand. Returns undef when no
/// conversion exists; lifted signatures are integer/float/pointer only.
Value *coerceTo(Value *V, Type *Ty, IRBuilder<> &B) {
  Type *VT = V->getType();
  if (VT == Ty)
    return V;
  if (VT->isIntegerTy() && Ty->isIntegerTy())
    return B.CreateZExtOrTrunc(V, Ty, "aco");
  if (VT->isIntegerTy() && Ty->isPointerTy())
    return B.CreateIntToPtr(V, Ty, "aco");
  if (VT->isPointerTy() && Ty->isIntegerTy())
    return B.CreatePtrToInt(V, Ty, "aco");
  if (VT->isIntegerTy() && Ty->isFloatingPointTy()) {
    Value *Wide = B.CreateZExtOrTrunc(
        V, IntegerType::get(V->getContext(), Ty->getPrimitiveSizeInBits()),
        "aco");
    return B.CreateBitCast(Wide, Ty, "aco");
  }
  if (VT->isFloatingPointTy() && Ty->isIntegerTy())
    return B.CreateZExtOrTrunc(
        B.CreateBitCast(
            V, IntegerType::get(V->getContext(), VT->getPrimitiveSizeInBits()),
            "aco"),
        Ty, "aco");
  if (VT->isFloatingPointTy() && Ty->isFloatingPointTy())
    return B.CreateFPCast(V, Ty, "aco");
  return UndefValue::get(Ty);
}
/// Absolute address constants the lifter leaves as `inttoptr` because the
/// referenced symbol was not materialized for that function yet. Every
/// lifted object carries its image VA (functions: "revenant.va" attribute,
/// globals: "revenant.va" metadata; import decls carry the IAT slot VA),
/// so a constant inttoptr can resolve to the real symbol or a GEP into the
/// global containing the address.
struct Absolute {
  /// Exact function VA -> Function.
  DenseMap<uint64_t, Function *> Functions;
  /// Global VA ranges, sorted: [VA, VA + allocated size).
  std::vector<std::pair<uint64_t, GlobalVariable *>> Globals;

  /// The image-backed object covering V, or nullptr.
  Constant *forAddress(uint64_t V) const {
    // `DenseMapInfo<uint64_t>` reserves ~0 (empty) and ~1 (tombstone) for
    // key comparison, so a lookup of either address is undefined and can
    // return an unrelated function (lifted `mov r, -1` sentinels land here).
    if (V != ~0ULL && V != ~1ULL) {
      if (auto It = Functions.find(V); It != Functions.end())
        return It->second;
    }
    auto It = std::upper_bound(
        Globals.begin(), Globals.end(), V,
        [](uint64_t V, const std::pair<uint64_t, GlobalVariable *> &G) {
          return V < G.first;
        });
    if (It == Globals.begin())
      return nullptr;
    --It;
    GlobalVariable *G = It->second;
    uint64_t Size =
        G->getParent()->getDataLayout()
            .getTypeAllocSize(G->getValueType())
            .getFixedValue();
    if (V >= It->first + Size)
      return nullptr;
    if (V == It->first)
      return G;
    LLVMContext &C = G->getContext();
    Constant *Idx = ConstantInt::get(Type::getInt64Ty(C), V - It->first);
    return ConstantExpr::getGetElementPtr(Type::getInt8Ty(C), G, Idx);
  }
};

/// `C` with every inttoptr-of-image-VA replaced by its global object.
/// Always returns C itself or a Constant of the same type owned by C's
/// context, so callers can install the result into a Use unconditionally.
Constant *resolveConstant(Constant *C, const Absolute &Abs) {
  auto *CE = dyn_cast<ConstantExpr>(C);
  if (!CE)
    return C;
  if (CE->getOpcode() == Instruction::IntToPtr)
    if (auto *CI = dyn_cast<ConstantInt>(CE->getOperand(0)))
      // Only an image-backed address in the same address space resolves.
      // A sentinel such as -1 names no object; leave the inttoptr alone.
      if (Constant *R = Abs.forAddress(CI->getZExtValue()))
        return R->getType() == C->getType() ? R : C;
  // getWithOperands only supports casts, select/binop/vector ops and GEP;
  // anything else keeps its operands as they are.
  switch (CE->getOpcode()) {
  case Instruction::InsertElement:
  case Instruction::ExtractElement:
  case Instruction::ShuffleVector:
  case Instruction::GetElementPtr:
    break;
  default:
    if (!CE->isCast() &&
        !(Instruction::isBinaryOp(CE->getOpcode()) &&
          CE->getNumOperands() == 2))
      return C;
    break;
  }
  SmallVector<Constant *, 8> Ops;
  bool Any = false;
  for (unsigned K = 0; K < CE->getNumOperands(); ++K) {
    auto *Op = cast<Constant>(CE->getOperand(K));
    Constant *R = resolveConstant(Op, Abs);
    Ops.push_back(R);
    Any |= R != Op;
  }
  if (!Any)
    return C;
  // OnlyIfReduced may decline to rebuild; and never take a result whose
  // type drifted from C's (e.g. an addrspace-mismatched replacement).
  Constant *R = CE->getWithOperands(Ops, C->getType(), true);
  if (!R || R->getType() != C->getType())
    return C;
  return R;
}

/// Point `CB` at F, rebuilding the call when the site's function type no
/// longer matches (extra register args are dropped, missing ones become
/// undef, the result is coerced back). An invoke is rebuilt as an invoke:
/// replacing it with a call would drop the unwind edge and leave the block
/// unterminated. Keeps provenance metadata.
void retargetCall(CallBase &CB, Function *F) {
  if (CB.getFunctionType() == F->getFunctionType()) {
    CB.setCalledFunction(F);
    return;
  }
  FunctionType *FT = F->getFunctionType();
  SmallVector<Value *, 8> Args;
  IRBuilder<> B(&CB);
  for (unsigned K = 0; K < FT->getNumParams(); ++K) {
    Value *A = K < CB.arg_size()
                   ? coerceTo(CB.getArgOperand(K), FT->getParamType(K), B)
                   : UndefValue::get(FT->getParamType(K));
    Args.push_back(A);
  }
  // An invoke's result is usable only on its normal edge, so when the site
  // needs a coercion back to its own type the cast gets a block there. The
  // split runs before the new invoke is created, while the old invoke is
  // still the terminator it rewires.
  BasicBlock *ResultBlock = nullptr;
  if (auto *II = dyn_cast<InvokeInst>(&CB))
    if (!CB.getType()->isVoidTy() && !FT->getReturnType()->isVoidTy() &&
        FT->getReturnType() != CB.getType())
      ResultBlock = SplitEdge(II->getParent(), II->getNormalDest());
  CallBase *New;
  if (auto *II = dyn_cast<InvokeInst>(&CB))
    New = B.CreateInvoke(FT, F, II->getNormalDest(), II->getUnwindDest(),
                         Args);
  else
    New = B.CreateCall(FT, F, Args);
  New->setCallingConv(CB.getCallingConv());
  if (CB.getDebugLoc())
    New->setDebugLoc(CB.getDebugLoc());
  else if (DISubprogram *SP = CB.getFunction()->getSubprogram())
    New->setDebugLoc(DILocation::get(CB.getContext(), 1, 1, SP));
  New->copyMetadata(CB);
  if (!CB.getType()->isVoidTy()) {
    Value *Res;
    if (New->getType()->isVoidTy())
      Res = UndefValue::get(CB.getType());
    else {
      if (ResultBlock)
        B.SetInsertPoint(ResultBlock->getTerminator());
      Res = coerceTo(New, CB.getType(), B);
    }
    CB.replaceAllUsesWith(Res);
  }
  CB.eraseFromParent();
}
void copyOrigin(Instruction *To, const Instruction &From) {
  for (StringRef Kind : {"revenant.va", "revenant.opaque", "revenant.region"})
    if (MDNode *MD = From.getMetadata(Kind))
      To->setMetadata(Kind, MD);
  To->setDebugLoc(From.getDebugLoc());
}
Value *toPointer(Value *V, IRBuilder<> &B) {
  if (auto *P = dyn_cast<PtrToIntInst>(V))
    return P->getPointerOperand();
  if (V->getType()->isPointerTy())
    return V;
  if (!V->getType()->isIntegerTy(64))
    return nullptr;
  return B.CreateIntToPtr(V, B.getPtrTy(), "abi.ptr");
}
/// The narrow-typed value a return of `V` carries when every return of the
/// function is a zero extension from `Narrow` (see `narrowReturnType`). A
/// bool held in a byte is truncated to its bit.
Value *narrowReturnValue(Value *V, Type *Narrow, IRBuilder<> &B) {
  if (auto *Z = dyn_cast<ZExtInst>(V))
    if (Z->getSrcTy() == Narrow)
      return Z->getOperand(0);
  if (auto *C = dyn_cast<ConstantInt>(V))
    return ConstantInt::get(Narrow, C->getValue().trunc(Narrow->getIntegerBitWidth()));
  if (isa<UndefValue>(V))
    return UndefValue::get(Narrow);
  if (Narrow->isIntegerTy(1))
    return B.CreateTrunc(V, Narrow, "abi.bool");
  return nullptr;
}
/// V is a byte that only ever holds 0 or 1: a bool as the x64 ABI passes
/// it. Unoptimized code masks a bool it returns (`and al, 1`), optimized
/// code returns a `setcc` result.
bool boolByte(Value *V, SmallPtrSetImpl<Value *> &Seen) {
  if (!Seen.insert(V).second || isa<UndefValue>(V))
    return true;
  if (auto *C = dyn_cast<ConstantInt>(V))
    return C->getValue().ule(1);
  if (auto *Z = dyn_cast<ZExtInst>(V))
    return Z->getSrcTy()->isIntegerTy(1);
  if (auto *BO = dyn_cast<BinaryOperator>(V)) {
    if (BO->getOpcode() == Instruction::And)
      return match(BO->getOperand(1), m_One()) || boolByte(BO->getOperand(0), Seen) ||
             boolByte(BO->getOperand(1), Seen);
    if (BO->getOpcode() == Instruction::Xor || BO->getOpcode() == Instruction::Or)
      return boolByte(BO->getOperand(0), Seen) && boolByte(BO->getOperand(1), Seen);
    return false;
  }
  if (auto *P = dyn_cast<PHINode>(V))
    return llvm::all_of(P->incoming_values(), [&](Value *In) { return boolByte(In, Seen); });
  return false;
}
/// The type every return value of F zero-extends from, when all returns are
/// such extensions (at least one) or constants that fit. MSVC x64 returns
/// `int` in eax, and a 32-bit write zero-extends rax, so the lift sees
/// `zext i32 %x to i64` where the source returned the i32 itself. A byte
/// result that only holds 0 or 1 is a bool (`i1`); a wider result built
/// from a compare stays, `int f() { return a < b; }` returns an int.
Type *narrowReturnType(Function &F) {
  auto *RT = dyn_cast<IntegerType>(F.getReturnType());
  if (!RT)
    return nullptr;
  if (RT->getBitWidth() == 8) {
    SmallPtrSet<Value *, 16> Seen;
    bool Any = false, Bool = true;
    for (BasicBlock &BB : F)
      if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
        if (Value *V = R->getReturnValue()) {
          Any |= !isa<Constant>(V);
          Bool &= boolByte(V, Seen);
        }
    return Any && Bool ? Type::getInt1Ty(F.getContext()) : nullptr;
  }
  Type *Narrow = nullptr;
  SmallVector<ConstantInt *, 4> Constants;
  for (BasicBlock &BB : F) {
    auto *R = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!R || !R->getReturnValue())
      continue;
    Value *V = R->getReturnValue();
    if (auto *Z = dyn_cast<ZExtInst>(V)) {
      if (Narrow && Z->getSrcTy() != Narrow)
        return nullptr;
      Narrow = Z->getSrcTy();
    } else if (auto *C = dyn_cast<ConstantInt>(V)) {
      Constants.push_back(C);
    } else if (!isa<UndefValue>(V)) {
      return nullptr;
    }
  }
  if (!Narrow || Narrow->getIntegerBitWidth() < 8)
    return nullptr;
  for (ConstantInt *C : Constants)
    if (C->getValue().getActiveBits() > Narrow->getIntegerBitWidth())
      return nullptr;
  return Narrow;
}
/// True when no direct call reads F's result and F has no other use, so the
/// value in the return register is not part of the interface. A result that
/// only feeds the return of a function already in `Dead` is unread too.
bool deadReturn(Function &F, const SmallPtrSetImpl<Function *> &Dead) {
  if (F.getReturnType()->isVoidTy() || F.use_empty())
    return false;
  for (const Use &U : F.uses()) {
    auto *C = dyn_cast<CallBase>(U.getUser());
    if (!C || !C->isCallee(&U))
      return false;
    for (const User *R : C->users())
      if (!isa<ReturnInst>(R) || !Dead.count(cast<ReturnInst>(R)->getFunction()))
        return false;
  }
  return true;
}
/// True when nothing F computes reaches its returns: each returned value is
/// undef or another call's result (through casts and phis), i.e. the return
/// register only still holds what a callee left there. Callers that drop a
/// result are not proof of a void function on their own: a lift that loses a
/// register across a call drops the use too. A function that computes its
/// return value keeps it. In an unoptimized function a register merged
/// across blocks is never a result: RegAllocFast spills every value live
/// across a block boundary, so a real result is reloaded where it returns,
/// and the merge is whatever the paths last left in rax.
bool incidentalReturn(Function &F) {
  const bool Unoptimized = F.hasFnAttribute("revenant.o0");
  SmallVector<Value *, 8> Work;
  SmallPtrSet<Value *, 16> Seen;
  for (BasicBlock &BB : F)
    if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
      if (R->getReturnValue())
        Work.push_back(R->getReturnValue());
  if (Work.empty())
    return false;
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    if (isa<UndefValue>(V) || isa<CallBase>(V))
      continue;
    // MSVC hands back an sret pointer in rax; the compiler's IR is void.
    if (auto *A = dyn_cast<Argument>(V); A && A->getArgNo() > 0)
      continue;
    if (auto *C = dyn_cast<CastInst>(V)) {
      Work.push_back(C->getOperand(0));
      continue;
    }
    if (auto *P = dyn_cast<PHINode>(V)) {
      if (Unoptimized)
        continue;
      for (Value *In : P->incoming_values())
        Work.push_back(In);
      continue;
    }
    return false;
  }
  return true;
}
/// True when every return of F hands back its first argument unchanged
/// (possibly through a pointer/integer cast).
bool returnsFirstArgument(Function &F) {
  if (F.arg_empty())
    return false;
  Argument *A0 = F.getArg(0);
  bool Any = false;
  for (BasicBlock &BB : F) {
    auto *R = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!R || !R->getReturnValue())
      continue;
    Value *V = R->getReturnValue();
    if (auto *C = dyn_cast<CastInst>(V))
      if (C->getOpcode() == Instruction::PtrToInt || C->getOpcode() == Instruction::IntToPtr)
        V = C->getOperand(0);
    if (V != A0)
      return false;
    Any = true;
  }
  return Any;
}
struct Signature {
  SmallVector<bool, 8> Pointers;
  bool PointerReturn = false;
  /// No direct caller reads the result and the function has no other use.
  bool VoidReturn = false;
  /// Every returned value is a zero extension from this narrower type.
  Type *NarrowReturn = nullptr;
};
/// V is the integer bits of a pointer the function holds: a `ptrtoint`, a
/// parameter the analysis already types as a pointer, or a read of a stack
/// slot a pointer was stored to (`Held`).
bool pointerBits(const Value *V, const DenseMap<Function *, Signature> &Sigs,
                 const SmallPtrSetImpl<const Value *> &Held) {
  if (isa<PtrToIntInst>(V) || Held.count(V))
    return true;
  if (const auto *A = dyn_cast<Argument>(V)) {
    auto It = Sigs.find(A->getParent());
    return It != Sigs.end() && It->second.Pointers[A->getArgNo()];
  }
  return false;
}
bool canRewrite(Function &F) {
  if (F.isIntrinsic() || F.getName().starts_with("revenant.asm.") ||
      F.getName().starts_with("revenant.o0."))
    return false;
  for (User *U : F.users())
    if (auto *C = dyn_cast<CallBase>(U))
      if (isa<CallBrInst>(C) || C->isMustTailCall())
        return false;
  return true;
}
void rewriteSignature(Function &F, Signature &S) {
  Module &M = *F.getParent();
  SmallVector<Type *, 8> Params;
  Type *PT = PointerType::getUnqual(M.getContext());
  for (Argument &A : F.args())
    Params.push_back(S.Pointers[A.getArgNo()] ? PT : A.getType());
  Type *Ret = S.VoidReturn      ? Type::getVoidTy(M.getContext())
              : S.PointerReturn ? PT
              : S.NarrowReturn  ? S.NarrowReturn
                                : F.getReturnType();
  const bool RetChanged = Ret != F.getReturnType();
  auto *FT = FunctionType::get(Ret, Params, F.isVarArg());
  if (FT == F.getFunctionType())
    return;
  Function *N = Function::Create(FT, F.getLinkage(), F.getAddressSpace());
  N->copyAttributesFrom(&F);
  N->copyMetadata(&F, 0);
  SmallVector<AttributeSet, 8> Attrs;
  for (unsigned J = 0; J < Params.size(); ++J)
    Attrs.push_back(S.Pointers[J] ? AttributeSet() : F.getAttributes().getParamAttrs(J));
  N->setAttributes(AttributeList::get(M.getContext(), F.getAttributes().getFnAttrs(),
      RetChanged ? AttributeSet() : F.getAttributes().getRetAttrs(), Attrs));
  M.getFunctionList().insert(F.getIterator(), N);
  N->takeName(&F);
  N->splice(N->end(), &F);
  updateSubprogramType(*N, ArrayRef<bool>(S.Pointers), S.PointerReturn);
  for (Argument &Old : F.args()) {
    Argument *New = N->getArg(Old.getArgNo());
    New->takeName(&Old);
    if (!S.Pointers[Old.getArgNo()]) {
      Old.replaceAllUsesWith(New);
      continue;
    }
    if (!N->empty()) {
      IRBuilder<> B(&*N->getEntryBlock().getFirstInsertionPt());
      Value *Bits = B.CreatePtrToInt(New, Old.getType(), "abi.bits");
      copyOrigin(cast<Instruction>(Bits), *N->getEntryBlock().getTerminator());
      Old.replaceAllUsesWith(Bits);
    }
  }
  for (BasicBlock &BB : *N) {
    auto *R = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!R || !R->getReturnValue())
      continue;
    IRBuilder<> B(R);
    if (S.VoidReturn) {
      auto *RV = B.CreateRetVoid();
      copyOrigin(RV, *R);
      R->eraseFromParent();
      continue;
    }
    Instruction *Prev = R->getPrevNode();
    Value *V = nullptr;
    if (S.PointerReturn)
      V = toPointer(R->getReturnValue(), B);
    else if (S.NarrowReturn)
      V = narrowReturnValue(R->getReturnValue(), S.NarrowReturn, B);
    if (!V)
      continue;
    // Casts the conversion inserted sit between Prev and the return.
    for (Instruction *I = Prev ? Prev->getNextNode() : &BB.front(); I != R;
         I = I->getNextNode())
      copyOrigin(I, *R);
    R->setOperand(0, V);
  }
  SmallVector<CallBase *, 16> Calls;
  for (User *U : F.users())
    if (auto *CB = dyn_cast<CallBase>(U))
      if (CB->getCalledOperand() == &F)
        Calls.push_back(CB);
  for (CallBase *C : Calls) {
    BasicBlock *ResultBlock = nullptr;
    if (S.PointerReturn || S.NarrowReturn)
      if (auto *II = dyn_cast<InvokeInst>(C))
        if (!C->use_empty())
          ResultBlock = SplitEdge(II->getParent(), II->getNormalDest());
    IRBuilder<> B(C);
    auto *New = cast<CallBase>(C->clone());
    // A call site recovered with fewer operands than the new signature keeps
    // its own shape: rewriting it would read past the operand list.
    unsigned Convertible = std::min<unsigned>(Params.size(), C->arg_size());
    for (unsigned J = 0; J < Convertible; ++J)
      if (S.Pointers[J]) {
        Value *Arg = toPointer(C->getArgOperand(J), B);
        if (!Arg)
          continue;
        if (auto *I = dyn_cast<Instruction>(Arg))
          if (I != C->getArgOperand(J))
            copyOrigin(I, *C);
        New->setArgOperand(J, Arg);
      }
    New->insertBefore(C->getIterator());
    New->mutateFunctionType(FT);
    New->setCalledFunction(N);
    New->setAttributes(AttributeList::get(M.getContext(), C->getAttributes().getFnAttrs(),
        RetChanged ? AttributeSet() : C->getAttributes().getRetAttrs(), Attrs));
    if (New->getType()->isVoidTy())
      New->setName("");
    Value *Result = New;
    if ((S.PointerReturn || S.NarrowReturn) && !C->use_empty()) {
      if (ResultBlock)
        B.SetInsertPoint(ResultBlock->getTerminator());
      else
        B.SetInsertPoint(C);
      Result = S.PointerReturn ? B.CreatePtrToInt(New, C->getType(), "abi.result")
                               : B.CreateZExt(New, C->getType(), "abi.result");
      copyOrigin(cast<Instruction>(Result), *C);
    }
    // A void callee's result only fed returns of functions that become void
    // too; undef holds the place until their returns are rewritten.
    if (!C->use_empty())
      C->replaceAllUsesWith(S.VoidReturn ? UndefValue::get(C->getType()) : Result);
    C->eraseFromParent();
  }
  // Opaque pointers retain the exact code address for address-taken uses.
  // Known direct calls above carry the recovered function type explicitly.
  F.replaceAllUsesWith(N);
  F.eraseFromParent();
}
/// Retype one indirect call: an argument made by `ptrtoint` passes the
/// pointer, and a result whose every use truncates to one width is read at
/// that width. The callee is unknown, so only the call site's type changes.
bool retypeIndirectCall(CallBase &CB) {
  LLVMContext &Ctx = CB.getContext();
  FunctionType *Old = CB.getFunctionType();
  Type *PT = PointerType::getUnqual(Ctx);
  SmallVector<Type *, 8> Params(Old->params());
  SmallVector<Value *, 8> Args(CB.args());
  SmallVector<unsigned, 4> ChangedArgs;
  for (unsigned J = 0; J < Args.size() && J < Params.size(); ++J)
    if (auto *P = dyn_cast<PtrToIntInst>(Args[J]))
      if (Params[J]->isIntegerTy(64) && P->getPointerOperand()->getType() == PT) {
        Args[J] = P->getPointerOperand();
        Params[J] = PT;
        ChangedArgs.push_back(J);
      }
  Type *Ret = Old->getReturnType();
  SmallVector<TruncInst *, 4> Truncs;
  if (Ret->isIntegerTy() && !CB.use_empty() && !isa<InvokeInst>(CB)) {
    Type *Narrow = nullptr;
    for (User *U : CB.users()) {
      auto *T = dyn_cast<TruncInst>(U);
      if (!T || (Narrow && T->getType() != Narrow)) {
        Narrow = nullptr;
        break;
      }
      Narrow = T->getType();
      Truncs.push_back(T);
    }
    if (Narrow)
      Ret = Narrow;
    else
      Truncs.clear();
  }
  if (ChangedArgs.empty() && Truncs.empty())
    return false;
  auto *FT = FunctionType::get(Ret, Params, Old->isVarArg());
  auto *New = cast<CallBase>(CB.clone());
  for (unsigned J : ChangedArgs)
    New->setArgOperand(J, Args[J]);
  New->mutateFunctionType(FT);
  AttributeList AL = New->getAttributes();
  for (unsigned J : ChangedArgs)
    AL = AL.removeParamAttributes(Ctx, J);
  if (!Truncs.empty())
    AL = AL.removeAttributesAtIndex(Ctx, AttributeList::ReturnIndex);
  New->setAttributes(AL);
  New->insertBefore(CB.getIterator());
  New->takeName(&CB);
  for (TruncInst *T : Truncs) {
    T->replaceAllUsesWith(New);
    T->eraseFromParent();
  }
  if (!CB.use_empty())
    CB.replaceAllUsesWith(New);
  CB.eraseFromParent();
  return true;
}
} // namespace
PreservedAnalyses LiftABIPass::run(Module &M, ModuleAnalysisManager &) {
  const DataLayout &DL = M.getDataLayout();
  if (DL.getPointerSizeInBits() != 64 || DL.isNonIntegralAddressSpace(0))
    return PreservedAnalyses::all();
  // Absolute addresses the lifter left as integer constants: globals,
  // strings and IAT slots that this function never materialized, plus
  // direct calls whose target was lifted as inttoptr code VA. Map them
  // onto the module's own symbols first so signature evidence below sees
  // real pointer uses.
  Absolute Abs;
  bool Changed = false;
  for (GlobalVariable &G : M.globals())
    if (auto VA = ir::getGlobalVA(G))
      Abs.Globals.push_back({*VA, &G});
  llvm::sort(Abs.Globals,
             [](const auto &A, const auto &B) { return A.first < B.first; });
  for (Function &F : M)
    if (auto VA = ir::getFunctionVA(F)) {
      // ~0 / ~1 are DenseMapInfo's reserved keys for uint64_t.
      if (*VA != ~0ULL && *VA != ~1ULL)
        Abs.Functions.try_emplace(*VA, &F);
    }
  for (GlobalVariable &G : M.globals())
    if (G.hasInitializer()) {
      Constant *R = resolveConstant(G.getInitializer(), Abs);
      if (R != G.getInitializer()) {
        G.setInitializer(R);
        Changed = true;
      }
    }
  for (Function &F : M) {
    // Calls through a resolved code VA become direct calls.
    SmallVector<CallBase *, 8> Calls;
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (!isa<CallBrInst>(CB) && !CB->isMustTailCall())
          Calls.push_back(CB);
    for (CallBase *CB : Calls)
      if (auto *CE = dyn_cast<ConstantExpr>(CB->getCalledOperand()))
        if (CE->getOpcode() == Instruction::IntToPtr)
          if (auto *CI = dyn_cast<ConstantInt>(CE->getOperand(0)))
            if (Constant *R = Abs.forAddress(CI->getZExtValue())) {
              if (auto *Target = dyn_cast<Function>(R))
                retargetCall(*CB, Target);
              else
                CB->setCalledOperand(R);
              Changed = true;
            }
  }
  // Any other constant inttoptr operand: phi inedges, non-callee call
  // operands, stores, selects and surviving IntToPtr instructions.
  for (Function &F : M)
    for (Instruction &I : instructions(F))
      for (Use &U : I.operands()) {
        auto *C = dyn_cast<Constant>(U.get());
        if (!C || isa<GlobalValue>(C) || isa<ConstantInt>(C))
          continue;
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (CB->isCallee(&U))
            continue;
        Constant *R = resolveConstant(C, Abs);
        if (R != C) {
          U.set(R);
          Changed = true;
        }
      }
  // IntToPtr instructions over image constants never reach the operand
  // loop above unchanged; fold them to the resolved symbol.
  for (Function &F : M)
    for (Instruction &I : make_early_inc_range(instructions(F)))
      if (auto *P = dyn_cast<IntToPtrInst>(&I))
        if (auto *CI = dyn_cast<ConstantInt>(P->getOperand(0)))
          if (Constant *R = Abs.forAddress(CI->getZExtValue())) {
            P->replaceAllUsesWith(R);
            P->eraseFromParent();
            Changed = true;
          }

  DenseMap<Function *, Signature> Signatures;
  SmallPtrSet<Value *, 32> Addresses;
  // Evidence is per value, and a constant is one value module-wide: an
  // `i64 11` used as an address in one function would make every function
  // returning 11 return a pointer. Image addresses were resolved to their
  // symbols above, so constants carry no evidence.
  auto Mark = [&](Value *V) {
    return !isa<Constant>(V) && Addresses.insert(V).second;
  };
  for (Function &F : M) {
    if (!canRewrite(F))
      continue;
    Signature &S = Signatures[&F];
    S.Pointers.resize(F.arg_size(), false);
    // An address is a value used as a pointer (inttoptr operand) or made
    // from one (ptrtoint result).
    for (Instruction &I : instructions(F))
      if (auto *P = dyn_cast<IntToPtrInst>(&I)) {
        if (P->getOperand(0)->getType()->isIntegerTy(64))
          Mark(P->getOperand(0));
      } else if (auto *P = dyn_cast<PtrToIntInst>(&I)) {
        if (P->getType()->isIntegerTy(64))
          Addresses.insert(P);
      }
  }
  // Propagate address evidence along real direct-call parameters and returns.
  // A monotone finite set reaches a fixed point even for recursive functions.
  // Walk the module's function order, not the map's: pointer-keyed iteration
  // order changes per process, and a rewrite cloning a caller's arguments
  // would land in a different instruction order run to run.
  // A stack slot is one variable: what is stored to it and what is loaded
  // from it are the same value. An unoptimized frame keeps its slots, so the
  // evidence has to cross them. A slot a pointer is stored to (as a pointer,
  // or as its `ptrtoint` bits), or that is read as a pointer, holds an
  // address: its integer reads are the bits of a pointer the function holds.
  struct SlotValues {
    SmallVector<Value *, 4> Values;
  };
  std::vector<SlotValues> Slots;
  SmallPtrSet<const Value *, 32> Held;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : F.getEntryBlock()) {
      auto *A = dyn_cast<AllocaInst>(&I);
      if (!A)
        continue;
      SlotValues SV;
      bool Closed = true;
      bool HoldsPointer = false;
      for (Use &U : A->uses()) {
        if (auto *L = dyn_cast<LoadInst>(U.getUser())) {
          if (L->getType()->isIntegerTy(64))
            SV.Values.push_back(L);
          HoldsPointer |= L->getType()->isPointerTy();
        } else if (auto *S = dyn_cast<StoreInst>(U.getUser());
                   S && U.getOperandNo() == StoreInst::getPointerOperandIndex()) {
          Type *T = S->getValueOperand()->getType();
          if (T->isIntegerTy(64))
            SV.Values.push_back(S->getValueOperand());
          HoldsPointer |= T->isPointerTy() || isa<PtrToIntInst>(S->getValueOperand());
        } else if (auto *C = dyn_cast<CallBase>(U.getUser());
                   C && C->getCalledFunction() &&
                   C->getCalledFunction()->getName() == "revenant.o0.keep") {
          // The unoptimized-frame pin is not a use of the slot's contents.
        } else {
          Closed = false;
        }
      }
      if (!Closed)
        continue;
      if (HoldsPointer) {
        for (Value *V : SV.Values) {
          Mark(V);
          if (isa<LoadInst>(V))
            Held.insert(V);
        }
      } else if (SV.Values.size() > 1) {
        Slots.push_back(std::move(SV));
      }
    }
  }
  bool Progress;
  do {
    Progress = false;
    for (SlotValues &SV : Slots)
      if (llvm::any_of(SV.Values, [&](Value *V) { return Addresses.count(V); }))
        for (Value *V : SV.Values)
          Progress |= Mark(V);
    for (Function &F : M) {
      auto It = Signatures.find(&F);
      if (It == Signatures.end())
        continue;
      Signature &S = It->second;
      for (Argument &A : F.args()) {
        if (A.getType()->isIntegerTy(64) && Addresses.count(&A) && !S.Pointers[A.getArgNo()]) {
          S.Pointers[A.getArgNo()] = true;
          Progress = true;
        }
        // A pointer parameter is an address inside the callee as well.
        if (S.Pointers[A.getArgNo()])
          Progress |= Mark(&A);
      }
      for (User *U : F.users())
        if (auto *C = dyn_cast<CallBase>(U)) {
          if (C->getCalledOperand() != &F)
            continue;
          if (C->getType()->isIntegerTy(64) && Addresses.count(C) && !S.PointerReturn) {
            S.PointerReturn = true;
            Progress = true;
          }
          // A pointer-returning callee yields addresses at its call sites.
          if (S.PointerReturn && C->getType()->isIntegerTy(64))
            Progress |= Mark(C);
          for (unsigned J = 0; J < S.Pointers.size() && J < C->arg_size(); ++J) {
            Value *Arg = C->getArgOperand(J);
            if (!Arg->getType()->isIntegerTy(64))
              continue;
            if (S.Pointers[J])
              Progress |= Mark(Arg);
            else if (F.getArg(J)->getType()->isIntegerTy(64) &&
                     pointerBits(Arg, Signatures, Held)) {
              // The caller passes the bits of a pointer it holds: the
              // parameter is a pointer. Only a direct cast, a pointer
              // parameter or a pointer read back from its slot counts;
              // integers derived from addresses (hashes, offsets) stay
              // integers.
              S.Pointers[J] = true;
              Progress = true;
            }
          }
        }
      if (!S.PointerReturn && F.getReturnType()->isIntegerTy(64))
        for (BasicBlock &BB : F)
          if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
            if (R->getReturnValue() && Addresses.count(R->getReturnValue())) {
              // The function returns an address.
              S.PointerReturn = true;
              Progress = true;
              break;
            }
      if (S.PointerReturn)
        for (BasicBlock &BB : F)
          if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
            if (R->getReturnValue())
              Progress |= Mark(R->getReturnValue());
    }
  } while (Progress);
  // Return width. A result no direct caller reads is not part of the
  // interface (void), except for a function that hands back its first
  // argument: MSVC constructors and assignment operators return `this`
  // whether or not the caller uses it. Otherwise a return that is always a
  // zero extension narrows to the extended type.
  SmallPtrSet<Function *, 32> Dead;
  bool Grew;
  do {
    Grew = false;
    for (Function &F : M)
      if (Signatures.count(&F) && !Dead.count(&F) && deadReturn(F, Dead) &&
          incidentalReturn(F) && !returnsFirstArgument(F)) {
        Dead.insert(&F);
        Grew = true;
      }
  } while (Grew);
  for (Function &F : M) {
    auto It = Signatures.find(&F);
    if (It == Signatures.end())
      continue;
    Signature &S = It->second;
    if (Dead.count(&F)) {
      S.VoidReturn = true;
      S.PointerReturn = false;
    } else if (!S.PointerReturn) {
      S.NarrowReturn = narrowReturnType(F);
    }
  }
  // Rewriting a signature replaces the function object, so collect the
  // targets in module order first.
  SmallVector<Function *, 32> ToRewrite;
  for (Function &F : M) {
    auto It = Signatures.find(&F);
    if (It == Signatures.end())
      continue;
    Signature &S = It->second;
    if (S.PointerReturn || S.VoidReturn || S.NarrowReturn || llvm::is_contained(S.Pointers, true))
      ToRewrite.push_back(&F);
  }
  for (Function *F : ToRewrite) {
    Signature &S = Signatures[F];
    rewriteSignature(*F, S);
    Changed = true;
  }
  // Memory is untyped on this target. A pointer-sized integer load consumed
  // as an address can load ptr directly, preserving integer-only uses through
  // ptrtoint. Keep alignment, atomic ordering, alias metadata and provenance.
  SmallVector<LoadInst *, 32> Loads;
  for (Function &F : M)
    for (Instruction &I : instructions(F))
      if (auto *L = dyn_cast<LoadInst>(&I))
        Loads.push_back(L);
  for (LoadInst *L : Loads) {
    if (!L->getType()->isIntegerTy(64) || L->getMetadata(LLVMContext::MD_range))
      continue;
    SmallVector<IntToPtrInst *, 8> Casts;
    for (User *U : L->users())
      if (auto *P = dyn_cast<IntToPtrInst>(U))
        if (P->getAddressSpace() == 0)
          Casts.push_back(P);
    if (Casts.empty())
      continue;
    IRBuilder<> B(L);
    auto *P = cast<LoadInst>(L->clone());
    P->mutateType(B.getPtrTy());
    P->insertBefore(L->getIterator());
    for (IntToPtrInst *C : Casts) {
      C->replaceAllUsesWith(P);
      C->eraseFromParent();
    }
    if (!L->use_empty()) {
      Value *Bits = B.CreatePtrToInt(P, L->getType(), "field.bits");
      copyOrigin(cast<Instruction>(Bits), *L);
      L->replaceAllUsesWith(Bits);
    }
    L->eraseFromParent();
    Changed = true;
  }
  // The same holds for stores: an address stored as its integer bits stores
  // the pointer itself.
  for (Function &F : M)
    for (Instruction &I : instructions(F))
      if (auto *St = dyn_cast<StoreInst>(&I))
        if (auto *P = dyn_cast<PtrToIntInst>(St->getValueOperand()))
          if (P->getType()->isIntegerTy(64) && P->getPointerOperand()->getType()->isPointerTy() &&
              P->getPointerAddressSpace() == 0) {
            St->setOperand(0, P->getPointerOperand());
            Changed = true;
          }
  // Indirect calls carry the call site's own function type: pass addresses as
  // pointers, and read a result that is only ever truncated at its width.
  SmallVector<CallBase *, 32> Indirect;
  for (Function &F : M)
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction() && !CB->isInlineAsm() && !isa<CallBrInst>(CB) &&
            !CB->isMustTailCall() && !CB->getFunctionType()->isVarArg())
          Indirect.push_back(CB);
  for (CallBase *CB : Indirect) {
    if (retypeIndirectCall(*CB)) {
      Changed = true;
    }
  }
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
} // namespace revenant
