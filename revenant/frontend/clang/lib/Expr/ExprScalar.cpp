//===- ExprScalar.cpp - scalar expression lowering -------*- C++ -*-===//
//
// Arithmetic, compares, casts, conditions. Part of RevenantClangExpr.
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

// True when Clang has a signed counterpart type for `T`; the frontend
// models IR i1 as BoolTy, which reads as an unsigned integer type but has
// none (getCorrespondingSignedType asserts on it). Enums resolve through
// their underlying integer type.
static bool hasSignedCounterpart(QualType T) {
  if (const auto *ED = T->getAsEnumDecl())
    T = ED->getIntegerType();
  if (T->isBitIntType())
    return !T->isBooleanType();
  const auto *BT = T->getAs<BuiltinType>();
  if (!BT)
    return false;
  switch (BT->getKind()) {
  case BuiltinType::Char_U:
  case BuiltinType::UChar:
  case BuiltinType::Char8:
  case BuiltinType::UShort:
  case BuiltinType::UInt:
  case BuiltinType::ULong:
  case BuiltinType::ULongLong:
  case BuiltinType::UInt128:
  case BuiltinType::WChar_U:
    return true;
  default:
    return false;
  }
}

// Signed value of an IR i1 operand as a C int: sign-extend means -1 or 0,
// so `x` becomes `-x` (promoted to int). Constants fold to the literal.
static Expr *asSignedI1(clang::ASTContext &AST, const llvm::Value *V,
                        Expr *E) {
  if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V))
    return clang::IntegerLiteral::Create(
        AST, CI->getValue().sextOrTrunc(32), AST.IntTy, SourceLocation());
  QualType T = E->getType();
  if (!T->isBooleanType() && !T->isAnyCharacterType())
    E = CStyleCastExpr::Create(
        AST, AST.IntTy, VK_PRValue, CK_IntegralCast, E, nullptr,
        FPOptionsOverride(), AST.getTrivialTypeSourceInfo(AST.IntTy),
        SourceLocation(), SourceLocation());
  return UnaryOperator::Create(AST, E, UO_Minus, AST.IntTy, VK_PRValue,
                               OK_Ordinary, SourceLocation(), false,
                               FPOptionsOverride());
}
// ---- binary / compare -----------------------------------------------------

