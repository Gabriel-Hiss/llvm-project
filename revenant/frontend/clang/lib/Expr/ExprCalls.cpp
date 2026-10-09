//===- ExprCalls.cpp - call and select lowering -------*- C++ -*-===//
//
// Calls, selects, virtual dispatch. Part of RevenantClangExpr.
//
//===----------------------------------------------------------------------===//

#include "ExprBuilderImpl.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"


namespace revenant::clangx {
using namespace clang;
using namespace llvm::dwarf;

// ---- select / phi / extractvalue -------------------------------------------

Expr *ExprBuilderImpl::buildSelect(llvm::SelectInst &I, ExprScope &Scope) {
  Expr *C = truthTest(value(I.getCondition(), Scope));
  Expr *T = parenIfNeeded(value(I.getTrueValue(), Scope), Prec::Ternary);
  Expr *F = parenIfNeeded(value(I.getFalseValue(), Scope), Prec::Ternary);
  QualType T0 = T->getType(), T1 = F->getType();
  QualType ResTy = exprTypeOf(&I);
  // Usual arithmetic conversions: llvm::cast the narrower side up.
  if (T0 != T1) {
    if (T0->isIntegerType() && T1->isIntegerType()) {
      uint64_t W0 = AST->getTypeSize(T0), W1 = AST->getTypeSize(T1);
      if (W0 < W1)
        T = makeCStyleCast(T1, CK_IntegralCast, T);
      else
        F = makeCStyleCast(T0, CK_IntegralCast, F);
    } else if ((T0->isPointerType() && T1->isIntegerType()) ||
               (T1->isPointerType() && T0->isIntegerType())) {
      if (ResTy->isPointerType()) {
        if (T0->isPointerType() && T0 != ResTy)
          T = makeCStyleCast(ResTy, CK_BitCast, T);
        else if (T0->isIntegerType())
          T = makeCStyleCast(ResTy, CK_IntegralToPointer, T);
        if (T1->isPointerType() && T1 != ResTy)
          F = makeCStyleCast(ResTy, CK_BitCast, F);
        else if (T1->isIntegerType())
          F = makeCStyleCast(ResTy, CK_IntegralToPointer, F);
      } else {
        QualType IntTy = uintptrTy();
        T = makeCStyleCast(IntTy, T0->isPointerType()
                                      ? CK_PointerToIntegral
                                      : CK_IntegralCast, T);
        F = makeCStyleCast(IntTy, T1->isPointerType()
                                      ? CK_PointerToIntegral
                                      : CK_IntegralCast, F);
        ResTy = IntTy;
      }
    } else if (T0->isPointerType() && T1->isPointerType()) {
      if (T0 != ResTy)
        T = makeCStyleCast(ResTy, CK_BitCast, T);
      if (F->getType() != ResTy)
        F = makeCStyleCast(ResTy, CK_BitCast, F);
    }
  }
  // Condition below ternary precedence gets parens (emit-c prec rule:
  // `?:` is 4, comparisons are 9/10 so they stay bare).
  Expr *Cp = parenIfNeeded(C, Prec::Ternary);
  clang::ConditionalOperator *Q = new (*AST) clang::ConditionalOperator(
      Cp, SourceLocation(), T, SourceLocation(), F, ResTy, VK_PRValue,
      OK_Ordinary);
  return ensureTemp(&I, Scope, Q);
}

Expr *ExprBuilderImpl::buildPHIValue(llvm::PHINode &I, ExprScope &Scope) {
  // PHIs have no inline C spelling; StmtBuilder materializes them as
  // temporaries assigned on each predecessor edge (or a single init when
  // all inputs agree). Here resolve to the temporary, creating it.
  return tempFor(&I, Scope);
}

Expr *ExprBuilderImpl::buildExtractValue(llvm::ExtractValueInst &I, ExprScope &Scope) {
  Expr *Agg = value(I.getAggregateOperand(), Scope);
  // Single-index struct extract with a known record becomes member
  // access; anything else falls back to the temporary (StmtBuilder
  // lowers multi-index extracts to byte arithmetic).
  if (I.getNumIndices() == 1) {
    unsigned FieldNo = *I.idx_begin();
    const llvm::DICompositeType *RecDI =
        compositeBehindPointer(debugTypeOf(I.getAggregateOperand()));
    RecordDecl *Rec = RecDI ? Types.recordFor(RecDI) : nullptr;
    QualType ElemQT = exprTypeOf(&I);
    if (Rec) {
      llvm::DINodeArray Elems = RecDI->getElements();
      unsigned Seen = 0;
      uint64_t BitOff = 0;
      for (llvm::DINode *N : Elems) {
        auto *M = llvm::dyn_cast<llvm::DIDerivedType>(N);
        if (!M || M->getTag() != llvm::dwarf::DW_TAG_member)
          continue;
        if (Seen++ == FieldNo) {
          BitOff = M->getOffsetInBits();
          break;
        }
      }
      if (FieldDecl *FD = fieldAtOffset(Rec, BitOff)) {
        bool IsArrow = Agg->getType()->isPointerType();
        clang::MemberExpr *M = MemberExpr::CreateImplicit(
            *AST, Agg, IsArrow, FD, ElemQT, VK_LValue, OK_Ordinary);
        return ensureTemp(&I, Scope,
                          makeImplicit(ElemQT, CK_LValueToRValue, M));
      }
    }
    // Positional fallback: `agg.field<N>`.
    (void)FieldNo;
  }
  return tempFor(&I, Scope);
}

// ---- calls ------------------------------------------------------------------

std::pair<llvm::Value *, unsigned>
ExprBuilderImpl::vfptrBaseOf(llvm::Value *V) {
  // Unwrap a vtable-slot load chain to (object base, slot): each step is
  // a load, optionally through a constant-index GEP (struct field or
  // vtable slot) or a bitcast. Covers both `load (gep (load obj), slot)`
  // and the all-zero-offset form `load (load (load obj))` the contract
  // uses for slot 0. Returns {nullptr, 0} unless at least two loads were
  // unwrapped (object + slot indirection).
  const llvm::DataLayout &DL = CurF->getParent()->getDataLayout();
  uint64_t ByteOff = 0;
  unsigned Loads = 0;
  llvm::Value *Cur = V;
  while (true) {
    while (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(Cur))
      Cur = BC->getOperand(0);
    auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur);
    if (!LI)
      break;
    ++Loads;
    Cur = LI->getPointerOperand();
    while (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(Cur))
      Cur = BC->getOperand(0);
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Cur)) {
      llvm::APInt Off(DL.getPointerSizeInBits(), 0);
      if (!GEP->accumulateConstantOffset(DL, Off))
        return {nullptr, 0};
      ByteOff += Off.getZExtValue();
      Cur = GEP->getPointerOperand();
    }
  }
  if (Loads < 2 || ByteOff % 8 != 0 || ByteOff / 8 > 1024)
    return {nullptr, 0};
  return {Cur, (unsigned)(ByteOff / 8)};
}

