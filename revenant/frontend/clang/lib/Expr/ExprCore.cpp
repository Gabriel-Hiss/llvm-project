//===- ExprCore.cpp - ExprBuilder driver and helpers -------*- C++ -*-===//
//
// Entry points, precedence, literals, temporaries. Part of RevenantClangExpr.
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

std::unique_ptr<ExprBuilder> createExprBuilder(FrontendContext &Ctx,
                                               TypeBuilder &Types) {
  return std::make_unique<ExprBuilderImpl>(Ctx, Types);
}

// ---- precedence -----------------------------------------------------------

Prec ExprBuilderImpl::precedenceOf(const Expr *E) {
  if (!E)
    return Prec::Atom;
  // Strip implicit casts and parens: they do not change the printed shape.
  while (true) {
    if (auto *P = llvm::dyn_cast<clang::ParenExpr>(E)) {
      E = P->getSubExpr();
      continue;
    }
    if (auto *C = llvm::dyn_cast<clang::ImplicitCastExpr>(E)) {
      E = C->getSubExpr();
      continue;
    }
    break;
  }
  switch (E->getStmtClass()) {
  case Stmt::CallExprClass:
  case Stmt::CXXMemberCallExprClass:
  case Stmt::ArraySubscriptExprClass:
  case Stmt::IntegerLiteralClass:
  case Stmt::FloatingLiteralClass:
  case Stmt::StringLiteralClass:
  case Stmt::CharacterLiteralClass:
  case Stmt::CXXBoolLiteralExprClass:
  case Stmt::DeclRefExprClass:
  case Stmt::MemberExprClass:
    return Prec::Postfix;
  case Stmt::UnaryOperatorClass:
  case Stmt::CStyleCastExprClass:
    return Prec::Unary;
  case Stmt::BinaryOperatorClass: {
    switch (llvm::cast<clang::BinaryOperator>(E)->getOpcode()) {
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
      return Prec::Mul;
    case BO_Add:
    case BO_Sub:
      return Prec::Add;
    case BO_Shl:
    case BO_Shr:
      return Prec::Shift;
    case BO_LT:
    case BO_GT:
    case BO_LE:
    case BO_GE:
      return Prec::Rel;
    case BO_EQ:
    case BO_NE:
      return Prec::Eq;
    case BO_And:
      return Prec::And;
    case BO_Xor:
      return Prec::Xor;
    case BO_Or:
      return Prec::Or;
    case BO_LAnd:
      return Prec::AndAnd;
    case BO_LOr:
      return Prec::OrOr;
    default:
      return Prec::Atom;
    }
  }
  case Stmt::ConditionalOperatorClass:
  case Stmt::BinaryConditionalOperatorClass:
    return Prec::Ternary;
  default:
    return Prec::Atom;
  }
}

bool ExprBuilderImpl::wantHexForOperand(BinaryOperatorKind Op) {
  switch (Op) {
  case BO_And:
  case BO_Or:
  case BO_Xor:
  case BO_Shl:
  case BO_Shr:
    return true;
  default:
    return false;
  }
}

// ---- small helpers --------------------------------------------------------

QualType ExprBuilderImpl::uintptrTy() const {
  // uint64_t spelling without needing a typedef in scope: the printer
  // prints builtin types canonically.
  return AST->UnsignedLongLongTy;
}

const llvm::DIType *ExprBuilderImpl::debugTypeOf(const llvm::Value *V) const {
  auto It = DebugTypes.find(V);
  if (It != DebugTypes.end())
    return It->second;
  if (auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
    if (llvm::DISubprogram *SP = CurF ? CurF->getSubprogram() : nullptr) {
      // Arguments pair with DILocalVariables by `Arg` number (1-based).
      for (llvm::DINode *N : SP->getRetainedNodes()) {
        auto *LV = llvm::dyn_cast<llvm::DILocalVariable>(N);
        if (LV && LV->isParameter() && LV->getArg() == Arg->getArgNo() + 1)
          return LV->getType();
      }
    }
  }
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    // DbgVariableRecords attached to this instruction (new debug format)
    // carry the llvm::DILocalVariable whose type describes the value.
    for (llvm::DbgRecord &R : I->getDbgRecordRange()) {
      if (auto *VR = llvm::dyn_cast<llvm::DbgVariableRecord>(&R)) {
        if (llvm::DILocalVariable *LV = VR->getVariable()) {
          if (LV->getType())
            return LV->getType();
        }
      }
    }
  }
  return nullptr;
}

