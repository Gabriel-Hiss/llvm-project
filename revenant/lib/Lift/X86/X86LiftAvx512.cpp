//===- X86LiftAvx512.cpp - AVX-512 mask registers and masked ops ---------===//
//
// AVX-512 introduces eight mask registers (K0-K7), each holding one bit per
// lane, plus a masking modifier on most vector operations. Both map onto IR
// the compiler itself produces for the matching intrinsics:
//
//   - A k register is an i64 bit pattern. K0 is hardwired to all-ones, so a
//     read of K0 yields -1 and a write is dropped.
//   - A mask modifier is a `<N x i1>` vector. `select <N x i1> k, op, merge`
//     is exactly the merge-masking form; `merge = zeroinitializer` is the
//     zero-masking ({z}) form. That is the shape clang emits for
//     _mm512_mask_* / _mm512_maskz_* (see the intrinsic tests).
//   - The default-rounding FMA is `llvm.fma`, while an explicit embedded
//     rounding (the {er} forms) has no plain-IR spelling and calls the same
//     `llvm.x86.avx512.vfmadd.*` intrinsic clang emits for
//     _mm512_fmadd_round_*.
//
//===----------------------------------------------------------------------===//

#include "revenant/Lift/X86LiftContext.h"
#include "X86MCTargetDesc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace llvm::revenant;

namespace {

Intrinsic::ID x86Intrinsic(StringRef Name) {
  std::string FullName = (Twine("llvm.") + Name).str();
  return static_cast<Intrinsic::ID>(Intrinsic::lookupIntrinsicID(FullName));
}

} // namespace

// ---- mask registers --------------------------------------------------------

Value *X86LiftContext::kregRead(MCRegister Reg) {
  // K0 is not a real register: every read sees all-ones so that an
  // unmodified mask is a no-op select.
  if (Reg == X86::K0)
    return constI(-1, 64);
  unsigned I = Reg >= X86::K0 && Reg <= X86::K7 ? Reg - X86::K0 : 0;
  if (!KRegs[I])
    KRegs[I] = regSlot(("k" + Twine(I)).str(), iN(64));
  return B.CreateLoad(iN(64), KRegs[I]);
}

void X86LiftContext::kregWrite(MCRegister Reg, Value *V) {
  if (Reg == X86::K0)
    return;
  unsigned I = Reg >= X86::K0 && Reg <= X86::K7 ? Reg - X86::K0 : 0;
  if (!KRegs[I])
    KRegs[I] = regSlot(("k" + Twine(I)).str(), iN(64));
  B.CreateStore(zextOrTrunc(V, iN(64)), KRegs[I]);
}

void X86LiftContext::kregMerge(MCRegister Reg, Value *V) {
  // The 8/16/32-bit KMOV forms merge into the wider register: only the
  // low bits of the incoming value change.
  if (Reg == X86::K0)
    return;
  unsigned I = Reg >= X86::K0 && Reg <= X86::K7 ? Reg - X86::K0 : 0;
  if (!KRegs[I])
    KRegs[I] = regSlot(("k" + Twine(I)).str(), iN(64));
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *Old = B.CreateLoad(iN(64), KRegs[I]);
  Value *Keep = B.CreateAnd(Old, constI(~0ull << W, 64));
  B.CreateStore(B.CreateOr(Keep, B.CreateZExt(zextOrTrunc(V, iN(W)), iN(64))),
                KRegs[I]);
}