Expr *ExprBuilderImpl::buildBinary(llvm::Instruction &I, ExprScope &Scope) {
  unsigned Op = I.getOpcode();
  llvm::Value *LHS = I.getOperand(0);
  llvm::Value *RHS = I.getOperand(1);
  // DI signedness describes the source variable, not the operation selected
  // by LLVM. Signed and unsigned opcodes below define arithmetic semantics.

  BinaryOperatorKind BOp = BO_Add;
  Prec P = Prec::Add;
  bool NonAssocRight = false;
  bool IsShift = false;
  switch (Op) {
  case llvm::Instruction::Add:
    BOp = BO_Add;
    P = Prec::Add;
    break;
  case llvm::Instruction::Sub:
    BOp = BO_Sub;
    P = Prec::Add;
    NonAssocRight = true;
    break;
  case llvm::Instruction::Mul:
    BOp = BO_Mul;
    P = Prec::Mul;
    break;
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SDiv:
    BOp = BO_Div;
    P = Prec::Mul;
    NonAssocRight = true;
    break;
  case llvm::Instruction::URem:
  case llvm::Instruction::SRem:
    BOp = BO_Rem;
    P = Prec::Mul;
    NonAssocRight = true;
    break;
  case llvm::Instruction::Shl:
    BOp = BO_Shl;
    P = Prec::Shift;
    NonAssocRight = true;
    IsShift = true;
    break;
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
    BOp = BO_Shr;
    P = Prec::Shift;
    NonAssocRight = true;
    IsShift = true;
    break;
  case llvm::Instruction::And:
    BOp = BO_And;
    P = Prec::And;
    break;
  case llvm::Instruction::Or:
    BOp = BO_Or;
    P = Prec::Or;
    break;
  case llvm::Instruction::Xor:
    BOp = BO_Xor;
    P = Prec::Xor;
    break;
  case llvm::Instruction::FAdd:
    BOp = BO_Add;
    P = Prec::Add;
    break;
  case llvm::Instruction::FSub:
    BOp = BO_Sub;
    P = Prec::Add;
    NonAssocRight = true;
    break;
  case llvm::Instruction::FMul:
    BOp = BO_Mul;
    P = Prec::Mul;
    break;
  case llvm::Instruction::FDiv:
    BOp = BO_Div;
    P = Prec::Mul;
    NonAssocRight = true;
    break;
  case llvm::Instruction::FRem:
    BOp = BO_Rem;
    P = Prec::Mul;
    NonAssocRight = true;
    break;
  default:
    return nullptr;
  }

  Expr *L = value(LHS, Scope);
  Expr *R = value(RHS, Scope);

  // `x + -k` prints as `x - k` (emit-c Bin rule).
  if (BOp == BO_Add) {
    if (auto *RC = llvm::dyn_cast<llvm::ConstantInt>(RHS)) {
      if (RC->getValue().isNegative()) {
        llvm::APInt Neg = -RC->getValue();
        Expr *K = buildIntegerLiteral(Neg, R->getType());
        Expr *Lp = parenIfNeeded(L, Prec::Add);
        Expr *Rp = parenIfNeeded(K, Prec::Add, true);
        clang::BinaryOperator *B = BinaryOperator::Create(
            *AST, Lp, Rp, BO_Sub, exprTypeOf(&I), VK_PRValue, OK_Ordinary,
            SourceLocation(), FPOptionsOverride());
        return ensureTemp(llvm::cast<llvm::Instruction>(&I), Scope, B);
      }
    }
  }


  QualType ResTy = exprTypeOf(&I);
  // A wide record has no operator spelling in C; the model spells the
  // operator as a call to one of its wrappers.
  if (clang::Expr *Wide = wideIntArithmetic(*AST, ResTy, Op, 0, L, R))
    return ensureTemp(llvm::cast<llvm::Instruction>(&I), Scope, Wide);

  bool SignedOp = Op == llvm::Instruction::SDiv ||
                  Op == llvm::Instruction::SRem ||
                  Op == llvm::Instruction::AShr;
  bool UnsignedOp = Op == llvm::Instruction::UDiv ||
                    Op == llvm::Instruction::URem ||
                    Op == llvm::Instruction::LShr;
  if ((SignedOp || UnsignedOp) && L->getType()->isIntegerType()) {
    // Signed ops over IR i1 need each operand's signed value (-1 or 0);
    // the unsigned path needs none: bool prints as unsigned already and
    // getCorrespondingSignedType has no entry for BoolTy.
    if (SignedOp && LHS->getType()->isIntegerTy(1)) {
      L = asSignedI1(*AST, LHS, L);
      R = asSignedI1(*AST, RHS, R);
      ResTy = AST->IntTy;
    } else {
    QualType OperandTy = L->getType();
    if (SignedOp && hasSignedCounterpart(OperandTy))
      OperandTy = AST->getCorrespondingSignedType(OperandTy);
    else if (UnsignedOp && !OperandTy->isUnsignedIntegerType() &&
        (OperandTy->isSignedIntegerType() || OperandTy->isUnsignedIntegerType()))
      OperandTy = AST->getCorrespondingUnsignedType(OperandTy);
    if (!OperandTy.isNull()) {
      if (L->getType() != OperandTy)
        L = makeCStyleCast(OperandTy, CK_IntegralCast, L);
      if (!IsShift && R->getType() != OperandTy)
        R = makeCStyleCast(OperandTy, CK_IntegralCast, R);
      ResTy = OperandTy;
    }
    }
  }

  Expr *Lp = parenIfNeeded(L, P);
  Expr *Rp = parenIfNeeded(R, P, NonAssocRight);
  clang::BinaryOperator *B = BinaryOperator::Create(
      *AST, Lp, Rp, BOp, ResTy, VK_PRValue, OK_Ordinary, SourceLocation(),
      FPOptionsOverride());
  return ensureTemp(llvm::cast<llvm::Instruction>(&I), Scope, B);
}