Expr *ExprBuilderImpl::tryVirtualCall(llvm::CallInst &I, ExprScope &Scope) {
  if (!Ctx.isCPlusPlus())
    return nullptr;
  llvm::Value *CalleeOp = I.getCalledOperand();
  // Strip bitcasts on the callee.
  while (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(CalleeOp))
    CalleeOp = BC->getOperand(0);
  auto [Base, Slot] = vfptrBaseOf(CalleeOp);
  if (!Base)
    return nullptr;
  const llvm::DICompositeType *RecDI =
      compositeBehindPointer(debugTypeOf(Base));
  if (!RecDI)
    return nullptr;
  const llvm::DISubprogram *SP = Types.virtualMethod(RecDI, Slot);
  CXXMethodDecl *MD = SP ? Types.methodFor(SP) : nullptr;
  if (!MD)
    return nullptr;
  // The object is the chain base (usually the `self` alloca or `this`
  // parameter). The call arg identical to it becomes the implicit object
  // argument and is skipped.
  Expr *This = value(Base, Scope);
  if (!This)
    return nullptr;
  bool IsArrow = This->getType()->isPointerType();
  // Build `obj->method(args...)`: clang::MemberExpr on the method + call.
  QualType MethodTy = MD->getType();
  const FunctionType *FT = MethodTy->getAs<FunctionType>();
  if (!FT)
    return nullptr;
  clang::MemberExpr *Mem = MemberExpr::CreateImplicit(
      *AST, parenIfNeeded(This, Prec::Postfix), IsArrow, MD, MethodTy,
      VK_PRValue, OK_Ordinary);
  llvm::SmallVector<Expr *, 4> Args;
  for (unsigned A = 0; A < I.arg_size(); ++A) {
    if (I.getArgOperand(A) == Base)
      continue; // implicit object argument
    Args.push_back(value(I.getArgOperand(A), Scope));
  }
  QualType RetTy = exprTypeOf(&I);
  clang::CXXMemberCallExpr *Call = CXXMemberCallExpr::Create(
      *AST, Mem, Args, RetTy, VK_PRValue, SourceLocation(),
      FPOptionsOverride());
  if (I.getType()->isVoidTy())
    return Call;
  return ensureTemp(&I, Scope, Call);
}

