//===- ExprMemory.cpp - memory expression lowering -------*- C++ -*-===//
//
// Loads, stores, GEP member/subscript shapes. Part of RevenantClangExpr.
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

// ---- memory: loads, stores, GEPs ------------------------------------------

Expr *ExprBuilderImpl::decayToPointer(Expr *E, QualType Pointee) {
  QualType T = E->getType();
  if (T->isArrayType()) {
    const clang::ArrayType *AT = clang::cast<clang::ArrayType>(T.getTypePtr());
    QualType P = AST->getPointerType(AT->getElementType());
    return makeImplicit(P, CK_ArrayToPointerDecay, E);
  }
  if (T->isFunctionType()) {
    QualType P = AST->getPointerType(T);
    return makeImplicit(P, CK_FunctionToPointerDecay, E);
  }
  if (!T->isPointerType())
    return makeCStyleCast(AST->getPointerType(Pointee),
                          CK_IntegralToPointer, E);
  QualType WantPtr = AST->getPointerType(Pointee);
  if (T != WantPtr)
    return makeCStyleCast(WantPtr, CK_BitCast, E);
  return E;
}

Expr *ExprBuilderImpl::derefPointer(Expr *Ptr, QualType Pointee) {
  Expr *P = decayToPointer(Ptr, Pointee);
  // A byte-array global read as i64 decays to `unsigned char *`: view it as a
  // pointer to the accessed type instead of reading one element.
  if (const auto *PT = P->getType()->getAs<clang::PointerType>())
    if (!Pointee->isVoidType() &&
        AST->getCanonicalType(PT->getPointeeType().getUnqualifiedType()) !=
            AST->getCanonicalType(Pointee.getUnqualifiedType()))
      P = makeCStyleCast(AST->getPointerType(Pointee), CK_BitCast, P);
  // Clang's printer does not parenthesize unary operands itself
  // (Sema normally inserts ParenExpr); wrap anything below postfix so
  // `*(p + k)` does not print as `*p + k`.
  Expr *Op = parenIfNeeded(P, Prec::Unary);
  clang::UnaryOperator *D = UnaryOperator::Create(
      *AST, Op, UO_Deref, Pointee, VK_LValue, OK_Ordinary, SourceLocation(),
      false, FPOptionsOverride());
  return D;
}

const llvm::DICompositeType *
ExprBuilderImpl::compositeBehindPointer(const llvm::DIType *DI) {
  while (DI) {
    if (auto *CT = llvm::dyn_cast<llvm::DICompositeType>(DI))
      return CT;
    if (auto *DD = llvm::dyn_cast<llvm::DIDerivedType>(DI)) {
      unsigned Tag = DD->getTag();
      if (Tag == llvm::dwarf::DW_TAG_pointer_type ||
          Tag == llvm::dwarf::DW_TAG_reference_type ||
          Tag == llvm::dwarf::DW_TAG_rvalue_reference_type ||
          Tag == llvm::dwarf::DW_TAG_typedef ||
          Tag == llvm::dwarf::DW_TAG_const_type ||
          Tag == llvm::dwarf::DW_TAG_volatile_type) {
        DI = DD->getBaseType();
        continue;
      }
    }
    break;
  }
  return nullptr;
}

FieldDecl *ExprBuilderImpl::fieldAtOffset(RecordDecl *Rec, uint64_t BitOff) {
  if (!Rec || !Rec->getDefinitionOrSelf())
    return nullptr;
  Rec = Rec->getDefinitionOrSelf();
  const ASTRecordLayout &Layout = AST->getASTRecordLayout(Rec);
  for (FieldDecl *FD : Rec->fields()) {
    if (Layout.getFieldOffset(FD->getFieldIndex()) == BitOff)
      return FD;
  }
  return nullptr;
}