bool ExprBuilderImpl::isSignedDI(const llvm::DIType *DI) {
  if (!DI)
    return true; // emit-c default: integers are signed unless DI says so.
  if (auto *BT = llvm::dyn_cast<llvm::DIBasicType>(DI)) {
    switch (BT->getEncoding()) {
    case llvm::dwarf::DW_ATE_unsigned:
    case llvm::dwarf::DW_ATE_unsigned_char:
    case llvm::dwarf::DW_ATE_unsigned_fixed:
      return false;
    case llvm::dwarf::DW_ATE_UTF:
    case llvm::dwarf::DW_ATE_UCS:
    case llvm::dwarf::DW_ATE_ASCII:
    case llvm::dwarf::DW_ATE_address:
    case llvm::dwarf::DW_ATE_boolean:
      return false;
    default:
      return true;
    }
  }
  if (auto *DD = llvm::dyn_cast<llvm::DIDerivedType>(DI)) {
    // Typedefs inherit the carrier's signedness.
    if (DD->getTag() == llvm::dwarf::DW_TAG_typedef ||
        DD->getTag() == llvm::dwarf::DW_TAG_const_type ||
        DD->getTag() == llvm::dwarf::DW_TAG_volatile_type)
      return isSignedDI(DD->getBaseType());
    if (DD->getTag() == llvm::dwarf::DW_TAG_enumeration_type)
      return false;
  }
  return true;
}

bool ExprBuilderImpl::isSignedIntValue(const llvm::Value *V) const {
  return isSignedDI(debugTypeOf(V));
}

QualType ExprBuilderImpl::exprTypeOf(llvm::Value *V) {
  const llvm::DIType *DI = debugTypeOf(V);
  QualType T = Types.typeOf(V->getType(), DI);
  if (CurFD && V->getType() == CurF->getReturnType() &&
      (V->getType()->isStructTy() || V->getType()->isArrayTy()))
    T = CurFD->getReturnType();
  assert(!T.isNull() && "TypeBuilder must never return null");
  return T;
}

Expr *ExprBuilderImpl::declRef(ValueDecl *D) {
  QualType T = D->getType();
  ExprValueKind VK = VK_LValue;
  if (llvm::isa<ParmVarDecl>(D) || llvm::isa<FunctionDecl>(D))
    VK = VK_LValue;
  if (auto *VD = llvm::dyn_cast<VarDecl>(D))
    if (VD->getType()->isReferenceType())
      VK = VK_LValue;
  clang::DeclRefExpr *R = DeclRefExpr::Create(
      *AST, NestedNameSpecifierLoc(), SourceLocation(), D, false,
      SourceLocation(), T, VK);
  return R;
}

Expr *ExprBuilderImpl::parenIfNeeded(Expr *E, Prec Parent,
                                     bool RightOfNonAssoc) {
  Prec P = precedenceOf(E);
  if (P < Parent)
    return new (*AST) clang::ParenExpr(SourceLocation(), SourceLocation(), E);
  // Right operand of Sub/Div/Rem/Shift at equal precedence needs parens
  // (emit-c `assoc_right`); comparisons chain through the same rule for
  // readability (`a < b < c` is parsed left-assoc but means something
  // unintended, so parenthesize the right side).
  if (RightOfNonAssoc && P == Parent)
    return new (*AST) clang::ParenExpr(SourceLocation(), SourceLocation(), E);
  return E;
}