Expr *ExprBuilderImpl::buildCmp(llvm::CmpInst &I, ExprScope &Scope) {
  Expr *L = value(I.getOperand(0), Scope);
  Expr *R = value(I.getOperand(1), Scope);
  BinaryOperatorKind BOp = BO_EQ;
  Prec P = Prec::Eq;
  bool NonAssocRight = true;
  bool Signed = isSignedIntValue(I.getOperand(0));
  if (auto *IC = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
    switch (IC->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      BOp = BO_EQ;
      P = Prec::Eq;
      break;
    case llvm::CmpInst::ICMP_NE:
      BOp = BO_NE;
      P = Prec::Eq;
      break;
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_SGT:
      BOp = BO_GT;
      P = Prec::Rel;
      Signed = IC->getPredicate() == llvm::CmpInst::ICMP_SGT;
      break;
    case llvm::CmpInst::ICMP_UGE:
    case llvm::CmpInst::ICMP_SGE:
      BOp = BO_GE;
      P = Prec::Rel;
      Signed = IC->getPredicate() == llvm::CmpInst::ICMP_SGE;
      break;
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_SLT:
      BOp = BO_LT;
      P = Prec::Rel;
      Signed = IC->getPredicate() == llvm::CmpInst::ICMP_SLT;
      break;
    case llvm::CmpInst::ICMP_ULE:
    case llvm::CmpInst::ICMP_SLE:
      BOp = BO_LE;
      P = Prec::Rel;
      Signed = IC->getPredicate() == llvm::CmpInst::ICMP_SLE;
      break;
    default:
      break;
    }
    // C's usual arithmetic conversions can erase LLVM's signed predicate.
    // Cast both operands to the predicate's signedness before comparison.
    bool IsRelational = BOp == BO_GT || BOp == BO_GE || BOp == BO_LT ||
                       BOp == BO_LE;
    if (IsRelational && L->getType()->isIntegerType()) {
      if (Signed && I.getOperand(0)->getType()->isIntegerTy(1)) {
        // Signed compare over IR i1: -1 or 0 per side.
        L = asSignedI1(*AST, I.getOperand(0), L);
        R = asSignedI1(*AST, I.getOperand(1), R);
      } else {
      QualType OperandTy = L->getType();
      if (Signed && hasSignedCounterpart(OperandTy))
        OperandTy = AST->getCorrespondingSignedType(OperandTy);
      else if (!Signed && !OperandTy->isUnsignedIntegerType() &&
          (OperandTy->isSignedIntegerType() || OperandTy->isUnsignedIntegerType()))
        OperandTy = AST->getCorrespondingUnsignedType(OperandTy);
      if (!OperandTy.isNull()) {
        if (L->getType() != OperandTy)
          L = makeCStyleCast(OperandTy, CK_IntegralCast, L);
        if (R->getType() != OperandTy)
          R = makeCStyleCast(OperandTy, CK_IntegralCast, R);
      }
      }
    }
  } else if (auto *FC = llvm::dyn_cast<llvm::FCmpInst>(&I)) {
    switch (FC->getPredicate()) {
    case llvm::CmpInst::FCMP_OEQ:
    case llvm::CmpInst::FCMP_UEQ:
      BOp = BO_EQ;
      P = Prec::Eq;
      break;
    case llvm::CmpInst::FCMP_ONE:
    case llvm::CmpInst::FCMP_UNE:
      BOp = BO_NE;
      P = Prec::Eq;
      break;
    case llvm::CmpInst::FCMP_OGT:
    case llvm::CmpInst::FCMP_UGT:
      BOp = BO_GT;
      P = Prec::Rel;
      break;
    case llvm::CmpInst::FCMP_OGE:
    case llvm::CmpInst::FCMP_UGE:
      BOp = BO_GE;
      P = Prec::Rel;
      break;
    case llvm::CmpInst::FCMP_OLT:
    case llvm::CmpInst::FCMP_ULT:
      BOp = BO_LT;
      P = Prec::Rel;
      break;
    case llvm::CmpInst::FCMP_OLE:
    case llvm::CmpInst::FCMP_ULE:
      BOp = BO_LE;
      P = Prec::Rel;
      break;
    default:
      // unordered/ordered-anything predicates have no C spelling;
      // emit `!(x == x)`-style false via a comparison to itself.
      BOp = BO_NE;
      P = Prec::Eq;
      R = L;
      break;
    }
  }
  Expr *Lp = parenIfNeeded(L, P);
  Expr *Rp = parenIfNeeded(R, P, NonAssocRight);
  QualType BoolTy =
      Ctx.isCPlusPlus() ? AST->BoolTy : AST->UnsignedCharTy;
  // A wide record has no comparison spelling in C.
  if (clang::Expr *Wide = wideIntArithmetic(
          *AST, L->getType(), llvm::Instruction::ICmp,
          (unsigned)llvm::cast<llvm::CmpInst>(&I)->getPredicate(), L, R))
    return ensureTemp(&I, Scope, Wide);
  clang::BinaryOperator *B =
      BinaryOperator::Create(*AST, Lp, Rp, BOp, BoolTy, VK_PRValue,
                             OK_Ordinary, SourceLocation(),
                             FPOptionsOverride());
  return ensureTemp(&I, Scope, B);
}

