//===- X86LiftX87Env.cpp - x87 environment, save/restore and BCD ----------===//
//
// The x87 environment (FNSTENV/FSTENV/FLDENV), the full FPU image
// (FSAVE/FNSAVE/FRSTOR) and the packed-BCD conversions (FBLD/FBSTP) have no
// single IR instruction, but they are not opaque either: they are a defined
// byte-level view of state the context already models.
//
//   - The 28-byte environment image holds the control word at offset 0 and
//     the status word at offset 4 (the 32-bit protected-mode layout the
//     hardware uses in 64-bit mode). Both are modeled (X87CW, the packed
//     status word), so FNSTENV/FLDENV round-trip them. The tag, instruction
//     pointer and operand-pointer fields are not modeled and are left as the
//     caller's memory on store.
//   - FSAVE/FNSAVE write the environment followed by the eight 10-byte ST(i)
//     registers; FRSTOR reads them back. The tag word is not modeled, so the
//     registers are read/written by physical index and TOP comes from the
//     status word.
//   - FXSAVE/FXRSTOR go through the backend's llvm.x86.fxsave/fxrstor
//     intrinsics: they are the same calls clang emits for the
//     __builtin_ia32_fxsave*/fxrstor* builtins and carry the full 512-byte
//     read/write memory effect.
//   - FBLD/FBSTP convert between the 10-byte packed-BCD integer and
//     x86_fp80 with the obvious digit loop; FBSTP rounds to nearest and
//     pushes/pops the stack like the hardware.
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

// ---- 28-byte environment image ---------------------------------------------

void X86LiftContext::x87Env(StringRef Dir, Value *Addr) {
  // 32-bit protected-mode layout (what 64-bit mode always uses): control
  // word at offset 0, status word at offset 4.
  if (Dir == "st") {
    memWriteAddr(Addr, x87ControlWord());
    memWriteAddr(B.CreateAdd(Addr, constI(4, 64)), x87StatusWord());
  } else {
    x87SetControlWord(memReadAddr(Addr, iN(16)));
    x87SetStatusWord(memReadAddr(B.CreateAdd(Addr, constI(4, 64)), iN(16)));
  }
}

// ---- 108-byte FPU image ----------------------------------------------------

void X86LiftContext::x87SaveRestore(StringRef Dir, Value *Addr) {
  ensureX87();
  x87Env(Dir, Addr);
  Value *Base = B.CreateAdd(Addr, constI(28, 64));
  for (unsigned I = 0; I < 8; ++I) {
    Value *Slot = B.CreateAdd(Base, constI(10 * I, 64));
    // The image holds ST(0)..ST(7) in stack order (slot i is the physical
    // register (TOP+i)&7), the same order FSAVE/FNSAVE write and FRSTOR
    // reads. TOP is in the status word written/read above, so x87St resolves
    // the physical index on both sides.
    if (Dir == "st")
      memWriteAddr(Slot, x87St(I));
    else
      x87StSet(I, memReadAddr(Slot, iN(80)));
  }
  if (Dir == "st") {
    // FSAVE/FNSAVE leave the stack empty with TOP = 0.
    B.CreateStore(constI(0, 8), X87Top);
    x87SetControlWord(constI(0x037f, 16));
    x87SetStatusWord(constI(0, 16));
  }
}

// ---- FXSAVE / FXRSTOR ------------------------------------------------------

void X86LiftContext::fxsaveOp(Value *Addr) {
  Function *Fn = Intrinsic::getOrInsertDeclaration(&module(),
                                                   x86Intrinsic("x86.fxsave"));
  B.CreateCall(Fn, {B.CreateIntToPtr(Addr, ptrTy())});
}

void X86LiftContext::fxrstorOp(Value *Addr) {
  Function *Fn = Intrinsic::getOrInsertDeclaration(&module(),
                                                   x86Intrinsic("x86.fxrstor"));
  B.CreateCall(Fn, {B.CreateIntToPtr(Addr, ptrTy())});
}