/// Clang asserts that any cast to bool carries a *ToBoolean kind (or NoOp
/// from bool): pick it from the operand's type.
static CastKind boolDestKind(QualType T, CastKind K, const Expr *Op) {
  if (!T->isBooleanType())
    return K;
  QualType S = Op->getType();
  if (S->isBooleanType())
    return CK_NoOp;
  if (S->isPointerType())
    return CK_PointerToBoolean;
  if (S->isFloatingType())
    return CK_FloatingToBoolean;
  return CK_IntegralToBoolean;
}

Expr *ExprBuilderImpl::makeCStyleCast(QualType T, CastKind K, Expr *Op) {
  // `(T)a + b` casts only `a`: a binary operand needs its own parentheses.
  Op = parenIfNeeded(Op, Prec::Unary);
  K = boolDestKind(T, K, Op);
  TypeSourceInfo *TSI = AST->getTrivialTypeSourceInfo(T);
  clang::CStyleCastExpr *C =
      CStyleCastExpr::Create(*AST, T, VK_PRValue, K, Op, nullptr,
                             FPOptionsOverride(), TSI, SourceLocation(),
                             SourceLocation());
  return C;
}

Expr *ExprBuilderImpl::makeImplicit(QualType T, CastKind K, Expr *Op) {
  ExprValueKind VK = VK_PRValue;
  if (K == CK_LValueToRValue || K == CK_ArrayToPointerDecay ||
      K == CK_FunctionToPointerDecay)
    VK = VK_PRValue;
  clang::ImplicitCastExpr *C = ImplicitCastExpr::Create(
      *AST, T, K, Op, nullptr, VK, FPOptionsOverride());
  return C;
}

// `if (x)` / `x ? :` / `!x` need a scalar truth value: pointers and
// integers are already truth-testable, floats compare against 0.
Expr *ExprBuilderImpl::truthTest(Expr *E) {
  QualType T = E->getType();
  if (T->isBooleanType())
    return E;
  if (T->isPointerType() || T->isMemberPointerType() || T->isIntegerType() ||
      T->isEnumeralType())
    return E;
  if (T->isFloatingType()) {
    Expr *Zero = clang::FloatingLiteral::Create(
        *AST, llvm::APFloat(0.0), false, T, SourceLocation());
    clang::BinaryOperator *Cmp = BinaryOperator::Create(
        *AST, E, Zero, BO_NE, AST->BoolTy, VK_PRValue, OK_Ordinary,
        SourceLocation(), FPOptionsOverride());
    return Cmp;
  }
  return E;
}

bool ExprBuilderImpl::isStoredVar(llvm::Value *PtrOp, const clang::Expr *Ptr) {
  if (!llvm::isa<llvm::AllocaInst>(PtrOp) &&
      !llvm::isa<llvm::GlobalVariable>(PtrOp))
    return false;
  // An array designator is not assignable: access it through a pointer.
  return llvm::isa<clang::DeclRefExpr>(Ptr) && !Ptr->getType()->isArrayType();
}

// ---- literals -------------------------------------------------------------