// ---- casts ---------------------------------------------------------------

Expr *ExprBuilderImpl::buildCast(llvm::CastInst &I, ExprScope &Scope) {
  Expr *Op = value(I.getOperand(0), Scope);
  QualType Dst = exprTypeOf(&I);
  llvm::Type *SrcTy = I.getOperand(0)->getType();
  llvm::Type *DstTy = I.getType();
  CastKind K = CK_NoOp;
  switch (I.getOpcode()) {
  case llvm::Instruction::Trunc:
    K = DstTy->isIntegerTy(1) ? CK_IntegralToBoolean : CK_IntegralCast;
    break;
  case llvm::Instruction::ZExt:
    K = Op->getType()->isBooleanType() ? CK_BooleanToSignedIntegral
                                       : CK_IntegralCast;
    break;
  case llvm::Instruction::SExt: {
    K = CK_IntegralCast;
    if (SrcTy->isIntegerTy(1)) {
      // i1 sign-extends to -1 or 0; there is no signed C narrow type for
      // the source, so the sign fill is spelled explicitly.
      auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(0));
      unsigned Wide =
          DstTy->isIntegerTy() ? DstTy->getIntegerBitWidth() : 128;
      if (isWideIntRecord(Dst)) {
        Expr *Neg = nullptr;
        if (CI)
          Neg = wideIntLiteral(*AST, Dst, CI->getValue().sextOrTrunc(Wide));
        else if (Expr *B0 = wideIntConvert(*AST, Dst, Op, CK_IntegralCast))
          Neg = wideIntArithmetic(
              *AST, Dst, llvm::Instruction::Sub, 0,
              wideIntLiteral(*AST, Dst, llvm::APInt(Wide, 0)), B0);
        if (Neg) {
          Op = Neg;
          break;
        }
      } else if (Dst->isIntegerType()) {
        if (CI)
          Op = buildIntegerLiteral(CI->getValue().sextOrTrunc(Wide), Dst);
        else
          Op = UnaryOperator::Create(
              *AST, makeCStyleCast(Dst, CK_IntegralCast, Op), UO_Minus, Dst,
              VK_PRValue, OK_Ordinary, SourceLocation(), false,
              FPOptionsOverride());
        break;
      }
      // Non-integer, non-record destination: fall through to the generic
      // cast (BoolTy has no signed counterpart to cast through).
    }
    // For sext, cast through the signed narrow type first so the
    // compiler sign-extends: (long long)(int)a1, not (unsigned long long)a1.
    if (SrcTy->isIntegerTy() && DstTy->isIntegerTy() &&
        SrcTy->getIntegerBitWidth() < DstTy->getIntegerBitWidth()) {
      QualType Narrow = Types.typeOf(SrcTy, nullptr);
      if (hasSignedCounterpart(Narrow)) {
        QualType SignedNarrow = AST->getCorrespondingSignedType(Narrow);
        if (!SignedNarrow.isNull() && SignedNarrow != Op->getType())
          Op = makeCStyleCast(SignedNarrow, CK_IntegralCast, Op);
      }
    }
    break;
  }
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
    K = CK_FloatingCast;
    break;
  case llvm::Instruction::FPToUI:
    K = CK_FloatingToIntegral;
    break;
  case llvm::Instruction::FPToSI:
    K = CK_FloatingToIntegral;
    break;
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
    K = CK_IntegralToFloating;
    break;
  case llvm::Instruction::PtrToInt:
    K = CK_PointerToIntegral;
    break;
  case llvm::Instruction::IntToPtr:
    K = CK_IntegralToPointer;
    break;
  case llvm::Instruction::BitCast:
    if (DstTy->isPointerTy() && SrcTy->isPointerTy())
      K = CK_BitCast;
    else if (DstTy->isIntegerTy() && SrcTy->isFloatingPointTy())
      K = CK_FloatingToIntegral;
    else if (DstTy->isFloatingPointTy() && SrcTy->isIntegerTy())
      K = CK_IntegralToFloating;
    else
      K = CK_BitCast;
    break;
  case llvm::Instruction::AddrSpaceCast:
    K = CK_AddressSpaceConversion;
    break;
  default:
    K = CK_NoOp;
    break;
  }
  // A wide record has no C cast spelling; the model builds the compound
  // literal or reads `.lo` instead of a C-style cast.
  if (clang::Expr *Wide = wideIntConvert(*AST, Dst, Op, K))
    return Wide;
  // Collapse same-width sign-flip llvm::cast chains ((u64)((i64)x) -> (u64)x),
  // mirroring emit-c's Cast rule.
  if (auto *Inner = llvm::dyn_cast<clang::CStyleCastExpr>(Op)) {
    QualType IT = Inner->getType();
    if (IT->isIntegerType() && Dst->isIntegerType() &&
        AST->getTypeSize(IT) == AST->getTypeSize(Dst)) {
      Op = Inner->getSubExpr();
    }
  }
  // Same-type llvm::cast is a no-op: fold away.
  if (Op->getType() == Dst)
    return Op;
  Expr *C = makeCStyleCast(Dst, K, parenIfNeeded(Op, Prec::Unary));
  // Casts fold into their use unless multi-use.
  if (I.hasOneUse())
    return C;
  return ensureTemp(&I, Scope, C);
}

