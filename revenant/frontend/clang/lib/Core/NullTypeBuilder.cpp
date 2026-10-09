//===- NullTypeBuilder.cpp - fallback TypeBuilder ---------------*- C++ -*-===//
//
// Minimal TypeBuilder so the host produces a compilable TU before lib/Type
// lands. Maps IR types onto Clang builtins; DI only refines signedness.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Clang/FrontendContext.h"
#include "llvm/ADT/APInt.h"
#include "llvm/BinaryFormat/Dwarf.h"

using llvm::APInt;
using llvm::ArrayRef;
using llvm::SmallVector;
using llvm::Twine;

namespace revenant::clangx {

namespace c = clang;

NullTypeBuilder::NullTypeBuilder(FrontendContext &Ctx) : Ctx(Ctx) {}

/// Unsigned builtin of `Bits`; wraps to Int128 and finally to an i8 array.
static c::QualType intType(c::ASTContext &AST, unsigned Bits, bool Signed) {
  c::QualType T;
  if (Bits <= 1)
    T = AST.UnsignedCharTy;
  else if (Bits <= 8)
    T = AST.UnsignedCharTy;
  else if (Bits <= 16)
    T = AST.UnsignedShortTy;
  else if (Bits <= 32)
    T = AST.UnsignedIntTy;
  else if (Bits <= 64)
    T = AST.UnsignedLongLongTy;
  else if (Bits <= 128)
    T = AST.getIntTypeForBitwidth(128, /*Signed=*/false);
  else
    return AST.getConstantArrayType(
        AST.UnsignedCharTy, APInt(64, (Bits + 7) / 8), nullptr,
        c::ArraySizeModifier::Normal, 0);
  if (Signed)
    T = AST.getCorrespondingSignedType(T);
  return T;
}

/// DI signedness hint: DW_ATE_signed/signed_char -> signed.
static bool diSigned(const llvm::DIType *DI) {
  auto *B = llvm::dyn_cast_or_null<llvm::DIBasicType>(DI);
  if (!B)
    return false;
  unsigned E = B->getEncoding();
  return E == llvm::dwarf::DW_ATE_signed ||
         E == llvm::dwarf::DW_ATE_signed_char;
}

c::QualType NullTypeBuilder::typeOf(llvm::Type *Ty, const llvm::DIType *DI) {
  c::ASTContext &AST = Ctx.ast();
  if (!Ty)
    return intType(AST, 64, false);
  switch (Ty->getTypeID()) {
  case llvm::Type::VoidTyID:
    return AST.VoidTy;
  case llvm::Type::HalfTyID:
    return AST.HalfTy;
  case llvm::Type::BFloatTyID:
    return AST.getIntTypeForBitwidth(16, false);
  case llvm::Type::FloatTyID:
    return AST.FloatTy;
  case llvm::Type::DoubleTyID:
    return AST.DoubleTy;
  case llvm::Type::X86_FP80TyID:
    return AST.LongDoubleTy;
  case llvm::Type::IntegerTyID: {
    unsigned Bits = llvm::cast<llvm::IntegerType>(Ty)->getBitWidth();
    return intType(AST, Bits, diSigned(DI));
  }
  case llvm::Type::PointerTyID:
    return AST.getPointerType(AST.VoidTy);
  case llvm::Type::ArrayTyID: {
    auto *AT = llvm::cast<llvm::ArrayType>(Ty);
    c::QualType Elem = typeOf(AT->getElementType(), nullptr);
    return AST.getConstantArrayType(Elem, APInt(64, AT->getNumElements()),
                                    nullptr, c::ArraySizeModifier::Normal,
                                    0);
  }
  case llvm::Type::StructTyID: {
    auto *ST = llvm::cast<llvm::StructType>(Ty);
    if (DI)
      return typeOfDI(DI);
    unsigned Bytes = 0;
    const llvm::DataLayout &DL = Ctx.module().getDataLayout();
    if (!ST->isOpaque())
      Bytes = (unsigned)DL.getTypeStoreSize(ST);
    if (Bytes == 0)
      Bytes = 1;
    return AST.getConstantArrayType(AST.UnsignedCharTy, APInt(64, Bytes),
                                    nullptr, c::ArraySizeModifier::Normal,
                                    0);
  }
  case llvm::Type::FunctionTyID: {
    c::QualType FT =
        functionTypeFor(llvm::cast<llvm::FunctionType>(Ty), nullptr);
    return AST.getPointerType(FT);
  }
  case llvm::Type::FixedVectorTyID:
  case llvm::Type::ScalableVectorTyID: {
    auto *VT = llvm::cast<llvm::VectorType>(Ty);
    c::QualType Elem = typeOf(VT->getElementType(), nullptr);
    unsigned N = VT->getElementCount().getKnownMinValue();
    if (N == 0)
      N = 1;
    return AST.getConstantArrayType(Elem, APInt(64, N), nullptr,
                                    c::ArraySizeModifier::Normal, 0);
  }
  default:
    return intType(AST, 64, false);
  }
}

c::QualType NullTypeBuilder::functionTypeFor(llvm::FunctionType *FTy,
                                             const llvm::DIType *DI) {
  c::ASTContext &AST = Ctx.ast();
  c::QualType Ret = typeOf(FTy->getReturnType(), DI);
  SmallVector<c::QualType, 8> Params;
  for (llvm::Type *P : FTy->params())
    Params.push_back(typeOf(P, nullptr));
  c::FunctionProtoType::ExtProtoInfo EPI;
  EPI.Variadic = FTy->isVarArg();
  return AST.getFunctionType(Ret, Params, EPI);
}

c::QualType NullTypeBuilder::typeOfDI(const llvm::DIType *DI) {
  c::ASTContext &AST = Ctx.ast();
  if (!DI)
    return intType(AST, 64, false);
  if (auto *B = llvm::dyn_cast<llvm::DIBasicType>(DI)) {
    unsigned Bits = B->getSizeInBits();
    if (!Bits)
      Bits = 64;
    if (B->getEncoding() == llvm::dwarf::DW_ATE_float)
      return Bits <= 32 ? AST.FloatTy : AST.DoubleTy;
    if (B->getEncoding() == llvm::dwarf::DW_ATE_boolean)
      return AST.BoolTy;
    return intType(AST, Bits, diSigned(B));
  }
  if (auto *D = llvm::dyn_cast<llvm::DIDerivedType>(DI)) {
    if (D->getTag() == llvm::dwarf::DW_TAG_pointer_type ||
        D->getTag() == llvm::dwarf::DW_TAG_reference_type ||
        D->getTag() == llvm::dwarf::DW_TAG_rvalue_reference_type)
      return AST.getPointerType(AST.VoidTy);
    if (auto *Base = llvm::dyn_cast_or_null<llvm::DIType>(D->getBaseType()))
      return typeOfDI(Base);
  }
  return intType(AST, DI->getSizeInBits() ? DI->getSizeInBits() : 64, false);
}

c::QualType NullTypeBuilder::functionType(llvm::Function &F) {
  const llvm::DIType *DI = nullptr;
  if (auto *SP = F.getSubprogram())
    DI = SP->getType();
  return functionTypeFor(F.getFunctionType(), DI);
}

c::QualType NullTypeBuilder::globalType(llvm::GlobalVariable &GV) {
  return typeOf(GV.getValueType(), nullptr);
}

c::RecordDecl *NullTypeBuilder::recordFor(const llvm::DICompositeType *) {
  return nullptr;
}

const llvm::DISubprogram *
NullTypeBuilder::virtualMethod(const llvm::DICompositeType *, unsigned) {
  return nullptr;
}

c::CXXMethodDecl *NullTypeBuilder::methodFor(const llvm::DISubprogram *) {
  return nullptr;
}

} // namespace revenant::clangx