Expr *ExprBuilderImpl::buildLoadValue(llvm::LoadInst &I, ExprScope &Scope) {
  Expr *Ptr = value(I.getPointerOperand(), Scope);
  QualType Pointee = Types.typeOf(I.getType(), debugTypeOf(&I));
  // `this`-relative or struct GEP pointers already build clang::MemberExpr /
  // subscript shapes via buildGEPValue; a load through them is just the
  // lvalue itself (implicit lvalue-to-rvalue on use). The same holds for
  // a bound alloca/global VarDecl of non-pointer type: the DeclRef names
  // the variable, not its address.
  if (llvm::isa<clang::ArraySubscriptExpr>(Ptr) ||
      llvm::isa<clang::MemberExpr>(Ptr) ||
      isStoredVar(I.getPointerOperand(), Ptr)) {
    Expr *Use = makeImplicit(Pointee, CK_LValueToRValue, Ptr);
    return ensureTemp(&I, Scope, Use);
  }
  Expr *D = derefPointer(Ptr, Pointee);
  Expr *Use = makeImplicit(Pointee, CK_LValueToRValue, D);
  return ensureTemp(&I, Scope, Use);
}

Stmt *ExprBuilderImpl::buildStore(llvm::StoreInst &I, ExprScope &Scope) {
  Expr *Val = value(I.getValueOperand(), Scope);
  Expr *Ptr = value(I.getPointerOperand(), Scope);
  QualType Pointee = Types.typeOf(I.getValueOperand()->getType(),
                                  debugTypeOf(I.getValueOperand()));
  Expr *Dst;
  if (llvm::isa<clang::ArraySubscriptExpr>(Ptr) ||
      llvm::isa<clang::MemberExpr>(Ptr) ||
      isStoredVar(I.getPointerOperand(), Ptr))
    Dst = Ptr;
  else
    Dst = derefPointer(Ptr, Pointee);
  QualType DestType = Dst->getType();
  if (Val->getType() != DestType && !DestType.isNull()) {
    // A wide destination has no cast rule of its own.
    if (isWideIntRecord(DestType) || isWideIntRecord(Val->getType()))
      return makeWideStore(*AST, Dst, Val);
    if (Val->getType()->isIntegerType() && DestType->isIntegerType())
      Val = makeCStyleCast(DestType, CK_IntegralCast, Val);
    else if (Val->getType()->isPointerType() && DestType->isPointerType())
      Val = makeCStyleCast(DestType, CK_BitCast, Val);
    else if (Val->getType()->isFloatingType() &&
             DestType->isFloatingType())
      Val = makeCStyleCast(DestType, CK_FloatingCast, Val);
    else if (Val->getType()->isIntegerType() && DestType->isPointerType())
      Val = makeCStyleCast(DestType, CK_IntegralToPointer, Val);
    else if (Val->getType()->isPointerType() && DestType->isIntegerType())
      Val = makeCStyleCast(DestType, CK_PointerToIntegral, Val);
  }
  clang::BinaryOperator *Assign = BinaryOperator::Create(
      *AST, Dst, Val, BO_Assign, Dst->getType(), VK_PRValue, OK_Ordinary,
      SourceLocation(), FPOptionsOverride());
  return Assign;
}

Expr *ExprBuilderImpl::buildGEPValue(llvm::GetElementPtrInst &I, ExprScope &Scope) {
  llvm::SmallVector<llvm::Value *, 4> Idxs(I.indices());
  return buildGEPFrom(value(I.getPointerOperand(), Scope),
                      I.getSourceElementType(), I.getPointerOperand(), Idxs,
                      Scope);
}