Expr *ExprBuilderImpl::cond(llvm::Value *Cond, ExprScope &Scope) {
  assert(AST && "beginFunction must run before cond()");
  // IR-level logical not is `xor i1 %c, true`; flip the comparison
  // directly (`!(a < b)` -> `a >= b`, emit-c Unary rule) before building.
  if (auto *Xor = llvm::dyn_cast<llvm::Instruction>(Cond)) {
    if (Xor->getOpcode() == llvm::Instruction::Xor &&
        Xor->getType()->isIntegerTy(1)) {
      for (llvm::Value *Op : {Xor->getOperand(0), Xor->getOperand(1)}) {
        if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Op)) {
          if (CI->isOne()) {
            llvm::Value *Inner = (Op == Xor->getOperand(0))
                                     ? Xor->getOperand(1)
                                     : Xor->getOperand(0);
            if (llvm::isa<llvm::CmpInst>(Inner)) {
              // BinaryOperator kind after building the inner compare.
              Expr *C = value(Inner, Scope);
              if (auto *B =
                      llvm::dyn_cast<clang::BinaryOperator>(C)) {
                BinaryOperatorKind Flip = BO_EQ;
                bool Flippable = true;
                switch (B->getOpcode()) {
                case BO_EQ:
                  Flip = BO_NE;
                  break;
                case BO_NE:
                  Flip = BO_EQ;
                  break;
                case BO_LT:
                  Flip = BO_GE;
                  break;
                case BO_LE:
                  Flip = BO_GT;
                  break;
                case BO_GT:
                  Flip = BO_LE;
                  break;
                case BO_GE:
                  Flip = BO_LT;
                  break;
                default:
                  Flippable = false;
                  break;
                }
                if (Flippable) {
                  clang::BinaryOperator *F = BinaryOperator::Create(
                      *AST, B->getLHS(), B->getRHS(), Flip, B->getType(),
                      VK_PRValue, OK_Ordinary, SourceLocation(),
                      FPOptionsOverride());
                  return F;
                }
              }
              return truthTest(C);
            }
          }
        }
      }
    }
  }
  Expr *C = value(Cond, Scope);
  // `!(a < b)` flips to `a >= b` (emit-c Unary rule).
  if (auto *UO = llvm::dyn_cast<clang::UnaryOperator>(C)) {
    if (UO->getOpcode() == UO_LNot) {
      if (auto *B = llvm::dyn_cast<clang::BinaryOperator>(UO->getSubExpr())) {
        BinaryOperatorKind Flip = BO_EQ;
        bool Flippable = true;
        switch (B->getOpcode()) {
        case BO_EQ:
          Flip = BO_NE;
          break;
        case BO_NE:
          Flip = BO_EQ;
          break;
        case BO_LT:
          Flip = BO_GE;
          break;
        case BO_LE:
          Flip = BO_GT;
          break;
        case BO_GT:
          Flip = BO_LE;
          break;
        case BO_GE:
          Flip = BO_LT;
          break;
        default:
          Flippable = false;
          break;
        }
        if (Flippable) {
          clang::BinaryOperator *F = BinaryOperator::Create(
              *AST, B->getLHS(), B->getRHS(), Flip, B->getType(),
              VK_PRValue, OK_Ordinary, SourceLocation(),
              FPOptionsOverride());
          return F;
        }
      }
    }
  }
  return truthTest(C);
}

