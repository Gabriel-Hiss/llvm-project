//===- X86LiftVector.cpp - typed vector semantics for the lift table -------===//
//
// The generated table inverts the backend's SelectionDAG patterns. For the
// vector instructions each node keeps its machine type, so the generated code
// builds ordinary typed IR (`add <16 x i8>`) and calls vecOp for the X86ISD
// nodes whose meaning is a lane permutation, a lane-wise compare or a mask
// extraction. Every helper here is width generic: xmm and ymm forms share the
// code and differ only in the vector type.
//
//===----------------------------------------------------------------------===//

#include "revenant/Lift/X86LiftContext.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"

using namespace llvm;
using namespace llvm::revenant;

Type *X86LiftContext::vecTy(Type *Elt, unsigned N) {
  return FixedVectorType::get(Elt, N);
}

Value *X86LiftContext::vcast(Value *V, Type *Ty) {
  if (V->getType() == Ty)
    return V;
  return fromInt(toInt(V, Ty->getPrimitiveSizeInBits()), Ty);
}

namespace {

unsigned numElts(Type *T) {
  return cast<FixedVectorType>(T)->getNumElements();
}

unsigned eltBits(Type *T) { return T->getScalarSizeInBits(); }

} // namespace

Value *X86LiftContext::vecOp(StringRef Kind, Type *VT, ArrayRef<Value *> A,
                             int64_t Imm) {
  const bool IsVec = VT->isVectorTy();
  const unsigned N = IsVec ? numElts(VT) : 1;
  const unsigned E = IsVec ? eltBits(VT) : VT->getPrimitiveSizeInBits();
  const unsigned Total = VT->getPrimitiveSizeInBits();
  // Elements per 128-bit lane (the unit the x86 in-lane shuffles work in).
  const unsigned LaneElts = std::min(N, 128 / E);
  const unsigned Lanes = N / LaneElts;
  Type *IVT = IsVec ? static_cast<Type *>(vecTy(iN(E), N)) : iN(E);

  auto Zero = [&] { return Constant::getNullValue(VT); };
  auto Shuf = [&](Value *X, Value *Y, ArrayRef<int> M) {
    return vcast(B.CreateShuffleVector(X, Y, M), VT);
  };
  auto Same = [&](Value *V) { return vcast(V, VT); };
  auto IntOf = [&](Value *V) { return vcast(V, IVT); };

  if (Kind == "cmpeq" || Kind == "cmpgt") {
    Value *R = B.CreateICmp(Kind == "cmpeq" ? CmpInst::ICMP_EQ
                                            : CmpInst::ICMP_SGT,
                            IntOf(A[0]), IntOf(A[1]));
    return Same(B.CreateSExt(R, IVT));
  }
  if (Kind == "blendv") {
    // (mask, if-set, if-clear): each lane follows its mask lane's sign bit.
    Value *M = IntOf(A[0]);
    Value *Cond = B.CreateICmpSLT(M, Constant::getNullValue(IVT));
    return B.CreateSelect(Cond, Same(A[1]), Same(A[2]));
  }
  if (Kind == "movmsk") {
    Type *ST = A[0]->getType();
    unsigned SE = eltBits(ST), SN = numElts(ST);
    Value *V = vcast(A[0], vecTy(iN(SE), SN));
    Value *Signs = B.CreateICmpSLT(V, Constant::getNullValue(V->getType()));
    return B.CreateZExtOrTrunc(B.CreateBitCast(Signs, iN(SN)), VT);
  }
  if (Kind == "unpckl" || Kind == "unpckh") {
    SmallVector<int, 64> M;
    for (unsigned L = 0; L < Lanes; ++L) {
      unsigned Base = L * LaneElts;
      unsigned Off = Kind == "unpckl" ? 0 : LaneElts / 2;
      for (unsigned I = 0; I < LaneElts / 2; ++I) {
        M.push_back(Base + Off + I);
        M.push_back(N + Base + Off + I);
      }
    }
    return Shuf(Same(A[0]), Same(A[1]), M);
  }
  if (Kind == "pshufd") {
    // 32-bit elements: imm picks four per lane. 64-bit (vpermilpd): one bit
    // per element.
    SmallVector<int, 64> M;
    for (unsigned L = 0; L < Lanes; ++L)
      for (unsigned J = 0; J < LaneElts; ++J) {
        unsigned Pick = E == 64 ? (Imm >> (L * LaneElts + J)) & 1
                                : (Imm >> (2 * J)) & 3;
        M.push_back(L * LaneElts + Pick);
      }
    return Shuf(Same(A[0]), UndefValue::get(VT), M);
  }
  if (Kind == "pshuflw" || Kind == "pshufhw") {
    SmallVector<int, 64> M;
    for (unsigned L = 0; L < Lanes; ++L)
      for (unsigned J = 0; J < 8; ++J) {
        bool Hit = Kind == "pshuflw" ? J < 4 : J >= 4;
        unsigned Pick = Hit ? (Imm >> (2 * (J & 3))) & 3 : J & 3;
        M.push_back(L * 8 + (Hit ? (J & 4) + Pick : J));
      }
    return Shuf(Same(A[0]), UndefValue::get(VT), M);
  }
  if (Kind == "shufp") {
    SmallVector<int, 64> M;
    for (unsigned L = 0; L < Lanes; ++L) {
      unsigned Base = L * LaneElts;
      if (E == 32) {
        M.push_back(Base + (Imm & 3));
        M.push_back(Base + ((Imm >> 2) & 3));
        M.push_back(N + Base + ((Imm >> 4) & 3));
        M.push_back(N + Base + ((Imm >> 6) & 3));
      } else {
        M.push_back(Base + ((Imm >> (2 * L)) & 1));
        M.push_back(N + Base + ((Imm >> (2 * L + 1)) & 1));
      }
    }
    return Shuf(Same(A[0]), Same(A[1]), M);
  }
  if (Kind == "alignr") {
    // (hi, lo, bytes): per 128-bit lane, bytes of (hi:lo) shifted right.
    Type *BT = vecTy(iN(8), Total / 8);
    unsigned NB = Total / 8;
    SmallVector<int, 64> M;
    SmallVector<Constant *, 64> Keep;
    for (unsigned L = 0; L < NB / 16; ++L)
      for (unsigned I = 0; I < 16; ++I) {
        uint64_t S = I + Imm;
        if (S < 16) {
          M.push_back(L * 16 + S);
          Keep.push_back(ConstantInt::get(iN(8), 0xff));
        } else if (S < 32) {
          M.push_back(NB + L * 16 + (S - 16));
          Keep.push_back(ConstantInt::get(iN(8), 0xff));
        } else {
          M.push_back(0);
          Keep.push_back(ConstantInt::get(iN(8), 0));
        }
      }
    Value *R = B.CreateShuffleVector(vcast(A[1], BT), vcast(A[0], BT), M);
    return Same(B.CreateAnd(R, ConstantVector::get(Keep)));
  }
  if (Kind == "perm2x128") {
    unsigned H = N / 2;
    SmallVector<int, 32> M;
    SmallVector<Constant *, 32> Keep;
    for (unsigned K = 0; K < 2; ++K) {
      unsigned Sel = (Imm >> (4 * K)) & 0xf;
      for (unsigned I = 0; I < H; ++I) {
        M.push_back((Sel & 2 ? N : 0) + (Sel & 1 ? H : 0) + I);
        Keep.push_back(ConstantInt::get(iN(E), Sel & 8 ? 0 : ~0ull));
      }
    }
    Value *R = B.CreateShuffleVector(IntOf(A[0]), IntOf(A[1]), M);
    return Same(B.CreateAnd(R, ConstantVector::get(Keep)));
  }
  if (Kind == "permq") {
    SmallVector<int, 8> M;
    for (unsigned J = 0; J < N; ++J)
      M.push_back((J & ~3u) + ((Imm >> (2 * (J & 3))) & 3));
    return Shuf(Same(A[0]), UndefValue::get(VT), M);
  }
  if (Kind == "permv") {
    // (indices, source): result[i] = source[indices[i] mod N].
    Value *Idx = IntOf(A[0]);
    Value *Src = Same(A[1]);
    Value *R = UndefValue::get(VT);
    for (unsigned I = 0; I < N; ++I) {
      Value *Ix = B.CreateAnd(B.CreateExtractElement(Idx, I),
                              ConstantInt::get(iN(E), N - 1));
      R = B.CreateInsertElement(R, B.CreateExtractElement(Src, Ix), I);
    }
    return R;
  }
  if (Kind == "pshufb") {
    unsigned NB = Total / 8;
    Type *BT = vecTy(iN(8), NB);
    Value *Data = vcast(A[0], BT), *Ctl = vcast(A[1], BT);
    Value *R = UndefValue::get(BT);
    for (unsigned I = 0; I < NB; ++I) {
      Value *C = B.CreateExtractElement(Ctl, I);
      Value *Ix = B.CreateAdd(B.CreateAnd(C, ConstantInt::get(iN(8), 15)),
                              ConstantInt::get(iN(8), I & ~15u));
      Value *V = B.CreateExtractElement(Data, Ix);
      Value *Neg = B.CreateICmpSLT(C, ConstantInt::get(iN(8), 0));
      R = B.CreateInsertElement(
          R, B.CreateSelect(Neg, ConstantInt::get(iN(8), 0), V), I);
    }
    return Same(R);
  }
  if (Kind == "movs") {
    SmallVector<int, 4> M;
    for (unsigned J = 0; J < N; ++J)
      M.push_back(J == 0 ? N : J);
    return Shuf(Same(A[0]), Same(A[1]), M);
  }
  if (Kind == "movlhps") {
    SmallVector<int, 4> M = {0, 1, int(N), int(N + 1)};
    return Shuf(Same(A[0]), Same(A[1]), M);
  }
  if (Kind == "movhlps") {
    SmallVector<int, 4> M = {int(N + 2), int(N + 3), 2, 3};
    return Shuf(Same(A[0]), Same(A[1]), M);
  }
  if (Kind == "vzmovl") {
    SmallVector<int, 64> M;
    for (unsigned J = 0; J < N; ++J)
      M.push_back(J == 0 ? 0 : N);
    return Shuf(Same(A[0]), Zero(), M);
  }
  if (Kind == "s2v") {
    Type *Elt = cast<VectorType>(VT)->getElementType();
    return B.CreateInsertElement(Zero(), vcast(A[0], Elt), uint64_t(0));
  }
  if (Kind == "broadcast") {
    Type *Elt = cast<VectorType>(VT)->getElementType();
    Type *ST = A[0]->getType();
    Value *S = ST->isVectorTy()
                   ? B.CreateExtractElement(
                         vcast(A[0], vecTy(Elt, ST->getPrimitiveSizeInBits() /
                                                    Elt->getPrimitiveSizeInBits())),
                         uint64_t(0))
                   : vcast(A[0], Elt);
    return B.CreateVectorSplat(N, S);
  }
  if (Kind == "insert_sub") {
    unsigned Mn = numElts(A[1]->getType());
    Value *Sub = A[1];
    SmallVector<int, 64> Widen;
    for (unsigned J = 0; J < N; ++J)
      Widen.push_back(J < Mn ? J : -1);
    Value *W = B.CreateShuffleVector(vcast(Sub, vecTy(cast<VectorType>(VT)
                                                          ->getElementType(),
                                                      Mn)),
                                     Widen);
    SmallVector<int, 64> M;
    for (unsigned J = 0; J < N; ++J)
      M.push_back(J >= Imm && J < Imm + Mn ? N + (J - Imm) : J);
    return Shuf(Same(A[0]), W, M);
  }
  if (Kind == "extract_sub") {
    SmallVector<int, 32> M;
    for (unsigned J = 0; J < N; ++J)
      M.push_back(Imm + J);
    Value *Src = A[0];
    return vcast(B.CreateShuffleVector(Src, M), VT);
  }
  if (Kind == "fmin" || Kind == "fmax") {
    // x86 min/max return the second operand unless the first strictly wins.
    Value *X = Same(A[0]), *Y = Same(A[1]);
    Value *C = Kind == "fmin" ? B.CreateFCmpOLT(X, Y) : B.CreateFCmpOGT(X, Y);
    return B.CreateSelect(C, X, Y);
  }
  if (Kind == "bitand" || Kind == "bitor" || Kind == "bitxor" ||
      Kind == "andn") {
    Type *WT = iN(Total);
    Value *X = vcast(A[0], WT), *Y = vcast(A[1], WT);
    Value *R = Kind == "bitand"  ? B.CreateAnd(X, Y)
               : Kind == "bitor"  ? B.CreateOr(X, Y)
               : Kind == "bitxor" ? B.CreateXor(X, Y)
                                  : B.CreateAnd(B.CreateNot(X), Y);
    return Same(R);
  }
  if (Kind == "shli" || Kind == "srli" || Kind == "srai") {
    Value *V = Same(A[0]);
    uint64_t C = Imm;
    if (C >= E) {
      if (Kind != "srai")
        return Zero();
      C = E - 1;
    }
    Value *S = B.CreateVectorSplat(N, ConstantInt::get(iN(E), C));
    return Kind == "shli"   ? B.CreateShl(V, S)
           : Kind == "srli" ? B.CreateLShr(V, S)
                            : B.CreateAShr(V, S);
  }
  if (Kind == "shl" || Kind == "srl" || Kind == "sra") {
    // One count (low 64 bits of the second operand) for every lane.
    Value *Cnt = B.CreateExtractElement(vcast(A[1], vecTy(iN(64), 2)),
                                        uint64_t(0));
    Value *Big = B.CreateICmpUGE(Cnt, ConstantInt::get(iN(64), E));
    Value *Safe = B.CreateSelect(
        Big, ConstantInt::get(iN(64), Kind == "sra" ? E - 1 : 0), Cnt);
    Value *S = B.CreateVectorSplat(N, B.CreateTrunc(Safe, iN(E)));
    Value *V = Same(A[0]);
    Value *R = Kind == "shl"   ? B.CreateShl(V, S)
               : Kind == "srl" ? B.CreateLShr(V, S)
                               : B.CreateAShr(V, S);
    return Kind == "sra" ? R : B.CreateSelect(Big, Zero(), R);
  }
  if (Kind == "shlv" || Kind == "srlv" || Kind == "srav") {
    Value *V = Same(A[0]), *Cnt = IntOf(A[1]);
    Value *Big = B.CreateICmpUGE(Cnt, B.CreateVectorSplat(
                                          N, ConstantInt::get(iN(E), E)));
    Value *Safe = B.CreateSelect(
        Big,
        B.CreateVectorSplat(
            N, ConstantInt::get(iN(E), Kind == "srav" ? E - 1 : 0)),
        Cnt);
    Value *R = Kind == "shlv"   ? B.CreateShl(V, Safe)
               : Kind == "srlv" ? B.CreateLShr(V, Safe)
                                : B.CreateAShr(V, Safe);
    return Kind == "srav" ? R : B.CreateSelect(Big, Zero(), R);
  }
  if (Kind == "zext_invec" || Kind == "sext_invec") {
    SmallVector<int, 32> M;
    for (unsigned J = 0; J < N; ++J)
      M.push_back(J);
    Value *Lo = B.CreateShuffleVector(A[0], M);
    return Kind == "zext_invec" ? B.CreateZExt(Lo, VT) : B.CreateSExt(Lo, VT);
  }
  if (Kind == "cmpp") {
    static const CmpInst::Predicate P[8] = {
        CmpInst::FCMP_OEQ, CmpInst::FCMP_OLT, CmpInst::FCMP_OLE,
        CmpInst::FCMP_UNO, CmpInst::FCMP_UNE, CmpInst::FCMP_UGE,
        CmpInst::FCMP_UGT, CmpInst::FCMP_ORD};
    Type *FT = vecTy(E == 32 ? Type::getFloatTy(Ctx) : Type::getDoubleTy(Ctx), N);
    Value *R = B.CreateFCmp(P[Imm & 7], vcast(A[0], FT), vcast(A[1], FT));
    return Same(B.CreateSExt(R, IVT));
  }
  if (Kind == "muludq" || Kind == "muldq") {
    Value *X = IntOf(A[0]), *Y = IntOf(A[1]);
    if (Kind == "muludq") {
      Constant *M = ConstantInt::get(iN(64), 0xffffffffull);
      Constant *MV = ConstantVector::getSplat(ElementCount::getFixed(N), M);
      return Same(B.CreateMul(B.CreateAnd(X, MV), B.CreateAnd(Y, MV)));
    }
    Constant *S = ConstantVector::getSplat(ElementCount::getFixed(N),
                                           ConstantInt::get(iN(64), 32));
    auto SX = [&](Value *V) { return B.CreateAShr(B.CreateShl(V, S), S); };
    return Same(B.CreateMul(SX(X), SX(Y)));
  }
  if (Kind == "avg") {
    // (a + b + 1) >> 1 in a lane twice as wide.
    Type *WV = vecTy(iN(E * 2), N);
    Value *X = B.CreateZExt(IntOf(A[0]), WV), *Y = B.CreateZExt(IntOf(A[1]), WV);
    Value *One = B.CreateVectorSplat(N, ConstantInt::get(iN(E * 2), 1));
    return Same(B.CreateTrunc(B.CreateLShr(B.CreateAdd(B.CreateAdd(X, Y), One),
                                           One),
                              IVT));
  }
  if (Kind == "packss" || Kind == "packus") {
    // Source lanes are twice VT's element width; narrow with saturation.
    unsigned SE = E * 2, SN = N / 2;
    Type *ST = vecTy(iN(SE), SN);
    unsigned PerLane = LaneElts / 2;
    SmallVector<int, 64> M;
    for (unsigned L = 0; L < Lanes; ++L) {
      for (unsigned I = 0; I < PerLane; ++I)
        M.push_back(L * PerLane + I);
      for (unsigned I = 0; I < PerLane; ++I)
        M.push_back(SN + L * PerLane + I);
    }
    Value *W = B.CreateShuffleVector(vcast(A[0], ST), vcast(A[1], ST), M);
    Type *WT = vecTy(iN(SE), N);
    Value *Hi, *Lo;
    if (Kind == "packss") {
      Hi = ConstantInt::get(WT, APInt::getSignedMaxValue(E).sext(SE));
      Lo = ConstantInt::get(WT, APInt::getSignedMinValue(E).sext(SE));
    } else {
      Hi = ConstantInt::get(WT, APInt::getMaxValue(E).zext(SE));
      Lo = Constant::getNullValue(WT);
    }
    Value *C = B.CreateBinaryIntrinsic(Intrinsic::smax,
                                       B.CreateBinaryIntrinsic(Intrinsic::smin,
                                                               W, Hi),
                                       Lo);
    return Same(B.CreateTrunc(C, IVT));
  }
  return nullptr;
}

Value *X86LiftContext::vecConvert(StringRef Kind, Value *A, Type *VT,
                                  unsigned Lanes) {
  unsigned N = numElts(VT);
  Type *Elt = cast<VectorType>(VT)->getElementType();
  SmallVector<int, 32> Low;
  for (unsigned J = 0; J < Lanes; ++J)
    Low.push_back(J);
  Value *Src = B.CreateShuffleVector(A, Low);
  Type *CT = vecTy(Elt, Lanes);
  Value *C = Kind == "sitofp"   ? B.CreateSIToFP(Src, CT)
             : Kind == "fptosi" ? B.CreateFPToSI(Src, CT)
             : Kind == "fpext"  ? B.CreateFPExt(Src, CT)
                                : B.CreateFPTrunc(Src, CT);
  if (Lanes == N)
    return C;
  SmallVector<int, 32> Pad;
  for (unsigned J = 0; J < N; ++J)
    Pad.push_back(J < Lanes ? J : Lanes);
  return B.CreateShuffleVector(C, Constant::getNullValue(CT), Pad);
}