Expr *ExprBuilderImpl::buildGEPFrom(Expr *Base, llvm::Type *SrcElem, llvm::Value *PtrOp,
                                    llvm::ArrayRef<llvm::Value *> Idxs,
                                    ExprScope &Scope) {
  llvm::Type *SrcElemTy = SrcElem;
  const llvm::DataLayout &DL = CurF->getParent()->getDataLayout();

  // DI composite behind the base pointer, for member naming.
  const llvm::DICompositeType *RecDI = compositeBehindPointer(debugTypeOf(PtrOp));
  RecordDecl *Rec = nullptr;
  if (RecDI)
    Rec = Types.recordFor(RecDI);

  // Walk indices: first index steps over the source element (folded for
  // inbounds pointer arithmetic), struct indices pick fields, array
  // indices subscript.
  bool First = true;
  Expr *Cur = Base;
  llvm::Type *CurTy = SrcElemTy;
  for (llvm::Value *Idx : Idxs) {
    if (First) {
      First = false;
      // Index 0 into the element type is the identity; anything else is
      // pointer arithmetic over whole elements.
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Idx)) {
        if (CI->isZero())
          continue;
        uint64_t ElemSize = DL.getTypeAllocSize(CurTy);
        Expr *Off;
        if (ElemSize == 1) {
          Off = value(Idx, Scope);
        } else {
          Expr *N = value(Idx, Scope);
          Expr *Sz = IntegerLiteral::Create(*AST, llvm::APInt(64, ElemSize),
                                            AST->UnsignedLongLongTy,
                                            SourceLocation());
          Off = BinaryOperator::Create(
              *AST, parenIfNeeded(N, Prec::Mul), Sz, BO_Mul,
              AST->UnsignedLongLongTy, VK_PRValue, OK_Ordinary,
              SourceLocation(), FPOptionsOverride());
        }
        QualType BytePtr = AST->getPointerType(AST->UnsignedCharTy);
        Expr *B = decayToPointer(Cur, AST->UnsignedCharTy);
        if (B->getType() != BytePtr)
          B = makeCStyleCast(BytePtr, CK_BitCast, B);
        Expr *O = parenIfNeeded(Off, Prec::Add);
        Expr *P = BinaryOperator::Create(
            *AST, B, O, BO_Add, BytePtr, VK_PRValue, OK_Ordinary,
            SourceLocation(), FPOptionsOverride());
        Cur = P;
        continue;
      }
      // Dynamic first index: p + i (elements). Scale folded like above.
      uint64_t ElemSize = DL.getTypeAllocSize(CurTy);
      Expr *N = value(Idx, Scope);
      if (ElemSize != 1) {
        Expr *Sz = IntegerLiteral::Create(*AST, llvm::APInt(64, ElemSize),
                                          AST->UnsignedLongLongTy,
                                          SourceLocation());
        N = BinaryOperator::Create(*AST, parenIfNeeded(N, Prec::Mul), Sz,
                                   BO_Mul, AST->UnsignedLongLongTy,
                                   VK_PRValue, OK_Ordinary, SourceLocation(),
                                   FPOptionsOverride());
      }
      QualType BytePtr = AST->getPointerType(AST->UnsignedCharTy);
      Expr *B = decayToPointer(Cur, AST->UnsignedCharTy);
      if (B->getType() != BytePtr)
        B = makeCStyleCast(BytePtr, CK_BitCast, B);
      Cur = BinaryOperator::Create(*AST, B, parenIfNeeded(N, Prec::Add),
                                   BO_Add, BytePtr, VK_PRValue, OK_Ordinary,
                                   SourceLocation(), FPOptionsOverride());
      continue;
    }
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(CurTy)) {
      unsigned FieldNo = (unsigned)llvm::cast<llvm::ConstantInt>(Idx)->getZExtValue();
      // Member access when DI knows the field: `base->field`.
      const llvm::DIType *FieldDI = nullptr;
      uint64_t BitOff = 0;
      if (RecDI) {
        llvm::DINodeArray Elems = RecDI->getElements();
        unsigned Seen = 0;
        for (llvm::DINode *N : Elems) {
          auto *M = llvm::dyn_cast<llvm::DIDerivedType>(N);
          if (!M || M->getTag() != llvm::dwarf::DW_TAG_member)
            continue;
          if (Seen++ == FieldNo) {
            FieldDI = M->getBaseType();
            BitOff = M->getOffsetInBits();
            break;
          }
        }
      } else {
        // No DI: byte offset from the LLVM layout.
        const llvm::StructLayout *SL = DL.getStructLayout(ST);
        BitOff = SL->getElementOffsetInBits(FieldNo);
      }
      FieldDecl *FD = Rec ? fieldAtOffset(Rec, BitOff) : nullptr;
      QualType ElemQT = Types.typeOf(ST->getElementType(FieldNo), FieldDI);
      if (FD) {
        // Arrow when Cur is a pointer, dot when already dereferenced.
        bool IsArrow = Cur->getType()->isPointerType();
        clang::MemberExpr *M = MemberExpr::CreateImplicit(
            *AST, parenIfNeeded(Cur, Prec::Postfix), IsArrow, FD, ElemQT,
            VK_LValue, OK_Ordinary);
        Cur = M;
      } else {
        // Unknown field: `base->field<N>` via a byte-offset pointer.
        uint64_t ByteOff = BitOff / 8;
        QualType BytePtr = AST->getPointerType(AST->UnsignedCharTy);
        Expr *B = decayToPointer(Cur, AST->UnsignedCharTy);
        if (B->getType() != BytePtr)
          B = makeCStyleCast(BytePtr, CK_BitCast, B);
        Expr *O = IntegerLiteral::Create(*AST, llvm::APInt(64, ByteOff),
                                         AST->UnsignedLongLongTy,
                                         SourceLocation());
        Expr *P = BinaryOperator::Create(
            *AST, B, O, BO_Add, BytePtr, VK_PRValue, OK_Ordinary,
            SourceLocation(), FPOptionsOverride());
        Cur = derefPointer(P, ElemQT);
      }
      CurTy = ST->getElementType(FieldNo);
      // The field's own composite becomes the record for deeper indices.
      if (FieldDI) {
        if (const llvm::DICompositeType *Sub = compositeBehindPointer(FieldDI)) {
          RecDI = Sub;
          Rec = Types.recordFor(RecDI);
        } else {
          Rec = nullptr;
        }
      }
      continue;
    }
    if (CurTy->isArrayTy() || CurTy->isVectorTy() || CurTy->isPointerTy()) {
      // Array subscript. The base must be an lvalue array/pointer;
      // pointer-arithmetic temporaries decay first.
      Expr *Index = value(Idx, Scope);
      llvm::Type *ElemTy = nullptr;
      if (CurTy->isArrayTy())
        ElemTy = CurTy->getArrayElementType();
      else if (CurTy->isVectorTy())
        ElemTy = llvm::cast<llvm::VectorType>(CurTy)->getElementType();
      else
        ElemTy = SrcElemTy; // opaque pointer: element type is the source
      QualType ElemQT =
          Types.typeOf(ElemTy, RecDI ? nullptr : debugTypeOf(PtrOp));
      Expr *B = Cur;
      if (!B->getType()->isArrayType() && !B->getType()->isPointerType())
        B = decayToPointer(B, ElemQT);
      // clang::ArraySubscriptExpr requires (pointer-or-vector, integer) order
      // with the value kinds Clang expects.
      if (B->getType()->isVectorType()) {
        // Vector element access shares the spelling.
      }
      clang::ArraySubscriptExpr *Sub = new (*AST) clang::ArraySubscriptExpr(
          parenIfNeeded(B, Prec::Postfix), parenIfNeeded(Index, Prec::Ternary),
          ElemQT, VK_LValue, OK_Ordinary, SourceLocation());
      Cur = Sub;
      CurTy = ElemTy;
      Rec = nullptr;
      continue;
    }
    // Scalar step (should not happen for well-formed GEPs): treat as
    // pointer arithmetic.
    Expr *Index = value(Idx, Scope);
    QualType P = Cur->getType();
    if (!P->isPointerType())
      P = AST->getPointerType(AST->UnsignedCharTy);
    Cur = BinaryOperator::Create(*AST, Cur, parenIfNeeded(Index, Prec::Add),
                                 BO_Add, P, VK_PRValue, OK_Ordinary,
                                 SourceLocation(), FPOptionsOverride());
  }
  if (Cur == Base)
    return Base;
  return Cur;
}

} // namespace revenant::clangx