void X86LiftContext::kregCompare(MCRegister Dst, unsigned Pred, bool Unsigned,
                                 Value *A, Value *Bv, unsigned W,
                                 unsigned EltBits) {
  unsigned N = W / EltBits;
  Type *VT = vecTy(iN(EltBits), N);
  Value *X = vcast(A, VT), *Y = vcast(Bv, VT);
  // Intel predicate: bit 0 -> less-than, bit 1 -> equal, bit 2 -> negate.
  auto Cmp = [&](CmpInst::Predicate P) { return B.CreateICmp(P, X, Y); };
  Value *Lt = Unsigned ? Cmp(CmpInst::ICMP_ULT) : Cmp(CmpInst::ICMP_SLT);
  Value *Eq = Cmp(CmpInst::ICMP_EQ);
  Value *Le = Unsigned ? Cmp(CmpInst::ICMP_ULE) : Cmp(CmpInst::ICMP_SLE);
  Value *Res;
  switch (Pred & 7) {
  case 0: Res = Eq; break;                                    // eq
  case 1: Res = Lt; break;                                    // lt
  case 2: Res = Le; break;                                    // le
  case 3: Res = Constant::getNullValue(vecTy(iN(1), N)); break; // false
  case 4: Res = B.CreateNot(Eq); break;                       // ne
  case 5: Res = B.CreateNot(Lt); break;                       // nlt (ge)
  case 6: Res = B.CreateNot(Le); break;                       // nle (gt)
  default: Res = Constant::getAllOnesValue(vecTy(iN(1), N)); break; // true
  }
  kregWrite(Dst, maskToKreg(Res));
}

Value *X86LiftContext::kregToMask(Value *K, unsigned N) {
  // Bit i of the k register selects lane i: the i64 is a `<N x i1>` vector
  // with its low N bits used and the rest ignored by the vector ops.
  Value *Bits = zextOrTrunc(K, iN(N));
  return B.CreateBitCast(Bits, vecTy(iN(1), N));
}

Value *X86LiftContext::maskToKreg(Value *Mask) {
  unsigned N = cast<FixedVectorType>(Mask->getType())->getNumElements();
  return B.CreateZExt(B.CreateBitCast(Mask, iN(N)), iN(64));
}

// ---- masked operations -----------------------------------------------------

Value *X86LiftContext::avx512Select(Value *K, Value *T, Value *F, unsigned W,
                                    unsigned EltBits) {
  unsigned N = W / EltBits;
  Value *TrueV = vcast(T, vecTy(iN(EltBits), N));
  Value *FalseV =
      F ? vcast(F, vecTy(iN(EltBits), N))
        : Constant::getNullValue(vecTy(iN(EltBits), N));
  // A null mask is the unmasked form: every lane takes the true arm.
  if (!K)
    return B.CreateBitCast(TrueV, iN(W));
  return B.CreateBitCast(
      B.CreateSelect(kregToMask(K, N), TrueV, FalseV), iN(W));
}

Value *X86LiftContext::avx512MaskedMove(Value *K, Value *Src, Value *Merge,
                                        unsigned W, unsigned EltBits) {
  return avx512Select(K, Src, Merge, W, EltBits);
}

Value *X86LiftContext::avx512MaskedLoad(Value *K, Value *AddrI64, unsigned W,
                                        unsigned EltBits, Value *Merge) {
  unsigned N = W / EltBits;
  Type *LaneTy = vecTy(iN(EltBits), N);
  Value *Mask = kregToMask(K, N);
  Value *Passthru =
      Merge ? vcast(Merge, LaneTy) : Constant::getNullValue(LaneTy);
  Value *L = B.CreateMaskedLoad(LaneTy, B.CreateIntToPtr(AddrI64, ptrTy()),
                                Align(EltBits / 8), Mask, Passthru);
  return vcast(L, iN(W));
}

void X86LiftContext::avx512MaskedStore(Value *K, Value *V, Value *AddrI64,
                                       unsigned W, unsigned EltBits) {
  unsigned N = W / EltBits;
  Type *LaneTy = vecTy(iN(EltBits), N);
  Value *Mask = kregToMask(K, N);
  B.CreateMaskedStore(vcast(V, LaneTy), B.CreateIntToPtr(AddrI64, ptrTy()),
                      Align(EltBits / 8), Mask);
}

// ---- FMA -------------------------------------------------------------------

Value *X86LiftContext::avx512FmaMasked(StringRef Op, Value *K, Value *A,
                                       Value *Bv, Value *C, Value *Merge,
                                       unsigned W, unsigned EltBits) {
  Type *FT = EltBits == 64 ? static_cast<Type *>(Type::getDoubleTy(Ctx))
                           : static_cast<Type *>(Type::getFloatTy(Ctx));
  Type *VT = vecTy(FT, W / EltBits);
  Value *X = vcast(A, VT), *Y = vcast(Bv, VT), *Z = vcast(C, VT);
  // Op is the Intel mnemonic tail: madd|msub|nmadd|nmsub with the 132/213/231
  // operand permutation already resolved by the emitter into A, Bv, C.
  bool NegProduct = Op.starts_with("n");
  bool NegAddend = Op.contains("sub");
  if (NegProduct)
    Y = B.CreateFNeg(Y);
  if (NegAddend)
    Z = B.CreateFNeg(Z);
  Value *R = B.CreateIntrinsic(Intrinsic::fma, {VT}, {X, Y, Z});
  return avx512Select(K, B.CreateBitCast(R, iN(W)), Merge, W, EltBits);
}