Expr *ExprBuilderImpl::buildIntegerLiteral(const llvm::APInt &V, QualType Ty) {
  // The QualType comes from TypeBuilder, which may map an IR width to a
  // different C width (e.g. i64 -> `long`, 32 bits on Windows). Clang's
  // IntegerLiteral asserts exact width match, so normalize the APInt to
  // the type width first, preserving the value under the type's own
  // signedness. Non-integer types fall back to `uintptr_t`.
  QualType T = Ty->isIntegerType() ? Ty : uintptrTy();
  // Bool context: 0/1 become `false`/`true` in C++ mode. IntegerLiteral
  // cannot carry a bool type, so C prints a plain `0`/`1`.
  if (T->isBooleanType()) {
    if (Ctx.isCPlusPlus())
      return CXXBoolLiteralExpr::Create(*AST, !V.isZero(), T,
                                        SourceLocation());
    T = AST->IntTy;
  }
  unsigned W = (unsigned)AST->getIntWidth(T);
  // No C/C++ spelling exists for a >64-bit decimal literal: Clang's
  // printer emits the raw digits and clang-cl rejects them ("integer
  // literal is too large"). Split the value into two 64-bit halves
  // joined by a shift+or, which folds to the same constant.
  if (W > 64 && !V.isZero()) {
    uint64_t Lo = V.trunc(64).getZExtValue();
    uint64_t Hi = V.lshr(64).trunc(64).getZExtValue();
    Expr *LoE = IntegerLiteral::Create(
        *AST, llvm::APInt(64, Lo), AST->UnsignedLongLongTy, SourceLocation());
    Expr *HiE = IntegerLiteral::Create(
        *AST, llvm::APInt(64, Hi), AST->UnsignedLongLongTy, SourceLocation());
    Expr *ShiftAmt = IntegerLiteral::Create(
        *AST, llvm::APInt(32, 64), AST->IntTy, SourceLocation());
    Expr *Shift = BinaryOperator::Create(*AST, HiE, ShiftAmt, BO_Shl,
                                         AST->UnsignedLongLongTy, VK_PRValue,
                                         OK_Ordinary, SourceLocation(),
                                         FPOptionsOverride());
    QualType U128 = T->isSignedIntegerType() ? AST->Int128Ty
                                             : AST->UnsignedInt128Ty;
    Expr *Or = BinaryOperator::Create(
        *AST, makeCStyleCast(U128, CK_IntegralCast, Shift),
        makeCStyleCast(U128, CK_IntegralCast, LoE), BO_Or, U128, VK_PRValue,
        OK_Ordinary, SourceLocation(), FPOptionsOverride());
    if (U128 != T)
      return Or;
    return makeCStyleCast(T, CK_IntegralCast, Or);
  }
  llvm::APInt Val = (T->isSignedIntegerType() && V.isNegative())
                        ? V.sextOrTrunc(W)
                        : V.zextOrTrunc(W);
  // Single characters in a char-typed slot print as 'x'.
  if ((T->isSpecificBuiltinType(BuiltinType::Char_S) ||
       T->isSpecificBuiltinType(BuiltinType::Char_U) ||
       T->isSpecificBuiltinType(BuiltinType::UChar) ||
       T->isSpecificBuiltinType(BuiltinType::SChar)) &&
      W == 8) {
    unsigned Ch = (unsigned)Val.getZExtValue();
    if (Ch >= 32 && Ch < 127 && Ch != '\'' && Ch != '\\')
      return new (*AST) clang::CharacterLiteral(Ch, CharacterLiteralKind::Ascii, T,
                                         SourceLocation());
  }
  return IntegerLiteral::Create(*AST, Val, T, SourceLocation());
}