// ---- packed BCD ------------------------------------------------------------

void X86LiftContext::x87BcdLoad(Value *Addr) {
  // 10 bytes: 18 BCD digits (two per byte, low nibble first) plus a sign byte
  // whose top bit is the sign. value = sign * sum(digit_i * 10^i).
  Type *F80 = Type::getX86_FP80Ty(Ctx);
  Value *Bits = memReadAddr(Addr, iN(80));
  Value *Acc = ConstantFP::get(F80, 0.0);
  Value *Ten = ConstantFP::get(F80, 10.0);
  for (unsigned I = 0; I < 18; ++I) {
    Value *Shift = B.CreateLShr(Bits, constI(4 * I, 80));
    Value *Nib = B.CreateAnd(Shift, constI(0xF, 80));
    Value *Digit = B.CreateUIToFP(B.CreateTrunc(Nib, iN(8)), F80);
    Acc = B.CreateFAdd(B.CreateFMul(Acc, Ten), Digit);
  }
  // Sign: bit 7 of byte 9 is bit 79 of the little-endian 80-bit image.
  Value *Neg = B.CreateICmpNE(B.CreateAnd(Bits, constI(1ull << 63, 80)),
                              constI(0, 80));
  x87Push(B.CreateSelect(Neg, B.CreateFNeg(Acc), Acc));
}

void X86LiftContext::x87BcdStore(Value *Addr) {
  // FBSTP rounds ST(0) to nearest (ties to even), converts the absolute value
  // to 18 BCD digits, sets the sign byte, stores 10 bytes and pops the stack.
  // The value range of a BCD integer fits i64, so the round is done on the
  // truncated integer with an explicit tie-to-even step (llvm.rint.f80 would
  // lower to a libcall the JIT cannot resolve).
  Type *F80 = Type::getX86_FP80Ty(Ctx);
  Value *V = x87Pop();
  Value *T = B.CreateFPToSI(V, iN(64)); // truncate toward zero
  Value *Frac = B.CreateFSub(V, B.CreateSIToFP(T, F80));
  Value *Half = ConstantFP::get(F80, 0.5);
  Value *AbsFrac =
      B.CreateIntrinsic(Intrinsic::fabs, {F80}, {Frac});
  Value *Neg = B.CreateFCmpOLT(V, ConstantFP::get(F80, 0.0));
  Value *Odd = B.CreateICmpNE(B.CreateAnd(T, constI(1, 64)), constI(0, 64));
  // Round up in magnitude when |frac| > 0.5, or == 0.5 and the integer is odd.
  Value *Round = B.CreateOr(
      B.CreateFCmpOGT(AbsFrac, Half),
      B.CreateAnd(B.CreateFCmpOEQ(AbsFrac, Half), Odd));
  Value *Step = B.CreateSelect(Neg, constI(-1, 64), constI(1, 64));
  Value *I = B.CreateAdd(T, B.CreateSelect(Round, Step, constI(0, 64)));
  Value *INeg = B.CreateICmpSLT(I, constI(0, 64));
  Value *Abs = B.CreateSelect(INeg, B.CreateSub(constI(0, 64), I), I);
  // 18 digits = 72 bits; accumulate in an i128 so no digit is lost.
  Value *Acc = constI(0, 128);
  Value *Cur = B.CreateZExt(Abs, iN(128));
  for (unsigned J = 0; J < 18; ++J) {
    Value *Digit = B.CreateURem(Cur, constI(10, 128));
    Cur = B.CreateUDiv(Cur, constI(10, 128));
    Acc = B.CreateOr(Acc, B.CreateShl(Digit, constI(4 * J, 128)));
  }
  Value *Low = B.CreateTrunc(Acc, iN(80));
  // Byte 9 is the sign byte; its high bit is bit 79 of the 80-bit image.
  Value *SignBit = B.CreateShl(B.CreateZExt(INeg, iN(80)), constI(79, 80));
  memWriteAddr(Addr, B.CreateOr(Low, SignBit));
}
