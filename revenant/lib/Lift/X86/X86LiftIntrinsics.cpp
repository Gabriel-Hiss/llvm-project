//===- X86LiftIntrinsics.cpp - helpers that call llvm.x86.* intrinsics ----===//
//
// Families whose exact semantics live in the backend's intrinsics (AES-NI,
// CLMUL, SHA, SSE4.2 string compares, masked loads/stores, RNG, MXCSR) are
// lifted as calls to the same llvm.x86.* intrinsic the compiler emits for
// the matching C intrinsic in source. Everything else here is small glue:
// scalar FMA over llvm.fma, scalar compares, rotates through CF, CPUID and
// ENTER.
//
//===---------------------------------------------------------------------===//

#include "revenant/Lift/X86LiftContext.h"
#include "X86MCTargetDesc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
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

// ---- masked moves ------------------------------------------------------------

Value *X86LiftContext::maskLoad(Value *Mask, Value *AddrI64, unsigned W,
                                unsigned EltBits) {
  unsigned N = W / EltBits;
  Type *IVT = vecTy(iN(EltBits), N);
  Value *Signs = B.CreateICmpSLT(vcast(Mask, IVT),
                                 Constant::getNullValue(IVT));
  Type *DataTy = vecTy(iN(8), W / 8);
  return B.CreateBitCast(
      B.CreateMaskedLoad(DataTy, B.CreateIntToPtr(AddrI64, ptrTy()),
                         Align(EltBits / 8), Signs, PoisonValue::get(DataTy)),
      iN(W));
}

void X86LiftContext::maskStore(Value *Mask, Value *V, Value *AddrI64,
                               unsigned W, unsigned EltBits) {
  unsigned N = W / EltBits;
  Type *IVT = vecTy(iN(EltBits), N);
  Value *Signs = B.CreateICmpSLT(vcast(Mask, IVT),
                                 Constant::getNullValue(IVT));
  B.CreateMaskedStore(vcast(V, vecTy(iN(8), W / 8)),
                      B.CreateIntToPtr(AddrI64, ptrTy()), Align(EltBits / 8),
                      Signs);
}

// ---- AES / carryless multiply ------------------------------------------------

Value *X86LiftContext::aesOp(StringRef Kind, Value *A, Value *Bv,
                             int64_t Imm) {
  unsigned W = A->getType()->getPrimitiveSizeInBits();

  Type *QVT = vecTy(iN(64), W / 64);
  Value *X = vcast(A, QVT), *Y = Bv ? vcast(Bv, QVT) : nullptr;
  StringRef N;
  if (Kind == "enc")
    N = W == 128 ? "x86.aesni.aesenc" : "x86.aesni.aesenc.256";
  else if (Kind == "enclast")
    N = W == 128 ? "x86.aesni.aesenclast" : "x86.aesni.aesenclast.256";
  else if (Kind == "dec")
    N = W == 128 ? "x86.aesni.aesdec" : "x86.aesni.aesdec.256";
  else if (Kind == "declast")
    N = W == 128 ? "x86.aesni.aesdeclast" : "x86.aesni.aesdeclast.256";
  else if (Kind == "imc")
    N = "x86.aesni.aesimc";
  else { // "keygen"
    Value *R = B.CreateIntrinsic(x86Intrinsic("x86.aesni.aeskeygenassist"),
                                 ArrayRef<Type *>(), {X, constI(Imm, 8)});
    return toInt(R, W);
  }
  if (Kind == "imc")
    return toInt(B.CreateIntrinsic(x86Intrinsic(N), ArrayRef<Type *>(), {X}),
                 W);
  return toInt(
      B.CreateIntrinsic(x86Intrinsic(N), ArrayRef<Type *>(), {X, Y}), W);
}

Value *X86LiftContext::crc32Op(Value *Acc, Value *Data, unsigned AccBits,
                               unsigned DataBits) {
  StringRef Name = AccBits == 64
                       ? "x86.sse42.crc32.64.64"
                       : DataBits == 8   ? "x86.sse42.crc32.32.8"
                         : DataBits == 16 ? "x86.sse42.crc32.32.16"
                         : DataBits == 32 ? "x86.sse42.crc32.32.32"
                                          : "";
  if (Name.empty())
    return PoisonValue::get(iN(AccBits));
  Function *Crc = Intrinsic::getOrInsertDeclaration(&module(),
                                                   x86Intrinsic(Name));
  return B.CreateCall(
      Crc, {vcast(Acc, iN(AccBits)), vcast(Data, iN(DataBits))});
}