Expr *ExprBuilderImpl::buildConstant(llvm::Constant *C, ExprScope &Scope) {
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(C)) {
    // Bitwise/shift mask operands print hex; values wider than int64
    // print hex with ULL (emit-c `ConstI` rule). The hex/dec choice is a
    // property of the *use* site, handled by buildBinary overriding the
    // literal; here produce the canonical decimal literal.
    return buildIntegerLiteral(CI->getValue(), exprTypeOf(C));
  }
  if (auto *CF = llvm::dyn_cast<llvm::ConstantFP>(C)) {
    const llvm::APFloat &Value = CF->getValueAPF();
    clang::QualType Ty = exprTypeOf(C);
    if (Value.isInfinity()) {
      clang::SourceLocation L;
      clang::Expr *Numerator = clang::FloatingLiteral::Create(
          *AST, llvm::APFloat(1.0), true, AST->DoubleTy, L);
      clang::Expr *Denominator = clang::FloatingLiteral::Create(
          *AST, llvm::APFloat(0.0), true, AST->DoubleTy, L);
      clang::Expr *Infinity = clang::BinaryOperator::Create(
          *AST, Numerator, Denominator, clang::BO_Div, AST->DoubleTy,
          clang::VK_PRValue, clang::OK_Ordinary, L, clang::FPOptionsOverride());
      if (Value.isNegative())
        Infinity = clang::UnaryOperator::Create(
            *AST, Infinity, clang::UO_Minus, AST->DoubleTy, clang::VK_PRValue,
            clang::OK_Ordinary, L, /*CanOverflow=*/false,
            clang::FPOptionsOverride());
      return Ty == AST->DoubleTy
                 ? Infinity
                 : makeImplicit(Ty, clang::CK_FloatingCast, Infinity);
    }
    return clang::FloatingLiteral::Create(*AST, Value, true, Ty,
                                          SourceLocation());
  }
  if (llvm::isa<llvm::ConstantPointerNull>(C)) {
    // emit-cpp prints null pointers as `nullptr` in C++ mode, `(T*)0`
    // otherwise.
    if (Ctx.isCPlusPlus()) {
      clang::CXXNullPtrLiteralExpr *N =
          new (*AST) clang::CXXNullPtrLiteralExpr(AST->NullPtrTy, SourceLocation());
      QualType T = exprTypeOf(C);
      if (!T->isNullPtrType())
        return makeImplicit(T, CK_NullToPointer, N);
      return N;
    }
    Expr *Zero = IntegerLiteral::Create(*AST, llvm::APInt(64, 0u),
                                        AST->UnsignedLongLongTy,
                                        SourceLocation());
    QualType T = exprTypeOf(C);
    if (T->isPointerType())
      return makeImplicit(T, CK_NullToPointer, Zero);
    if (!T->isIntegerType())
      T = uintptrTy();
    if (T != Zero->getType())
      return makeImplicit(T, CK_IntegralCast, Zero);
    return Zero;
  }
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
    // `@str_*` string constants become string literals.
    if (GV->getName().starts_with("str_") ||
        GV->getName().starts_with(".str")) {
      if (auto *Init = llvm::dyn_cast_or_null<llvm::ConstantDataArray>(
              GV->getInitializer())) {
        if (Init->isString() || Init->isCString()) {
          // StringLiteral stores the characters without the terminator;
          // the array type keeps the full stored extent (incl. NUL), so
          // the printer emits `"hi"` for a 3-byte `hi\0` global.
          std::string Bytes = Init->getAsString().str();
          while (!Bytes.empty() && Bytes.back() == '\0')
            Bytes.pop_back();
          QualType CharTy = Ctx.isCPlusPlus()
                                ? AST->CharTy
                                : AST->UnsignedCharTy;
          QualType ArrTy = AST->getConstantArrayType(
              CharTy, llvm::APInt(64, Bytes.size() + 1), nullptr,
              ArraySizeModifier::Normal, 0u);
          clang::StringLiteral *SL = StringLiteral::Create(
              *AST, Bytes, StringLiteralKind::Ordinary, false, ArrTy, {});
          // Decay to `const char *` like any string literal use.
          QualType PtrTy = AST->getPointerType(CharTy.withConst());
          return makeImplicit(PtrTy, CK_ArrayToPointerDecay, SL);
        }
      }
    }
    if (ValueDecl *D = Ctx.lookupDecl(GV))
      return declRef(D);
    // Unbound global: fall back to a uintptr constant of its address.
    // (Core binds all globals before bodies build; this is defensive.)
    return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                  SourceLocation());
  }
  if (auto *F = llvm::dyn_cast<llvm::Function>(C)) {
    if (ValueDecl *D = Ctx.lookupDecl(F))
      return declRef(D);
    // Forward reference: ask Core to materialize a declaration-only
    // FunctionDecl so the call still prints by name.
    FunctionDecl *FD = Ctx.functionDecl(*F, Types);
    if (FD)
      return declRef(FD);
    return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                  SourceLocation());
  }
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(C)) {
    // Fold the few constant-exprs the lifter leaves behind: gep address
    // math and inttoptr/ptrtoint/bitcast casts.
    switch (CE->getOpcode()) {
    case llvm::Instruction::GetElementPtr: {
      auto *GEP = llvm::cast<llvm::GEPOperator>(CE);
      // Share the instruction GEP path: walk the same index list over
      // the same source element type.
      Expr *Base = value(CE->getOperand(0), Scope);
      llvm::SmallVector<llvm::Value *, 4> Idxs(CE->op_begin() + 1, CE->op_end());
      Expr *R = buildGEPFrom(Base, GEP->getSourceElementType(),
                             CE->getOperand(0), Idxs, Scope);
      return R;
    }
    case llvm::Instruction::IntToPtr:
      return makeCStyleCast(exprTypeOf(C), CK_IntegralToPointer,
                            value(CE->getOperand(0), Scope));
    case llvm::Instruction::PtrToInt:
      // The operand may be an array or function designator: the cast makes
      // the decay and the integer result explicit.
      return makeCStyleCast(exprTypeOf(C), CK_PointerToIntegral,
                            value(CE->getOperand(0), Scope));
    case llvm::Instruction::BitCast:
      return value(CE->getOperand(0), Scope);
    default:
      break;
    }
    return value(CE->getOperand(0), Scope);
  }
  if (llvm::isa<llvm::UndefValue>(C) || llvm::isa<llvm::PoisonValue>(C))
    return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                  SourceLocation());
  // Last resort: zero of the value's type. Never null.
  return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                SourceLocation());
}

