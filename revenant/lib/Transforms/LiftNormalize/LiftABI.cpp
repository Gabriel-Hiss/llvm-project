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
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DIBuilder.h"
#include <algorithm>
#include <vector>
using namespace llvm;
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
Constant *resolveConstant(Constant *C, const Absolute &Abs) {
  auto *CE = dyn_cast<ConstantExpr>(C);
  if (!CE)
    return C;
  if (CE->getOpcode() == Instruction::IntToPtr)
    if (auto *CI = dyn_cast<ConstantInt>(CE->getOperand(0)))
      if (Constant *R = Abs.forAddress(CI->getZExtValue()))
        return R;
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
  return CE->getWithOperands(Ops, C->getType(), true);
}

/// Point `CB` at F, rebuilding the call when the site's function type no
/// longer matches (extra register args are dropped, missing ones become
/// undef, the result is coerced back). Keeps provenance metadata.
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
  auto *New = B.CreateCall(FT, F, Args);
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
    else
      Res = coerceTo(New, CB.getType(), B);
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
struct Signature {
  SmallVector<bool, 8> Pointers;
  bool PointerReturn = false;
};
bool canRewrite(Function &F) {
  if (F.isIntrinsic() || F.getName().starts_with("revenant.asm."))
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
  Type *Ret = S.PointerReturn ? PT : F.getReturnType();
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
      S.PointerReturn ? AttributeSet() : F.getAttributes().getRetAttrs(), Attrs));
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
  if (S.PointerReturn)
    for (BasicBlock &BB : *N)
      if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator())) {
        if (!R->getReturnValue())
          continue;
        IRBuilder<> B(R);
        Value *V = toPointer(R->getReturnValue(), B);
        if (!V)
          continue;
        if (auto *I = dyn_cast<Instruction>(V))
          if (I != R->getReturnValue())
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
    if (S.PointerReturn)
      if (auto *II = dyn_cast<InvokeInst>(C))
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
        S.PointerReturn ? AttributeSet() : C->getAttributes().getRetAttrs(), Attrs));
    Value *Result = New;
    if (S.PointerReturn && !C->use_empty()) {
      if (ResultBlock)
        B.SetInsertPoint(ResultBlock->getTerminator());
      else
        B.SetInsertPoint(C);
      Result = B.CreatePtrToInt(New, C->getType(), "abi.result");
      copyOrigin(cast<Instruction>(Result), *C);
    }
    if (!C->use_empty())
      C->replaceAllUsesWith(Result);
    C->eraseFromParent();
  }
  // Opaque pointers retain the exact code address for address-taken uses.
  // Known direct calls above carry the recovered function type explicitly.
  F.replaceAllUsesWith(N);
  F.eraseFromParent();
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
  for (Function &F : M) {
    if (!canRewrite(F))
      continue;
    Signature &S = Signatures[&F];
    S.Pointers.resize(F.arg_size(), false);
    for (Instruction &I : instructions(F))
      if (auto *P = dyn_cast<IntToPtrInst>(&I))
        if (P->getOperand(0)->getType()->isIntegerTy(64))
          Addresses.insert(P->getOperand(0));
  }
  // Propagate address evidence along real direct-call parameters and returns.
  // A monotone finite set reaches a fixed point even for recursive functions.
  // Walk the module's function order, not the map's: pointer-keyed iteration
  // order changes per process, and a rewrite cloning a caller's arguments
  // would land in a different instruction order run to run.
  bool Progress;
  do {
    Progress = false;
    for (Function &F : M) {
      auto It = Signatures.find(&F);
      if (It == Signatures.end())
        continue;
      Signature &S = It->second;
      for (Argument &A : F.args())
        if (A.getType()->isIntegerTy(64) && Addresses.count(&A) && !S.Pointers[A.getArgNo()]) {
          S.Pointers[A.getArgNo()] = true;
          Progress = true;
        }
      for (User *U : F.users())
        if (auto *C = dyn_cast<CallBase>(U)) {
          if (C->getCalledOperand() != &F)
            continue;
          if (C->getType()->isIntegerTy(64) && Addresses.count(C) && !S.PointerReturn) {
            S.PointerReturn = true;
            Progress = true;
          }
          for (unsigned J = 0; J < S.Pointers.size() && J < C->arg_size(); ++J)
            if (S.Pointers[J] && C->getArgOperand(J)->getType()->isIntegerTy(64))
              Progress |= Addresses.insert(C->getArgOperand(J)).second;
        }
      if (S.PointerReturn)
        for (BasicBlock &BB : F)
          if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
            if (R->getReturnValue())
              Progress |= Addresses.insert(R->getReturnValue()).second;
    }
  } while (Progress);
  // Rewriting a signature replaces the function object, so collect the
  // targets in module order first.
  SmallVector<Function *, 32> ToRewrite;
  for (Function &F : M) {
    auto It = Signatures.find(&F);
    if (It == Signatures.end())
      continue;
    Signature &S = It->second;
    if (S.PointerReturn || llvm::is_contained(S.Pointers, true))
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
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // namespace revenant