Value *X86LiftContext::pclmulOp(Value *A, Value *Bv, int64_t Imm) {
  unsigned W = A->getType()->getPrimitiveSizeInBits();
  Type *QVT = vecTy(iN(64), W / 64);
  StringRef N = W == 128 ? "x86.pclmulqdq"
                : W == 256 ? "x86.pclmulqdq.256"
                           : "x86.pclmulqdq.512";
  return toInt(B.CreateIntrinsic(
                   x86Intrinsic(N), ArrayRef<Type *>(),
                   {vcast(A, QVT), vcast(Bv, QVT), constI(Imm, 8)}),
               W);
}

// ---- SHA-1 / SHA-256 -----------------------------------------------------------

Value *X86LiftContext::sha1Op(StringRef Kind, Value *A, Value *Bv,
                            int64_t Imm) {
  Type *DVT = vecTy(iN(32), 4);
  Value *X = vcast(A, DVT), *Y = Bv ? vcast(Bv, DVT) : nullptr;
  StringRef N = Kind == "rnds4"  ? "x86.sha1rnds4"
                : Kind == "nexte"  ? "x86.sha1nexte"
                : Kind == "msg1"   ? "x86.sha1msg1"
                                   : "x86.sha1msg2";
  if (Kind == "rnds4")
    return toInt(B.CreateIntrinsic(x86Intrinsic(N), {DVT},
                                   {X, Y, constI(Imm, 32)}),
                 128);
  return toInt(B.CreateIntrinsic(x86Intrinsic(N), {DVT}, {X, Y}), 128);
}

Value *X86LiftContext::sha256Op(StringRef Kind, Value *A, Value *Bv) {
  Type *DVT = vecTy(iN(32), 4);
  Value *X = vcast(A, DVT), *Y = vcast(Bv, DVT);
  if (Kind == "rnds2") {
    // SHA256RNDS2 uses XMM0 as the third operand implicitly.
    Value *X0 = vcast(regRead(X86::XMM0, iN(128)), DVT);
    return toInt(
        B.CreateIntrinsic(x86Intrinsic("x86.sha256rnds2"), {DVT},
                          {X, Y, X0}),
        128);
  }
  StringRef N = Kind == "msg1" ? "x86.sha256msg1" : "x86.sha256msg2";
  return toInt(B.CreateIntrinsic(x86Intrinsic(N), {DVT}, {X, Y}), 128);
}

// ---- scalar FMA -----------------------------------------------------------------

Value *X86LiftContext::fmaScalar(StringRef Kind, unsigned Form,
                                 Value *MergeOrMul1, Value *Mul2OrAdd,
                                 Value *AddOrMul2, bool NegMul, bool NegAdd) {
  // Kind "sd"|"ss"; Form 132|213|231 picks which source is the second
  // multiplicand and which is the addend, per Intel: dst = src1*X + Y.
  //   132: src1*src3 + src2   213: src1*src2 + src3   231: src1*src3 + src2
  // For 132/213 the operands arrive (dst-tied, src2, src3) in td order.
  Type *FT = Kind == "sd" ? static_cast<Type *>(Type::getDoubleTy(Ctx))
                          : static_cast<Type *>(Type::getFloatTy(Ctx));
  unsigned W = MergeOrMul1->getType()->getPrimitiveSizeInBits();
  auto Lane0 = [&](Value *V) {
    return B.CreateExtractElement(
        B.CreateBitCast(V, vecTy(FT, V->getType()->getPrimitiveSizeInBits() /
                                          FT->getPrimitiveSizeInBits())),
        uint64_t(0));
  };
  Value *S1 = Lane0(MergeOrMul1), *S2 = Lane0(Mul2OrAdd), *S3 = Lane0(AddOrMul2);
  Value *Mul2 = Form == 132 ? S3 : Form == 213 ? S2 : S3;
  Value *Addend = Form == 132 ? S2 : Form == 213 ? S3 : S2;
  if (NegMul)
    Mul2 = B.CreateFNeg(Mul2);
  if (NegAdd)
    Addend = B.CreateFNeg(Addend);
  Value *R = B.CreateIntrinsic(Intrinsic::fma, {FT}, {S1, Mul2, Addend});
  // Merge upper lanes from the first (tied) operand.
  Value *Vec = B.CreateBitCast(
      MergeOrMul1,
      vecTy(FT, W / FT->getPrimitiveSizeInBits()));
  return B.CreateBitCast(B.CreateInsertElement(Vec, R, uint64_t(0)), iN(W));
}