Expr *ExprBuilderImpl::inverted(llvm::Value *Cond, ExprScope &Scope) {
  assert(AST && "beginFunction must run before inverted()");
  // Canonical negation: predicate flip on comparisons, else `!x`.
  Expr *C = cond(Cond, Scope);
  if (auto *B = llvm::dyn_cast<clang::BinaryOperator>(C)) {
    BinaryOperatorKind Flip = BO_EQ;
    bool Flippable = true;
    switch (B->getOpcode()) {
    case BO_EQ:
      Flip = BO_NE;
      break;
    case BO_NE:
      Flip = BO_EQ;
      break;
    case BO_LT:
      Flip = BO_GE;
      break;
    case BO_LE:
      Flip = BO_GT;
      break;
    case BO_GT:
      Flip = BO_LE;
      break;
    case BO_GE:
      Flip = BO_LT;
      break;
    default:
      Flippable = false;
      break;
    }
    if (Flippable) {
      return BinaryOperator::Create(*AST, B->getLHS(), B->getRHS(), Flip,
                                    B->getType(), VK_PRValue, OK_Ordinary,
                                    SourceLocation(), FPOptionsOverride());
    }
  }
  // `!!x` folds back to a truth test instead of stacking negations.
  if (auto *UO = llvm::dyn_cast<clang::UnaryOperator>(C)) {
    if (UO->getOpcode() == UO_LNot)
      return truthTest(UO->getSubExpr());
  }
  return UnaryOperator::Create(*AST, parenIfNeeded(C, Prec::Unary), UO_LNot,
                               AST->BoolTy, VK_PRValue, OK_Ordinary,
                               SourceLocation(), false, FPOptionsOverride());
}

} // namespace revenant::clangx
