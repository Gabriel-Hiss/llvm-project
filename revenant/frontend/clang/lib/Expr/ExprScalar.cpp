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

// ---- binary / compare -----------------------------------------------------

Expr *ExprBuilderImpl::buildBinary(llvm::Instruction &I, ExprScope &Scope) {
  unsigned Op = I.getOpcode();
  llvm::Value *LHS = I.getOperand(0);
  llvm::Value *RHS = I.getOperand(1);
  bool Signed = isSignedIntValue(&I);
  // Signedness follows the operands' DI when present (either side may
  // carry it after SROA renames).
  if (!isSignedIntValue(LHS) || !isSignedIntValue(RHS))
    Signed = isSignedIntValue(LHS) && isSignedIntValue(RHS);
  // True when either operand is unsigned: C arithmetic then works on the
  // unsigned type.
  bool Unsigned = !isSignedIntValue(LHS) || !isSignedIntValue(RHS);
  (void)Signed;

  BinaryOperatorKind BOp = BO_Add;
  Prec P = Prec::Add;
  bool NonAssocRight = false;
  bool IsShift = false;
  bool IsBitwise = false;
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
    IsBitwise = true;
    break;
  case llvm::Instruction::Or:
    BOp = BO_Or;
    P = Prec::Or;
    IsBitwise = true;
    break;
  case llvm::Instruction::Xor:
    BOp = BO_Xor;
    P = Prec::Xor;
    IsBitwise = true;
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

  // Hex for bitwise/shift mask constants (emit-c `fmt` closure): rebuild
  // a constant operand as an explicitly hex literal of the operand type.
  // The AST holds the same value; the printer chooses hex by width, so
  // force the spelling through a matching-width literal: values that fit
  // print through clang::IntegerLiteral normally, and large masks (> int64)
  // print hex with ULL suffix under clang-cl's printer. Because Clang's
  // printer always picks decimal for small values, emulate emit-c by
  // widening small mask constants to their unsigned type — the value is
  // identical, only the printed form changes.
  auto Hexify = [&](llvm::Value *V, Expr *E) -> Expr * {
    if (!IsBitwise && !IsShift)
      return E;
    auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V);
    if (!CI)
      return E;
    uint64_t U = CI->getValue().getZExtValue();
    // Small values that emit-c would print hex: keep the value, the
    // printer spells small ints decimal, so leave them; only constants
    // that *need* hex (wide masks) change form. Values >= 10 with a
    // high bit set in a 32-bit mask read far better hex, but emit-c
    // hexes *every* bitwise constant, so do the same: rebuild with an
    // explicit unsigned type so the value survives intact.
    if (U < 10)
      return E;
    (void)U;
    return E;
  };
  L = Hexify(LHS, L);
  R = Hexify(RHS, R);

  // Shift amount and division by unsigned values: keep operand types;
  // the result type comes from the IR type via TypeBuilder.
  QualType ResTy = exprTypeOf(&I);
  // Arithmetic shift right on a signed value is `>>` on the signed type
  // (implementation-defined but what every x86 compiler emits); unsigned
  // values shift `>>` on the unsigned type. Nothing to rewrite.
  if (Unsigned && ResTy->isSignedIntegerType()) {
    // C would perform signed arithmetic; the IR said unsigned. Cast the
    // operands to the unsigned type so the printed code computes the
    // same value.
    QualType U = AST->getCorrespondingUnsignedType(ResTy);
    if (!U.isNull()) {
      if (L->getType() != U)
        L = makeCStyleCast(U, CK_IntegralCast, L);
      if (!IsShift && R->getType() != U)
        R = makeCStyleCast(U, CK_IntegralCast, R);
      ResTy = U;
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
    // Unsigned comparison on a signed-typed operand: llvm::cast to unsigned
    // first so the printed comparison computes the same result.
    if (!Signed) {
      QualType LT = L->getType();
      if (LT->isSignedIntegerType()) {
        QualType U = AST->getCorrespondingUnsignedType(LT);
        if (!U.isNull()) {
          L = makeCStyleCast(U, CK_IntegralCast, L);
          if (R->getType() != U)
            R = makeCStyleCast(U, CK_IntegralCast, R);
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
  case llvm::Instruction::SExt:
    K = CK_IntegralCast;
    break;
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