Value *X86LiftContext::fmaAddSubOp(Value *A, Value *Bv, Value *C, unsigned W,
                                   unsigned EltBits, bool Invert) {
  Type *FT = EltBits == 64 ? static_cast<Type *>(Type::getDoubleTy(Ctx))
                           : static_cast<Type *>(Type::getFloatTy(Ctx));
  Type *VT = vecTy(FT, W / EltBits);
  Value *X = vcast(A, VT), *Y = vcast(Bv, VT), *Z = vcast(C, VT);
  if (Invert)
    Z = B.CreateFNeg(Z);
  StringRef Name = EltBits == 64
                       ? W == 256 ? "x86.fma.vfmaddsub.pd.256"
                                  : "x86.fma.vfmaddsub.pd"
                       : W == 256 ? "x86.fma.vfmaddsub.ps.256"
                                  : "x86.fma.vfmaddsub.ps";
  Function *Fn = Intrinsic::getOrInsertDeclaration(&module(),
                                                   x86Intrinsic(Name));
  return toInt(B.CreateCall(Fn, {X, Y, Z}), W);
}

// ---- scalar compare ---------------------------------------------------------------

Value *X86LiftContext::cmpScalar(bool IsSD, Value *Merge, Value *A,
                                 Value *Bv, int64_t Imm) {
  Type *FT = IsSD ? static_cast<Type *>(Type::getDoubleTy(Ctx))
                  : static_cast<Type *>(Type::getFloatTy(Ctx));
  Type *VT = vecTy(FT, 16 / (FT->getPrimitiveSizeInBits() / 8));
  Value *X = vcast(A, VT), *Y = vcast(Bv, VT);
  Value *R = B.CreateIntrinsic(
      x86Intrinsic(IsSD ? "x86.sse2.cmp.sd" : "x86.sse.cmp.ss"), {VT},
      {X, Y, constI(Imm, 8)});
  // Lane 0 = mask, upper lanes from operand A (both SSE and VEX forms).
  return B.CreateBitCast(B.CreateInsertElement(vcast(Merge, VT),
                                               B.CreateExtractElement(R, 0ull),
                                               uint64_t(0)),
                         iN(128));
}

// ---- PCMPxSTRx ---------------------------------------------------------------------