// ---- temporaries ----------------------------------------------------------

Expr *ExprBuilderImpl::tempFor(llvm::Instruction *I, ExprScope &Scope) {
  if (ValueDecl *D = Ctx.lookupDecl(I))
    return declRef(D);
  // Record this value so Core can declare its temporary in the function
  // prologue. Bind it now so later uses share the same VarDecl.
  if (Materialized.insert(I).second)
    Scope.PendingTemps.push_back(I);
  std::string Name = "v" + std::to_string(TempCounter++);
  IdentifierInfo *Id = Ctx.ident(Name);
  QualType T = exprTypeOf(I);
  TypeSourceInfo *TSI = AST->getTrivialTypeSourceInfo(T);
  VarDecl *VD = VarDecl::Create(*AST, CurFD, SourceLocation(),
                                SourceLocation(), Id, T, TSI, SC_None);
  Ctx.bindTemp(I, VD);
  return declRef(VD);
}

Expr *ExprBuilderImpl::ensureTemp(llvm::Instruction *I, ExprScope &Scope,
                                  clang::Expr *Inline) {
  // Values with multiple uses need a local. Core emits the declaration;
  // instr() emits the cached initializer at this instruction's position.
  // PHIs have no inline spelling and remain declaration-only here.
  bool NeedsTemp = !I->hasOneUse() && !I->use_empty();
  if (I->getOpcode() == llvm::Instruction::PHI)
    NeedsTemp = true;
  if (!NeedsTemp)
    return Inline;
  if (Materialized.insert(I).second)
    Scope.PendingTemps.push_back(I);
  // Bind now so later uses resolve even if StmtBuilder has not emitted
  // the DeclStmt yet.
  if (!Ctx.lookupDecl(I)) {
    std::string Name = "v" + std::to_string(TempCounter++);
    TypeSourceInfo *TSI =
        AST->getTrivialTypeSourceInfo(Inline->getType());
    VarDecl *VD = VarDecl::Create(*AST, CurFD, SourceLocation(),
                                  SourceLocation(), Ctx.ident(Name),
                                  Inline->getType(), TSI, SC_None);
    Ctx.bindTemp(I, VD);
  }
  Scope.PendingInitializers[I] = Inline;
  return declRef(llvm::cast<ValueDecl>(Ctx.lookupDecl(I)));
}

// ---- entry points -----------------------------------------------------------