Value *X86LiftContext::avx512FmaEmbedded(StringRef Op, Value *K, Value *A,
                                         Value *Bv, Value *C, Value *Merge,
                                         unsigned W, unsigned EltBits,
                                         uint64_t Rounding) {
  Type *FT = EltBits == 64 ? static_cast<Type *>(Type::getDoubleTy(Ctx))
                           : static_cast<Type *>(Type::getFloatTy(Ctx));
  Type *VT = vecTy(FT, W / EltBits);
  Value *X = vcast(A, VT), *Y = vcast(Bv, VT), *Z = vcast(C, VT);
  // Only 512-bit forms carry an embedded rounding immediate; the intrinsic
  // and its immediate match clang's _mm512_*_round_* lowering.
  StringRef Name;
  if (Op.contains("addsub"))
    Name = EltBits == 64 ? "x86.avx512.vfmaddsub.pd.512"
                         : "x86.avx512.vfmaddsub.ps.512";
  else if (Op.contains("subadd"))
    Name = EltBits == 64 ? "x86.avx512.vfmsubadd.pd.512"
                         : "x86.avx512.vfmsubadd.ps.512";
  else
    Name = EltBits == 64 ? "x86.avx512.vfmadd.pd.512"
                         : "x86.avx512.vfmadd.ps.512";
  Value *R = B.CreateIntrinsic(x86Intrinsic(Name), ArrayRef<Type *>(),
                               {X, Y, Z, constI(Rounding, 32)});
  return avx512Select(K, B.CreateBitCast(R, iN(W)), Merge, W, EltBits);
}

Value *X86LiftContext::avx512FmaVec(StringRef Kind, Value *A, Value *Bv,
                                    Value *C, unsigned W, unsigned EltBits,
                                    int64_t Rc) {
  Type *FT = EltBits == 64 ? static_cast<Type *>(Type::getDoubleTy(Ctx))
                           : static_cast<Type *>(Type::getFloatTy(Ctx));
  Type *VT = vecTy(FT, W / EltBits);
  Value *X = vcast(A, VT), *Y = vcast(Bv, VT), *Z = vcast(C, VT);
  if (Kind == "maddsub" || Kind == "msubadd") {
    // Alternating add/sub FMA: the same intrinsic clang emits for
    // _mm512_fmaddsub_* / _mm512_fmsubadd_*.
    StringRef Name =
        Kind == "maddsub"
            ? (EltBits == 64 ? "x86.avx512.vfmaddsub.pd.512"
                             : "x86.avx512.vfmaddsub.ps.512")
            : (EltBits == 64 ? "x86.avx512.vfmsubadd.pd.512"
                             : "x86.avx512.vfmsubadd.ps.512");
    return B.CreateIntrinsic(x86Intrinsic(Name), ArrayRef<Type *>(),
                             {X, Y, Z, constI(Rc < 0 ? 0 : Rc, 32)});
  }
  bool NegProduct = Kind.starts_with("n");
  bool NegAddend = Kind.contains("sub");
  if (NegProduct)
    Y = B.CreateFNeg(Y);
  if (NegAddend)
    Z = B.CreateFNeg(Z);
  if (Rc >= 0) {
    // Embedded rounding: llvm.x86.avx512.vfmadd.* with the rc immediate.
    StringRef Name = EltBits == 64 ? "x86.avx512.vfmadd.pd.512"
                                   : "x86.avx512.vfmadd.ps.512";
    return B.CreateIntrinsic(x86Intrinsic(Name), ArrayRef<Type *>(),
                             {X, Y, Z, constI(Rc, 32)});
  }
  return B.CreateIntrinsic(Intrinsic::fma, {VT}, {X, Y, Z});
}