void X86LiftContext::pcmpXstrOp(StringRef Kind, Value *A, Value *Bv,
                                int64_t Imm) {
  // Kind "estri"|"estrm"|"istri"|"istrm". A = $src1, Bv = $src3/$src2,
  // Imm = u8imm control. The *m128 intrinsics hand back IntRes2 as a byte
  // mask; bit-casting it to i16 gives IntRes2, so the index form derives its
  // result and every flag from the same value the hardware computes.
  Type *BVT = vecTy(iN(8), 16);
  Value *X = vcast(A, BVT), *Y = vcast(Bv, BVT);
  bool Explicit = Kind[0] == 'e';
  bool Index = Kind.ends_with("i");
  Value *Res2;
  if (Explicit) {
    Value *La = toInt(regRead(X86::EAX, iN(32)), 32);
    Value *Lb = toInt(regRead(X86::EDX, iN(32)), 32);
    Function *Fn = Intrinsic::getOrInsertDeclaration(
        &module(), x86Intrinsic("x86.sse42.pcmpestrm128"));
    Res2 = B.CreateCall(Fn, {X, La, Y, Lb, constI(Imm, 8)});
  } else {
    Function *Fn = Intrinsic::getOrInsertDeclaration(
        &module(), x86Intrinsic("x86.sse42.pcmpistrm128"));
    Res2 = B.CreateCall(Fn, {X, Y, constI(Imm, 8)});
  }
  Value *MaskInt = B.CreateBitCast(Res2, iN(128));
  Value *Bits = B.CreateTrunc(MaskInt, iN(16));
  Value *Zero16 = constI(0, 16);
  flagWrite(CF, B.CreateICmpNE(Bits, Zero16));
  flagWrite(OF, B.CreateTrunc(Bits, iN(1)));

  auto StrLen = [&](Value *Vec) -> Value * {
    // Length = index of the first 0 byte, or 16 when none.
    Value *Eq0 = B.CreateICmpEQ(Vec, Constant::getNullValue(BVT));
    Value *M = B.CreateBitCast(Eq0, iN(16));
    return B.CreateSelect(B.CreateICmpEQ(M, Zero16), constI(16, 32),
                          B.CreateIntrinsic(Intrinsic::cttz, {iN(32)},
                                            {B.CreateZExt(M, iN(32)),
                                             B.getFalse()}));
  };
  Value *La = Explicit ? toInt(regRead(X86::EAX, iN(32)), 32) : StrLen(X);
  Value *Lb = Explicit ? toInt(regRead(X86::EDX, iN(32)), 32) : StrLen(Y);
  flagWrite(SF, B.CreateICmpULT(La, constI(16, 32)));
  flagWrite(ZF, B.CreateICmpULT(Lb, constI(16, 32)));
  flagWrite(AF, B.getFalse());

  if (Index) {
    flagWrite(PF, B.getFalse());
    Value *Ix;
    if (Imm & 0x40) {
      // Most significant set bit; 16 when the mask is empty (ctlz at 0 is
      // undef, so the empty case is selected explicitly).
      Value *Clz = B.CreateIntrinsic(Intrinsic::ctlz, {iN(16)},
                                     {Bits, B.getFalse()});
      Ix = B.CreateSelect(B.CreateICmpEQ(Bits, Zero16), constI(16, 16),
                          B.CreateSub(constI(15, 16), Clz));
    } else {
      Ix = B.CreateIntrinsic(Intrinsic::cttz, {iN(16)}, {Bits, B.getFalse()});
    }
    regWrite(X86::ECX, B.CreateZExt(Ix, iN(32)));
  } else {
    flagWrite(PF, parityOf(Bits));
    // Imm[6] selects the unit-mask form. Otherwise return the low-bit mask.
    if (Imm & 0x40)
      regWrite(X86::XMM0, Res2);
    else
      regWrite(X86::XMM0, B.CreateZExt(Bits, iN(128)));
  }
}

// ---- RCL / RCR through the carry flag ----------------------------------------------

Value *X86LiftContext::rotCarryOp(bool IsLeft, Value *V, Value *Count) {
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *C = zextOrTrunc(Count, iN(64));
  // COUNT = (COUNT & 0x1F/0x3F) % (W + 1); 8/16-bit also mask to 0x1F.
  Value *Mask = constI(W == 64 ? 0x3f : 0x1f, 64);
  Value *N = B.CreateURem(B.CreateAnd(C, Mask), constI(W + 1, 64));
  // W+1-bit rotate of (CF : V).
  Value *Wide = B.CreateOr(
      B.CreateZExt(V, iN(W + 1)),
      B.CreateShl(B.CreateZExt(flagRead(CF), iN(W + 1)), constI(W, W + 1)));
  Value *NZ = B.CreateTrunc(N, iN(W + 1));
  Value *Rot = B.CreateIntrinsic(IsLeft ? Intrinsic::fshl : Intrinsic::fshr,
                                 {iN(W + 1)}, {Wide, Wide, NZ});
  Value *R = B.CreateTrunc(Rot, iN(W));
  // CF = the bit that leaves the W+1 ring. For RCL that is input bit
  // (W+1-N) mod (W+1) extracted from the pre-rotate value (bit W is the
  // incoming CF, which is also the N==0 answer). For RCR the dropped bit
  // lands at bit 0 of the rotated result.
  Value *CfOut;
  if (IsLeft) {
    Value *Idx = B.CreateURem(B.CreateSub(constI(W + 1, 64), N),
                              constI(W + 1, 64));
    Value *Shl = B.CreateShl(Wide, B.CreateTrunc(Idx, iN(W + 1)));
    CfOut = B.CreateTrunc(B.CreateLShr(Shl, constI(W, W + 1)), iN(1));
  } else {
    CfOut = B.CreateTrunc(Rot, iN(1));
  }
  flagWrite(CF, CfOut);
  // OF is defined only for count 1; preserve it otherwise.
  Value *IsOne = B.CreateICmpEQ(N, constI(1, 64));
  auto BitOf = [&](Value *X, uint64_t Bn) {
    return B.CreateTrunc(B.CreateLShr(X, constI(Bn, W)), iN(1));
  };
  Value *OfVal = IsLeft ? B.CreateXor(BitOf(R, W - 1), CfOut)
                        : B.CreateXor(BitOf(R, W - 1), BitOf(R, W - 2));
  flagWrite(OF, B.CreateSelect(IsOne, OfVal, flagRead(OF)));
  return R;
}