void ExprBuilderImpl::beginFunction(llvm::Function &F, FunctionDecl *FD,
                                    ExprScope &Scope) {
  CurF = &F;
  CurFD = FD;
  AST = &Ctx.ast();
  Materialized.clear();
  DebugTypes.clear();
  Scope.PendingTemps.clear();
  Scope.PendingInitializers.clear();
  TempCounter = 0;
  ThisRecord = nullptr;
  ThisArgIndex = 0;

  // Collect dbg.declare / dbg.assign records: llvm::AllocaInst -> DI type, and
  // argument DI types by position.
  for (llvm::BasicBlock &BB : F) {
    for (llvm::Instruction &I : BB) {
      for (llvm::DbgRecord &R : I.getDbgRecordRange()) {
        auto *VR = llvm::dyn_cast<llvm::DbgVariableRecord>(&R);
        if (!VR)
          continue;
        llvm::DILocalVariable *LV = VR->getVariable();
        if (!LV || !LV->getType())
          continue;
        if (VR->isDbgDeclare() || VR->isDbgDeclareValue()) {
          llvm::Value *Addr = VR->getVariableLocationOp(0);
          if (Addr)
            DebugTypes[Addr] = LV->getType();
        } else {
          llvm::Value *Val = VR->getVariableLocationOp(0);
          if (Val)
            DebugTypes[Val] = LV->getType();
        }
      }
      if (auto *DI = llvm::dyn_cast<llvm::DbgDeclareInst>(&I)) {
        if (llvm::DILocalVariable *LV = DI->getVariable())
          if (DI->getAddress() && LV->getType())
            DebugTypes[DI->getAddress()] = LV->getType();
      } else if (auto *DV = llvm::dyn_cast<llvm::DbgValueInst>(&I)) {
        if (llvm::DILocalVariable *LV = DV->getVariable())
          if (LV->getType())
            DebugTypes[DV->getValue()] = LV->getType();
      }
    }
  }
  // Cache the `this` record: first pointer-to-record argument in C++.
  if (Ctx.isCPlusPlus() && F.arg_size() > 0) {
    unsigned Idx = 0;
    for (llvm::Argument &A : F.args()) {
      const llvm::DIType *DI = debugTypeOf(&A);
      if (const llvm::DICompositeType *Rec = compositeBehindPointer(DI)) {
        ThisRecord = Rec;
        ThisArgIndex = Idx;
        break;
      }
      ++Idx;
    }
  }
}

Expr *ExprBuilderImpl::value(llvm::Value *V, ExprScope &Scope) {
  assert(AST && "beginFunction must run before value()");
  if (auto *C = llvm::dyn_cast<llvm::Constant>(V))
    return buildConstant(C, Scope);
  if (ValueDecl *D = Ctx.lookupDecl(V))
    return declRef(D);
  if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    switch (I->getOpcode()) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::SDiv:
    case llvm::Instruction::URem:
    case llvm::Instruction::SRem:
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
    case llvm::Instruction::FAdd:
    case llvm::Instruction::FSub:
    case llvm::Instruction::FMul:
    case llvm::Instruction::FDiv:
    case llvm::Instruction::FRem:
      return buildBinary(*I, Scope);
    case llvm::Instruction::ICmp:
    case llvm::Instruction::FCmp:
      return buildCmp(llvm::cast<llvm::CmpInst>(*I), Scope);
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt:
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::BitCast:
    case llvm::Instruction::AddrSpaceCast:
      return buildCast(llvm::cast<llvm::CastInst>(*I), Scope);
    case llvm::Instruction::Load:
      return buildLoadValue(llvm::cast<llvm::LoadInst>(*I), Scope);
    case llvm::Instruction::GetElementPtr:
      return buildGEPValue(llvm::cast<llvm::GetElementPtrInst>(*I), Scope);
    case llvm::Instruction::Select:
      return buildSelect(llvm::cast<llvm::SelectInst>(*I), Scope);
    case llvm::Instruction::Call:
      return buildCallValue(llvm::cast<llvm::CallInst>(*I), Scope);
    case llvm::Instruction::PHI:
      return buildPHIValue(llvm::cast<llvm::PHINode>(*I), Scope);
    case llvm::Instruction::ExtractValue:
      return buildExtractValue(llvm::cast<llvm::ExtractValueInst>(*I), Scope);
    case llvm::Instruction::InsertValue: {
      // No inline spelling; materialize.
      return tempFor(I, Scope);
    }
    case llvm::Instruction::Alloca:
      // Allocas are bound by Core; unbound means the declare ran late.
      return tempFor(I, Scope);
    default:
      // Opaque `revenant.asm.*` calls and anything unhandled: zero of
      // uintptr type. Never null.
      return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                    SourceLocation());
    }
  }
  if (auto *A = llvm::dyn_cast<llvm::Argument>(V)) {
    // Unbound argument (Core binds all params; defensive).
    return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                  SourceLocation());
    (void)A;
  }
  return IntegerLiteral::Create(*AST, llvm::APInt(64, 0u), uintptrTy(),
                                SourceLocation());
}