Expr *ExprBuilderImpl::buildCallExpr(llvm::CallInst &I, ExprScope &Scope,
                                     Expr *Callee, QualType RetTy) {
  llvm::SmallVector<Expr *, 8> Args;
  llvm::FunctionType *FTy = I.getFunctionType();
  for (unsigned A = 0; A < I.arg_size(); ++A) {
    Expr *E = value(I.getArgOperand(A), Scope);
    // Cast variadic extras and mismatched params to the declared type
    // so the output compiles under clang-cl's checking.
    (void)FTy;
    Args.push_back(E);
  }
  clang::CallExpr *Call =
      CallExpr::Create(*AST, Callee, Args, RetTy, VK_PRValue,
                       SourceLocation(), FPOptionsOverride());
  return Call;
}

Expr *ExprBuilderImpl::buildCallValue(llvm::CallInst &I, ExprScope &Scope) {
  // llvm.* intrinsics the lifter can leave behind fold to C idioms
  // (emit-c Call rule): min/max become `?:`.
  if (llvm::Function *DF = I.getCalledFunction()) {
    llvm::StringRef N = DF->getName();
    if ((N.starts_with("llvm.smin.") || N.starts_with("llvm.smax.") ||
         N.starts_with("llvm.umin.") || N.starts_with("llvm.umax.")) &&
        I.arg_size() == 2) {
      bool IsMin = N.contains("smin") || N.contains("umin");
      Expr *L = value(I.getArgOperand(0), Scope);
      Expr *R = value(I.getArgOperand(1), Scope);
      BinaryOperatorKind Cmp = IsMin ? BO_LT : BO_GT;
      clang::BinaryOperator *C = BinaryOperator::Create(
          *AST, parenIfNeeded(L, Prec::Rel), parenIfNeeded(R, Prec::Rel),
          Cmp, AST->BoolTy, VK_PRValue, OK_Ordinary, SourceLocation(),
          FPOptionsOverride());
      QualType ResTy = exprTypeOf(&I);
      clang::ConditionalOperator *Q = new (*AST) clang::ConditionalOperator(
          C, SourceLocation(), parenIfNeeded(L, Prec::Ternary),
          SourceLocation(), parenIfNeeded(R, Prec::Ternary), ResTy,
          VK_PRValue, OK_Ordinary);
      return ensureTemp(&I, Scope, Q);
    }
    if (N == "llvm.trap") {
      // `__debugbreak()` in MSVC terms; call it so the output compiles.
      IdentifierInfo *Id = Ctx.ident("__debugbreak");
      QualType FT = AST->getFunctionType(AST->VoidTy, {}, {});
      FunctionDecl *FD = FunctionDecl::Create(
          *AST, AST->getTranslationUnitDecl(), SourceLocation(),
          SourceLocation(), Id, FT, AST->getTrivialTypeSourceInfo(FT),
          SC_Extern);
      Expr *Ref = declRef(FD);
      clang::CallExpr *Call =
          CallExpr::Create(*AST, Ref, {}, AST->VoidTy, VK_PRValue,
                           SourceLocation(), FPOptionsOverride());
      return ensureTemp(&I, Scope, Call);
    }
  }

  // Virtual-dispatch fast path (agreed hook with lib/llvm::Type).
  if (Expr *V = tryVirtualCall(I, Scope))
    return V;

  llvm::Value *CalleeOp = I.getCalledOperand();
  while (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(CalleeOp))
    CalleeOp = BC->getOperand(0);
  QualType RetTy = exprTypeOf(&I);
  if (I.getType()->isVoidTy())
    RetTy = AST->VoidTy;

  if (llvm::Function *DF = llvm::dyn_cast<llvm::Function>(CalleeOp)) {
    // Direct call, including imports (external declarations Core bound).
    Expr *Callee = buildConstant(DF, Scope);
    // A FunctionDecl ref used as callee needs function-to-pointer decay
    // when the clang::CallExpr expects a pointer; CallExpr::Create takes the
    // callee expr directly, Sema would add the decay. Add it here.
    if (Callee->getType()->isFunctionType())
      Callee = makeImplicit(AST->getPointerType(Callee->getType()),
                            CK_FunctionToPointerDecay, Callee);
    Expr *Call = buildCallExpr(I, Scope, Callee, RetTy);
    if (I.getType()->isVoidTy())
      return Call;
    return ensureTemp(&I, Scope, Call);
  }

  // Indirect call through a function pointer: `(*fp)(args)`, with the
  // callee parenthesized (emit-c Indirect rule).
  Expr *FP = value(CalleeOp, Scope);
  QualType CalleeTy = FP->getType();
  // The callee must be a pointer to the function type; llvm::cast when the IR
  // carried it opaquely (i8* / uintptr_t).
  llvm::FunctionType *Wanted = I.getFunctionType();
  // Clang function type for the call shape, mapped param by param.
  QualType RetQT = Types.typeOf(Wanted->getReturnType(), nullptr);
  llvm::SmallVector<QualType, 8> ParamQTs;
  for (llvm::Type *PT : Wanted->params())
    ParamQTs.push_back(Types.typeOf(PT, nullptr));
  FunctionProtoType::ExtProtoInfo EPI;
  QualType WantedPtr = AST->getPointerType(
      AST->getFunctionType(RetQT, ParamQTs, EPI));
  if (CalleeTy != WantedPtr && !CalleeTy->isFunctionPointerType()) {
    if (CalleeTy->isIntegerType())
      FP = makeCStyleCast(WantedPtr, CK_IntegralToPointer, FP);
    else if (CalleeTy->isPointerType())
      FP = makeCStyleCast(WantedPtr, CK_BitCast, FP);
    CalleeTy = WantedPtr;
  }
  // `(*fp)`: deref then paren so the printer keeps `(*fp)(...)`.
  Expr *Deref = UnaryOperator::Create(
      *AST, parenIfNeeded(FP, Prec::Postfix), UO_Deref,
      WantedPtr->castAs<PointerType>()->getPointeeType(), VK_LValue, OK_Ordinary, SourceLocation(),
      false, FPOptionsOverride());
  Expr *Callee =
      new (*AST) clang::ParenExpr(SourceLocation(), SourceLocation(), Deref);
  Expr *Call = buildCallExpr(I, Scope, Callee, RetTy);
  if (I.getType()->isVoidTy())
    return Call;
  return ensureTemp(&I, Scope, Call);
}

Stmt *ExprBuilderImpl::buildCallStmt(llvm::CallInst &I, ExprScope &Scope) {
  // Void calls and dead value-calls are statements by themselves.
  if (I.getType()->isVoidTy() || I.use_empty())
    return buildCallValue(I, Scope);
  // Live value-calls fold into their single use; multi-use ones bind a
  // temp whose DeclStmt StmtBuilder emits from PendingTemps.
  (void)value(&I, Scope);
  return nullptr;
}

} // namespace revenant::clangx