// ---- RNG / system -------------------------------------------------------------

Value *X86LiftContext::rngOp(StringRef Name, unsigned W) {
  std::string N = (Twine("x86.") + Name + "." + Twine(W)).str();
  Value *R = B.CreateIntrinsic(x86Intrinsic(N), ArrayRef<Type *>(),
                               ArrayRef<Value *>());
  // {iW value, i32 success} -> dst = value, CF = success; other flags clear.
  Value *Val = B.CreateExtractValue(R, 0);
  Value *Ok = B.CreateICmpNE(B.CreateExtractValue(R, 1), constI(0, 32));
  flagWrite(CF, Ok);
  for (FlagId F : {ZF, SF, OF, PF, AF})
    flagWrite(F, B.getFalse());
  return Val;
}

void X86LiftContext::cpuidOp() {
  // eax/ecx in; eax..edx out, like the source-level __cpuid builtin.
  FunctionType *FT = FunctionType::get(
      StructType::get(Ctx, {iN(32), iN(32), iN(32), iN(32)}),
      {iN(32), iN(32), iN(32), iN(32)}, false);
  InlineAsm *IA = InlineAsm::get(
      FT, "cpuid", "={ax},={bx},={cx},={dx},0,1,2,3",
      /*hasSideEffects=*/true);
  Value *Ax = regRead(X86::EAX, iN(32)), *Bx = regRead(X86::EBX, iN(32));
  Value *Cx = regRead(X86::ECX, iN(32)), *Dx = regRead(X86::EDX, iN(32));
  CallInst *Call = B.CreateCall(IA, {Ax, Bx, Cx, Dx});
  regWrite(X86::EAX, B.CreateExtractValue(Call, 0));
  regWrite(X86::EBX, B.CreateExtractValue(Call, 1));
  regWrite(X86::ECX, B.CreateExtractValue(Call, 2));
  regWrite(X86::EDX, B.CreateExtractValue(Call, 3));
}

void X86LiftContext::xgetbvOp() {
  // XCR0 on every 64-bit host OS; the operand is ECX.
  Value *Idx = regRead(X86::ECX, iN(32));
  Value *R = B.CreateIntrinsic(x86Intrinsic("x86.xgetbv"), {}, {Idx});
  regWrite(X86::EAX, B.CreateTrunc(R, iN(32)));
  regWrite(X86::EDX, B.CreateTrunc(B.CreateLShr(R, constI(32, 64)), iN(32)));
}

void X86LiftContext::ldmxcsrOp(const MCInst &MI, unsigned MIOpNo) {
  B.CreateIntrinsic(x86Intrinsic("x86.sse.ldmxcsr"), {},
                    {B.CreateIntToPtr(memAddr(MI, MIOpNo), ptrTy())});
}

void X86LiftContext::stmxcsrOp(const MCInst &MI, unsigned MIOpNo) {
  B.CreateIntrinsic(x86Intrinsic("x86.sse.stmxcsr"), {},
                    {B.CreateIntToPtr(memAddr(MI, MIOpNo), ptrTy())});
}

void X86LiftContext::pauseOp() {
  B.CreateIntrinsic(x86Intrinsic("x86.sse2.pause"), ArrayRef<Type *>(),
                    ArrayRef<Value *>());
}

// ---- ENTER -----------------------------------------------------------------------

void X86LiftContext::enterOp(uint64_t Frame, uint64_t Level) {
  // Push rbp, then copy up to 31 enclosing frame pointers, then reserve the
  // locals. Level 0 (the compiler-emitted form) skips the copy loop.
  Value *Rsp = regRead(X86::RSP, iN(64));
  pushVal(regRead(X86::RBP, iN(64)));
  Value *Temp = regRead(X86::RSP, iN(64));
  for (uint64_t I = 1; I < Level; ++I) {
    Value *Bp = B.CreateSub(regRead(X86::RBP, iN(64)), constI(8 * I, 64));
    pushVal(memReadAddr(Bp, iN(64)));
  }
  if (Level)
    pushVal(Temp);
  regWrite(X86::RBP, Temp);
  regWrite(X86::RSP,
           B.CreateSub(regRead(X86::RSP, iN(64)), constI(Frame, 64)));
}