Stmt *ExprBuilderImpl::instr(llvm::Instruction &I, ExprScope &Scope) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(I))
    return nullptr;
  assert(AST && "beginFunction must run before instr()");
  auto EmitTemp = [&]() -> Stmt * {
    auto It = Scope.PendingInitializers.find(&I);
    auto *D = llvm::dyn_cast_or_null<ValueDecl>(Ctx.lookupDecl(&I));
    if (It == Scope.PendingInitializers.end() || !D)
      return nullptr;
    Expr *Init = It->second;
    Scope.PendingInitializers.erase(It);
    Expr *LHS = declRef(D);
    return BinaryOperator::Create(*AST, LHS, Init, BO_Assign, D->getType(),
                                  VK_PRValue, OK_Ordinary, SourceLocation(),
                                  FPOptionsOverride());
  };
  switch (I.getOpcode()) {
  case llvm::Instruction::Store:
    return buildStore(llvm::cast<llvm::StoreInst>(I), Scope);
  case llvm::Instruction::Call:
    if (Stmt *S = buildCallStmt(llvm::cast<llvm::CallInst>(I), Scope))
      return S;
    return EmitTemp();
  case llvm::Instruction::Alloca:
  case llvm::Instruction::PHI:
    // Alloca declarations and PHI edge copies are handled separately.
    (void)value(&I, Scope);
    return nullptr;
  case llvm::Instruction::Fence:
    return nullptr;
  default:
    if (!I.getType()->isVoidTy())
      (void)value(&I, Scope);
    return EmitTemp();
  }
}


Expr *ExprBuilderImpl::retVal(llvm::Value *V, ExprScope &Scope) {
  assert(AST && "beginFunction must run before retVal()");
  if (!V)
    return nullptr;
  Expr *E = value(V, Scope);
  // Convert to the declared return type so `return v;` type-checks.
  QualType Want = CurFD ? CurFD->getReturnType() : QualType();
  if (!Want.isNull() && !Want->isVoidType() && E->getType() != Want) {
    if (E->getType()->isIntegerType() && Want->isIntegerType())
      E = makeCStyleCast(Want, CK_IntegralCast, E);
    else if (E->getType()->isPointerType() && Want->isPointerType())
      E = makeCStyleCast(Want, CK_BitCast, E);
    else if (E->getType()->isFloatingType() && Want->isFloatingType())
      E = makeCStyleCast(Want, CK_FloatingCast, E);
    else if (E->getType()->isIntegerType() && Want->isPointerType())
      E = makeCStyleCast(Want, CK_IntegralToPointer, E);
    else if (E->getType()->isPointerType() && Want->isIntegerType())
      E = makeCStyleCast(Want, CK_PointerToIntegral, E);
  }
  return E;
}

} // namespace revenant::clangx
