//===- X86LiftContext.cpp - x86-64 lift state -----------------------------===//

#include "revenant/Lift/X86LiftContext.h"
#include "X86MCTargetDesc.h"
#include "X86BaseInfo.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace llvm::revenant;

namespace {

// Flag slot names in FlagId order (CF..DF).
const char *FlagNames[] = {"cf", "zf", "sf", "of", "pf", "af", "df"};

} // namespace

X86LiftContext::X86LiftContext(LLVMContext &C, Module &M, Function &F,
                               const MCInstrInfo &MCII,
                               const MCRegisterInfo &TRI)
    : LiftContext(C, M, F), MCII(MCII), TRI(TRI) {}

// ---- registers -------------------------------------------------------------

MCRegister X86LiftContext::rootReg(MCRegister Reg) const {
  MCRegister Root = Reg;
  for (MCSuperRegIterator SI(Reg, &TRI); SI.isValid(); ++SI)
    Root = *SI;
  return Root;
}

unsigned X86LiftContext::regBits(MCRegister Reg) const {
  unsigned Bits = 0;
  for (const MCRegisterClass &RC : TRI.regclasses()) {
    if (!RC.contains(Reg))
      continue;
    unsigned ClassBits = RC.getSizeInBits();
    if (ClassBits && (!Bits || ClassBits < Bits))
      Bits = ClassBits;
  }
  return Bits ? Bits : 64;
}

std::pair<unsigned, unsigned>
X86LiftContext::regOffsetBits(MCRegister Reg) const {
  unsigned Offset = 0;
  StringRef Name(TRI.getName(Reg));
  if (Name == "AH" || Name == "BH" || Name == "CH" || Name == "DH")
    Offset = 8;
  return {Offset, regBits(Reg)};
}

AllocaInst *X86LiftContext::slotFor(MCRegister Reg) {
  MCRegister Root = rootReg(Reg);
  auto It = Slots.find(Root);
  if (It != Slots.end())
    return It->second;
  unsigned Bits = regBits(Root);
  AllocaInst *A = regSlot(StringRef(TRI.getName(Root)).lower(), iN(Bits));
  Slots[Root] = A;
  SlotBits[Root] = Bits;
  return A;
}

Value *X86LiftContext::toInt(Value *V, unsigned W) {
  Type *T = V->getType();
  if (T->isIntegerTy(W))
    return V;
  if (T->isIntegerTy())
    return B.CreateZExtOrTrunc(V, iN(W));
  if (T->isPointerTy())
    return B.CreatePtrToInt(V, iN(W));
  unsigned SrcW = T->getPrimitiveSizeInBits();
  Value *I = B.CreateBitCast(V, iN(SrcW));
  return B.CreateZExtOrTrunc(I, iN(W));
}

Value *X86LiftContext::fromInt(Value *V, Type *Ty) {
  if (Ty->isIntegerTy())
    return B.CreateZExtOrTrunc(V, Ty);
  if (Ty->isPointerTy())
    return B.CreateIntToPtr(zextOrTrunc(V, iN(64)), Ty);
  if (Ty->isX86_FP80Ty())
    return B.CreateBitCast(zextOrTrunc(V, iN(80)), Ty);
  unsigned DstW = Ty->getPrimitiveSizeInBits();
  return B.CreateBitCast(zextOrTrunc(V, iN(DstW)), Ty);
}

Value *X86LiftContext::regBitsRead(MCRegister Reg) {
  AllocaInst *S = slotFor(Reg);
  unsigned RootW = SlotBits[rootReg(Reg)];
  auto [Off, W] = regOffsetBits(Reg);
  Value *All = B.CreateLoad(iN(RootW), S);
  if (Off == 0 && W == RootW)
    return All;
  return B.CreateTrunc(B.CreateLShr(All, constI(Off, RootW)), iN(W));
}

void X86LiftContext::regBitsWrite(MCRegister Reg, Value *V) {
  MCRegister Root = rootReg(Reg);
  AllocaInst *S = slotFor(Reg);
  unsigned RootW = SlotBits[Root];
  auto [Off, W] = regOffsetBits(Reg);
  Value *I = toInt(V, W);
  if (W == 32 && RootW == 64 && Off == 0) {
    // 32-bit GPR writes zero the upper half of the 64-bit register.
    B.CreateStore(B.CreateZExt(I, iN(64)), S);
    return;
  }
  if (W == RootW) {
    B.CreateStore(I, S);
    return;
  }
  // Merge at the bit offset.
  Value *Old = B.CreateLoad(iN(RootW), S);
  APInt Mask = APInt::getBitsSet(RootW, Off, Off + W);
  Value *Kept = B.CreateAnd(Old, ConstantInt::get(iN(RootW), ~Mask));
  Value *Placed = B.CreateShl(B.CreateZExt(I, iN(RootW)), constI(Off, RootW));
  B.CreateStore(B.CreateOr(Kept, Placed), S);
}

Value *X86LiftContext::regRead(MCRegister Reg, Type *Ty) {
  if (Reg == X86::RIP || Reg == X86::EIP)
    return fromInt(resolveVAI64(NextVA), Ty);
  return fromInt(regBitsRead(Reg), Ty);
}

void X86LiftContext::regWrite(MCRegister Reg, Value *V) {
  if (Reg == X86::RIP || Reg == X86::EIP)
    return;
  regBitsWrite(Reg, V);
}

// ---- operands --------------------------------------------------------------

LiftContext::OpKind X86LiftContext::opKind(const MCInst &MI,
                                           unsigned MIOpNo) const {
  const MCInstrDesc &Desc = MCII.get(MI.getOpcode());
  if (MIOpNo >= Desc.NumOperands || MIOpNo >= MI.getNumOperands())
    return OpKind::Invalid;
  uint8_t OT = Desc.operands()[MIOpNo].OperandType;
  switch (OT) {
  case MCOI::OPERAND_REGISTER:
    return OpKind::Reg;
  case MCOI::OPERAND_MEMORY:
    return OpKind::Mem;
  default:
    // Everything else (OPERAND_IMMEDIATE, OPERAND_PCREL, and the X86
    // target types like OPERAND_COND_CODE) reads as an immediate.
    return OpKind::Imm;
  }
}

Value *X86LiftContext::memAddr(const MCInst &MI, unsigned MIOpNo) {
  // moffs (absolute) forms decode to a single immediate operand instead of
  // the five-part group.
  if (opKind(MI, MIOpNo) == OpKind::Imm)
    return resolveVAI64(static_cast<uint64_t>(MI.getOperand(MIOpNo).getImm()));

  const MCOperand &Base = MI.getOperand(MIOpNo);
  const MCOperand &Scale = MI.getOperand(MIOpNo + 1);
  const MCOperand &Index = MI.getOperand(MIOpNo + 2);
  const MCOperand &Disp = MI.getOperand(MIOpNo + 3);
  const MCOperand &Seg = MI.getOperand(MIOpNo + 4);

  Value *Addr = nullptr;
  if (Base.isReg() && Base.getReg() == X86::RIP) {
    Addr = constI(NextVA + Disp.getImm(), 64);
  } else {
    if (Base.isReg() && Base.getReg().isValid())
      Addr = regRead(Base.getReg(), iN(64));
    if (Index.isReg() && Index.getReg().isValid()) {
      Value *Idx = regRead(Index.getReg(), iN(64));
      if (Scale.getImm() != 1)
        Idx = B.CreateMul(Idx, constI(Scale.getImm(), 64));
      Addr = Addr ? B.CreateAdd(Addr, Idx) : Idx;
    }
    int64_t D = Disp.isImm() ? Disp.getImm() : 0;
    if (D != 0 || !Addr)
      Addr = Addr ? B.CreateAdd(Addr, constI(D, 64)) : constI(D, 64);
  }

  // Non-default segment: route through a segment-base helper so TLS and
  // fs/gs accesses stay representable instead of silently wrong. SS and DS
  // are the implicit defaults on 64-bit and mean +0.
  MCRegister SegReg = PendingSeg;
  if (Seg.isReg() && Seg.getReg().isValid())
    SegReg = Seg.getReg();
  if (SegReg.isValid() && SegReg != X86::SS && SegReg != X86::DS &&
      SegReg != X86::ES && SegReg != X86::CS) {
    std::string Name = StringRef(TRI.getName(SegReg)).lower();
    FunctionCallee F = Mod.getOrInsertFunction(
        "revenant.segbase." + Name, FunctionType::get(iN(64), false));
    Addr = B.CreateAdd(Addr, B.CreateCall(F));
  }
  return Addr;
}

// ---- flags -------------------------------------------------------------------

Value *X86LiftContext::flagRead(FlagId F) {
  if (!Flags[F])
    Flags[F] = regSlot(FlagNames[F], iN(1));
  return B.CreateLoad(iN(1), Flags[F]);
}

void X86LiftContext::flagWrite(FlagId F, Value *I1) {
  if (!Flags[F])
    Flags[F] = regSlot(FlagNames[F], iN(1));
  B.CreateStore(B.CreateZExtOrTrunc(I1, iN(1)), Flags[F]);
}

Value *X86LiftContext::eflagsIn() { return UndefValue::get(iN(32)); }

Value *X86LiftContext::parityOf(Value *V) {
  Value *Lo = B.CreateTrunc(V, iN(8));
  Value *Ct = B.CreateIntrinsic(Intrinsic::ctpop, {iN(8)}, {Lo});
  return B.CreateNot(B.CreateTrunc(Ct, iN(1)));
}

void X86LiftContext::flagsZSP(Value *R) {
  unsigned W = R->getType()->getIntegerBitWidth();
  flagWrite(ZF, B.CreateICmpEQ(R, constI(0, W)));
  flagWrite(SF, B.CreateICmpSLT(R, constI(0, W)));
  flagWrite(PF, parityOf(R));
}

static Value *lowBit(X86LiftContext::BuilderTy &B, Value *V, unsigned Bit) {
  unsigned W = V->getType()->getIntegerBitWidth();
  if (Bit >= W)
    return B.getFalse();
  Value *Masked =
      B.CreateAnd(V, ConstantInt::get(V->getType(), APInt::getOneBitSet(W, Bit)));
  return B.CreateICmpNE(Masked, ConstantInt::get(V->getType(), 0));
}

Value *X86LiftContext::flagsAdd(Value *A, Value *B_) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *R = B.CreateAdd(A, B2);
  flagsZSP(R);
  flagWrite(AF, lowBit(B, B.CreateXor(B.CreateXor(A, B2), R), 4));
  flagWrite(CF, B.CreateICmpULT(R, A));
  flagWrite(OF, lowBit(B, B.CreateAnd(B.CreateNot(B.CreateXor(A, B2)),
                                      B.CreateXor(A, R)),
                       W - 1));
  return R;
}

Value *X86LiftContext::flagsSub(Value *A, Value *B_) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *R = B.CreateSub(A, B2);
  flagsZSP(R);
  flagWrite(AF, lowBit(B, B.CreateXor(B.CreateXor(A, B2), R), 4));
  flagWrite(CF, B.CreateICmpULT(A, B2));
  flagWrite(OF, lowBit(B, B.CreateAnd(B.CreateXor(A, B2), B.CreateXor(A, R)),
                       W - 1));
  return R;
}

Value *X86LiftContext::flagsLogic(Value *R) {
  flagsZSP(R);
  flagWrite(CF, B.getFalse());
  flagWrite(OF, B.getFalse());
  flagWrite(AF, UndefValue::get(iN(1)));
  return R;
}

Value *X86LiftContext::flagsAdc(Value *A, Value *B_) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *C = B.CreateZExt(flagRead(CF), A->getType());
  Value *Sum = B.CreateAdd(A, B2);
  Value *R = B.CreateAdd(Sum, C);
  flagsZSP(R);
  flagWrite(AF, lowBit(B, B.CreateXor(B.CreateXor(A, B2), R), 4));
  flagWrite(CF, B.CreateOr(B.CreateICmpULT(Sum, A), B.CreateICmpULT(R, Sum)));
  flagWrite(OF, lowBit(B, B.CreateAnd(B.CreateNot(B.CreateXor(A, B2)),
                                      B.CreateXor(A, R)),
                       W - 1));
  return R;
}

Value *X86LiftContext::flagsSbb(Value *A, Value *B_) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *C = B.CreateZExt(flagRead(CF), A->getType());
  Value *Diff = B.CreateSub(A, B2);
  Value *R = B.CreateSub(Diff, C);
  flagsZSP(R);
  flagWrite(AF, lowBit(B, B.CreateXor(B.CreateXor(A, B2), R), 4));
  flagWrite(CF, B.CreateOr(B.CreateICmpULT(A, B2), B.CreateICmpULT(Diff, C)));
  flagWrite(OF, lowBit(B, B.CreateAnd(B.CreateXor(A, B2), B.CreateXor(A, R)),
                       W - 1));
  return R;
}

Value *X86LiftContext::flagsNeg(Value *A) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *R = B.CreateNeg(A);
  flagsZSP(R);
  flagWrite(AF, lowBit(B, B.CreateOr(A, R), 4));
  flagWrite(CF, B.CreateICmpNE(A, constI(0, W)));
  flagWrite(OF, B.CreateICmpEQ(
                    A, ConstantInt::get(A->getType(),
                                        APInt::getSignedMinValue(W))));
  return R;
}

Value *X86LiftContext::flagsInc(Value *A) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *R = B.CreateAdd(A, constI(1, W));
  flagsZSP(R);
  flagWrite(AF, B.CreateICmpEQ(B.CreateAnd(A, constI(0xF, W)),
                             constI(0xF, W)));
  flagWrite(OF, lowBit(B, B.CreateAnd(B.CreateNot(A), R), W - 1));
  return R;
}

Value *X86LiftContext::flagsDec(Value *A) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *R = B.CreateSub(A, constI(1, W));
  flagsZSP(R);
  flagWrite(AF, B.CreateICmpEQ(B.CreateAnd(A, constI(0xF, W)),
                             constI(0, W)));
  flagWrite(OF, lowBit(B, B.CreateAnd(A, B.CreateNot(R)), W - 1));
  return R;
}

Value *X86LiftContext::flagsCmp(Value *A, Value *B_) {
  return flagsSub(A, B_);
}

Value *X86LiftContext::flagsSMul(Value *A, Value *B_) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *B2 = sextOrTrunc(B_, A->getType());
  Value *Wide = B.CreateMul(B.CreateSExt(A, iN(2 * W)),
                            B.CreateSExt(B2, iN(2 * W)));
  Value *Lo = B.CreateTrunc(Wide, iN(W));
  Value *Hi = B.CreateTrunc(B.CreateLShr(Wide, constI(W, 2 * W)), iN(W));
  Value *O = B.CreateICmpNE(Hi, B.CreateAShr(Lo, constI(W - 1, W)));
  flagWrite(CF, O);
  flagWrite(OF, O);
  for (FlagId F : {SF, ZF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
  return Lo;
}

Value *X86LiftContext::flagsUMul(Value *A, Value *B_) {
  unsigned W = A->getType()->getIntegerBitWidth();
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *Wide = B.CreateMul(B.CreateZExt(A, iN(2 * W)),
                            B.CreateZExt(B2, iN(2 * W)));
  Value *Lo = B.CreateTrunc(Wide, iN(W));
  Value *Hi = B.CreateTrunc(B.CreateLShr(Wide, constI(W, 2 * W)), iN(W));
  Value *O = B.CreateICmpNE(Hi, constI(0, W));
  flagWrite(CF, O);
  flagWrite(OF, O);
  for (FlagId F : {SF, ZF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
  return Lo;
}

// ---- shifts ------------------------------------------------------------------

Value *X86LiftContext::flagsShift(StringRef Name, Value *V, Value *Cnt) {
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *C = B.CreateZExtOrTrunc(Cnt, iN(64));
  // Hardware masks the count to 5 bits for 8/16/32-bit ops, 6 bits for
  // 64-bit ops.
  unsigned Mask = (W == 64) ? 63 : 31;
  Value *C8 = B.CreateAnd(C, constI(Mask, 64));
  Value *CW = B.CreateTrunc(C8, V->getType());
  Value *Is0 = B.CreateICmpEQ(C8, constI(0, 64));

  Value *Res;
  if (Name == "shl")
    Res = B.CreateShl(V, CW);
  else if (Name == "shr")
    Res = B.CreateLShr(V, CW);
  else if (Name == "sar")
    Res = B.CreateAShr(V, CW);
  else {
    Value *Ct = B.CreateTrunc(B.CreateURem(C8, constI(W, 64)), V->getType());
    Res = B.CreateIntrinsic(
        Name == "rol" ? Intrinsic::fshl : Intrinsic::fshr, {V->getType()},
        {V, V, Ct});
  }
  // Shift counts >= W give 0 (or all-sign for sar); rotates wrap.
  if (Name == "shl" || Name == "shr" || Name == "sar") {
    Value *TooBig = B.CreateICmpUGE(C8, constI(W, 64));
    Value *Sat = (Name == "sar") ? B.CreateAShr(V, constI(W - 1, W))
                                 : constI(0, W);
    Res = B.CreateSelect(TooBig, Sat, Res);
  }
  Res = B.CreateSelect(Is0, V, Res);

  Value *Zero = constI(0, W);
  flagWrite(ZF, B.CreateSelect(Is0, flagRead(ZF), B.CreateICmpEQ(Res, Zero)));
  if (Name != "rol" && Name != "ror") {
    flagWrite(SF,
              B.CreateSelect(Is0, flagRead(SF), B.CreateICmpSLT(Res, Zero)));
    flagWrite(PF, B.CreateSelect(Is0, flagRead(PF), parityOf(Res)));
  }

  // CF = last bit shifted out, only defined for 1 <= count <= W.
  Value *InRange =
      B.CreateAnd(B.CreateICmpUGE(C8, constI(1, 64)),
                  B.CreateICmpULE(C8, constI(W, 64)));
  Value *Cfc;
  if (Name == "shl") {
    Value *Pos = B.CreateSub(constI(W, 64), C8);
    Cfc = B.CreateTrunc(B.CreateLShr(B.CreateZExt(V, iN(64)), Pos), iN(1));
  } else if (Name == "shr") {
    Cfc = B.CreateTrunc(B.CreateLShr(B.CreateZExt(V, iN(64)),
                                     B.CreateSub(C8, constI(1, 64))),
                        iN(1));
  } else if (Name == "sar") {
    Value *Bit = B.CreateTrunc(
        B.CreateLShr(B.CreateZExt(V, iN(64)), B.CreateSub(C8, constI(1, 64))),
        iN(1));
    Cfc = B.CreateSelect(B.CreateICmpULE(C8, constI(W, 64)), Bit,
                         B.CreateICmpSLT(V, Zero));
  } else if (Name == "rol") {
    Cfc = B.CreateTrunc(Res, iN(1));
  } else { // ror
    Cfc = lowBit(B, Res, W - 1);
  }
  flagWrite(CF, B.CreateSelect(InRange, Cfc, UndefValue::get(iN(1))));

  // OF defined only for count == 1.
  Value *Of1;
  if (Name == "shl")
    Of1 = B.CreateXor(lowBit(B, Res, W - 1), Cfc);
  else if (Name == "shr")
    Of1 = lowBit(B, V, W - 1);
  else if (Name == "sar")
    Of1 = B.getFalse();
  else if (Name == "rol")
    Of1 = B.CreateXor(lowBit(B, Res, W - 1), B.CreateTrunc(Res, iN(1)));
  else
    Of1 = B.CreateXor(lowBit(B, Res, W - 1), lowBit(B, Res, W - 2));
  flagWrite(OF, B.CreateSelect(B.CreateICmpEQ(C8, constI(1, 64)), Of1,
                               UndefValue::get(iN(1))));
  return Res;
}

Value *X86LiftContext::flagsShift2(StringRef Name, Value *V, Value *Extra,
                                   Value *Cnt) {
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *E = zextOrTrunc(Extra, V->getType());
  Value *C = B.CreateZExtOrTrunc(Cnt, iN(64));
  Value *C8 = B.CreateAnd(C, constI(W == 64 ? 63 : 31, 64));
  Value *Wide = B.CreateOr(
      B.CreateShl(B.CreateZExt(V, iN(2 * W)), constI(W, 2 * W)),
      B.CreateZExt(E, iN(2 * W)));
  Value *CW = B.CreateTrunc(C8, iN(2 * W));
  Value *Res;
  if (Name == "shld")
    Res = B.CreateTrunc(
        B.CreateLShr(Wide, B.CreateSub(constI(2 * W, 2 * W), CW)), iN(W));
  else
    Res = B.CreateTrunc(B.CreateLShr(Wide, CW), iN(W));
  Value *Is0 = B.CreateICmpEQ(C8, constI(0, 64));
  Res = B.CreateSelect(Is0, V, Res);

  Value *Zero = constI(0, W);
  flagWrite(ZF, B.CreateSelect(Is0, flagRead(ZF), B.CreateICmpEQ(Res, Zero)));
  flagWrite(SF, B.CreateSelect(Is0, flagRead(SF), B.CreateICmpSLT(Res, Zero)));
  flagWrite(PF, B.CreateSelect(Is0, flagRead(PF), parityOf(Res)));
  // CF = last bit shifted out.
  Value *Cfc;
  if (Name == "shld")
    Cfc = B.CreateTrunc(
        B.CreateLShr(Wide,
                     B.CreateSub(constI(2 * W, 2 * W),
                                 B.CreateTrunc(C8, iN(2 * W)))),
        iN(1));
  else
    Cfc = B.CreateTrunc(B.CreateLShr(
                            B.CreateZExt(V, iN(64)),
                            B.CreateSub(C8, constI(1, 64))),
                        iN(1));
  Value *InRange =
      B.CreateAnd(B.CreateICmpUGE(C8, constI(1, 64)),
                  B.CreateICmpULE(C8, constI(2 * W, 64)));
  flagWrite(CF, B.CreateSelect(InRange, Cfc, UndefValue::get(iN(1))));
  Value *Is1 = B.CreateICmpEQ(C8, constI(1, 64));
  Value *Of1 = B.CreateXor(lowBit(B, Res, W - 1), lowBit(B, V, W - 1));
  flagWrite(OF, B.CreateSelect(Is1, Of1, UndefValue::get(iN(1))));
  return Res;
}

// ---- condition codes -----------------------------------------------------------

Value *X86LiftContext::condPred(unsigned CC) {
  Value *Cf = flagRead(CF), *Zf = flagRead(ZF), *Sf = flagRead(SF);
  Value *Of = flagRead(OF), *Pf = flagRead(PF);
  switch (CC) {
  case X86::COND_O:  return Of;
  case X86::COND_NO: return B.CreateNot(Of);
  case X86::COND_B:  return Cf;
  case X86::COND_AE: return B.CreateNot(Cf);
  case X86::COND_E:  return Zf;
  case X86::COND_NE: return B.CreateNot(Zf);
  case X86::COND_BE: return B.CreateOr(Cf, Zf);
  case X86::COND_A:  return B.CreateNot(B.CreateOr(Cf, Zf));
  case X86::COND_S:  return Sf;
  case X86::COND_NS: return B.CreateNot(Sf);
  case X86::COND_P:  return Pf;
  case X86::COND_NP: return B.CreateNot(Pf);
  case X86::COND_L:  return B.CreateICmpNE(Sf, Of);
  case X86::COND_GE: return B.CreateICmpEQ(Sf, Of);
  case X86::COND_LE: return B.CreateOr(Zf, B.CreateICmpNE(Sf, Of));
  case X86::COND_G:  return B.CreateAnd(B.CreateNot(Zf), B.CreateICmpEQ(Sf, Of));
  default:           return UndefValue::get(iN(1));
  }
}

Value *X86LiftContext::condPredOp(const MCInst &MI, unsigned MIOpNo) {
  return condPred(static_cast<unsigned>(MI.getOperand(MIOpNo).getImm()));
}

Value *X86LiftContext::setccVal(unsigned CC) {
  return B.CreateZExt(condPred(CC), iN(8));
}

// ---- control flow ------------------------------------------------------------

Value *X86LiftContext::pcrelTarget(const MCInst &MI, unsigned MIOpNo) {
  return constI(NextVA + MI.getOperand(MIOpNo).getImm(), 64);
}

void X86LiftContext::brOp(const MCInst &MI, unsigned MIOpNo) {
  if (opKind(MI, MIOpNo) == OpKind::Imm)
    jumpToVA(pcrelTarget(MI, MIOpNo));
  else
    jumpToVA(readOp(MI, MIOpNo, iN(64)));
}

void X86LiftContext::brccOp(const MCInst &MI, unsigned DstOpNo,
                            unsigned CondOpNo) {
  condJump(condPredOp(MI, CondOpNo), pcrelTarget(MI, DstOpNo));
}

void X86LiftContext::callToOp(const MCInst &MI, unsigned MIOpNo) {
  pushVal(constI(NextVA, 64));
  Value *T = (opKind(MI, MIOpNo) == OpKind::Imm)
                 ? pcrelTarget(MI, MIOpNo)
                 : readOp(MI, MIOpNo, iN(64));
  Flows.push_back({PendingFlow::Call, CurVA, T, nullptr});
}

void X86LiftContext::retAdjOp(const MCInst &MI, unsigned AdjOpNo) {
  int64_t Adj =
      (MI.getNumOperands() > AdjOpNo && MI.getOperand(AdjOpNo).isImm())
          ? MI.getOperand(AdjOpNo).getImm()
          : 0;
  regWrite(X86::RSP, B.CreateAdd(regRead(X86::RSP, iN(64)),
                                 constI(8 + Adj, 64)));
  Flows.push_back({PendingFlow::Return, CurVA, nullptr, nullptr});
}

void X86LiftContext::jumpToVA(Value *Target) {
  LiftContext::jumpToVA(Target);
  consumePrefixes();
}

void X86LiftContext::condJump(Value *Pred, Value *Target) {
  LiftContext::condJump(Pred, Target);
  consumePrefixes();
}

void X86LiftContext::callOp(const MCInst &MI, unsigned MIOpNo) {
  callToOp(MI, MIOpNo);
}

void X86LiftContext::retOp() {
  regWrite(X86::RSP, B.CreateAdd(regRead(X86::RSP, iN(64)), constI(8, 64)));
  LiftContext::retOp();
}

// ---- stack ---------------------------------------------------------------------

void X86LiftContext::pushVal(Value *V) {
  unsigned W = V->getType()->getPrimitiveSizeInBits();
  Value *Rsp = B.CreateSub(regRead(X86::RSP, iN(64)), constI(W / 8, 64));
  regWrite(X86::RSP, Rsp);
  memWriteAddr(Rsp, V);
}

Value *X86LiftContext::popVal(unsigned Width) {
  Value *Rsp = regRead(X86::RSP, iN(64));
  Value *V = memReadAddr(Rsp, iN(Width));
  regWrite(X86::RSP, B.CreateAdd(Rsp, constI(Width / 8, 64)));
  return V;
}

void X86LiftContext::pushFlagsVal() {
  // PUSHF64 pushes a full qword even though only 16 bits are meaningful.
  pushVal(B.CreateZExt(packFlags(16), iN(64)));
}

void X86LiftContext::popFlagsVal() { unpackFlags(popVal(64)); }

void X86LiftContext::leaveOp() {
  Value *Rbp = regRead(X86::RBP, iN(64));
  regWrite(X86::RSP, Rbp);
  regWrite(X86::RBP, popVal(64));
}

// ---- moves / exchange -------------------------------------------------------------

void X86LiftContext::xchgOps(const MCInst &MI, unsigned AOp, unsigned BOp) {
  Type *T = nullptr;
  for (unsigned O : {AOp, BOp})
    if (opKind(MI, O) == OpKind::Reg)
      T = iN(regBits(MI.getOperand(O).getReg()));
  if (!T)
    T = iN(64);
  Value *A = readOp(MI, AOp, T);
  Value *Bv = readOp(MI, BOp, T);
  writeOp(MI, AOp, Bv);
  writeOp(MI, BOp, A);
}

Value *X86LiftContext::xaddVal(const MCInst &MI, unsigned DstOp,
                               unsigned SrcOp, Value *A, Value *B_) {
  Value *Old = A;
  Value *Sum = flagsAdd(A, B_);
  writeOp(MI, SrcOp, Old);
  return Sum;
}

// ---- bit ops -----------------------------------------------------------------------

Value *X86LiftContext::bitTestOp(StringRef Op, Value *Base, Value *Off,
                                 bool IsMem, Value *MemAddr,
                                 unsigned EltWidth) {
  Value *Idx = B.CreateZExtOrTrunc(Off, iN(64));
  if (IsMem) {
    // Bit offsets address the full bitstring: elt = Idx / EltWidth.
    Value *Elt = B.CreateUDiv(Idx, constI(EltWidth, 64));
    Value *Addr =
        B.CreateAdd(MemAddr, B.CreateMul(Elt, constI(EltWidth / 8, 64)));
    Value *W = memReadAddr(Addr, iN(EltWidth));
    Value *BitOff = B.CreateURem(Idx, constI(EltWidth, 64));
    Value *Bit = B.CreateTrunc(
        B.CreateLShr(W, B.CreateTrunc(BitOff, iN(EltWidth))), iN(1));
    flagWrite(CF, Bit);
    if (Op == "bt")
      return Bit;
    Value *Mask =
        B.CreateShl(constI(1, EltWidth), B.CreateTrunc(BitOff, iN(EltWidth)));
    Value *New;
    if (Op == "bts")
      New = B.CreateOr(W, Mask);
    else if (Op == "btr")
      New = B.CreateAnd(W, B.CreateNot(Mask));
    else
      New = B.CreateXor(W, Mask);
    memWriteAddr(Addr, New);
    return New;
  }
  unsigned W = Base->getType()->getIntegerBitWidth();
  Value *O = B.CreateURem(B.CreateZExtOrTrunc(Off, iN(64)), constI(W, 64));
  Value *Bit = B.CreateTrunc(
      B.CreateLShr(Base, B.CreateTrunc(O, Base->getType())), iN(1));
  flagWrite(CF, Bit);
  if (Op == "bt")
    return Bit;
  Value *Mask =
      B.CreateShl(constI(1, W), B.CreateTrunc(O, Base->getType()));
  if (Op == "bts")
    return B.CreateOr(Base, Mask);
  if (Op == "btr")
    return B.CreateAnd(Base, B.CreateNot(Mask));
  return B.CreateXor(Base, Mask);
}

Value *X86LiftContext::bsfOp(Value *V) {
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *IsZ = B.CreateICmpEQ(V, constI(0, W));
  flagWrite(ZF, IsZ);
  Value *Idx =
      B.CreateIntrinsic(Intrinsic::cttz, {V->getType()}, {V, B.getTrue()});
  for (FlagId F : {CF, SF, OF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
  return B.CreateSelect(IsZ, UndefValue::get(V->getType()), Idx);
}

Value *X86LiftContext::bsrOp(Value *V) {
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *IsZ = B.CreateICmpEQ(V, constI(0, W));
  flagWrite(ZF, IsZ);
  Value *Lz =
      B.CreateIntrinsic(Intrinsic::ctlz, {V->getType()}, {V, B.getTrue()});
  Value *Idx = B.CreateSub(constI(W - 1, W), Lz);
  for (FlagId F : {CF, SF, OF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
  return B.CreateSelect(IsZ, UndefValue::get(V->getType()), Idx);
}

Value *X86LiftContext::tzcntOp(Value *V) {
  Value *R =
      B.CreateIntrinsic(Intrinsic::cttz, {V->getType()}, {V, B.getFalse()});
  unsigned W = V->getType()->getIntegerBitWidth();
  flagWrite(CF, B.CreateICmpEQ(V, constI(0, W)));
  flagWrite(ZF, B.CreateICmpEQ(R, constI(0, W)));
  for (FlagId F : {SF, OF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
  return R;
}

Value *X86LiftContext::lzcntOp(Value *V) {
  Value *R =
      B.CreateIntrinsic(Intrinsic::ctlz, {V->getType()}, {V, B.getFalse()});
  unsigned W = V->getType()->getIntegerBitWidth();
  flagWrite(CF, B.CreateICmpEQ(V, constI(0, W)));
  flagWrite(ZF, B.CreateICmpEQ(R, constI(0, W)));
  for (FlagId F : {SF, OF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
  return R;
}

Value *X86LiftContext::popcntOp(Value *V) {
  Value *R = B.CreateIntrinsic(Intrinsic::ctpop, {V->getType()}, {V});
  unsigned W = V->getType()->getIntegerBitWidth();
  flagWrite(ZF, B.CreateICmpEQ(V, constI(0, W)));
  for (FlagId F : {CF, SF, OF, PF, AF})
    flagWrite(F, F == CF ? static_cast<Value *>(B.getFalse())
                         : UndefValue::get(iN(1)));
  return R;
}

Value *X86LiftContext::bswapOp(Value *V) {
  return B.CreateIntrinsic(Intrinsic::bswap, {V->getType()}, {V});
}

Value *X86LiftContext::rorxOp(Value *V, Value *Cnt) {
  unsigned W = V->getType()->getIntegerBitWidth();
  Value *C =
      B.CreateURem(B.CreateZExtOrTrunc(Cnt, V->getType()), constI(W, W));
  return B.CreateIntrinsic(Intrinsic::fshr, {V->getType()}, {V, V, C});
}

// ---- writes with zeroing --------------------------------------------------------

void X86LiftContext::regWriteZ(MCRegister Reg, Value *V) {
  MCRegister Root = rootReg(Reg);
  AllocaInst *S = slotFor(Reg);
  unsigned RootW = SlotBits[Root];
  auto [Off, W] = regOffsetBits(Reg);
  Value *I = toInt(V, W);
  if (Off == 0) {
    // Full-width or zero-extending write: store zext to root width.
    B.CreateStore(B.CreateZExtOrTrunc(I, iN(RootW)), S);
    return;
  }
  regBitsWrite(Reg, V);
}

void X86LiftContext::writeOpZ(const MCInst &MI, unsigned MIOpNo, Value *V) {
  if (opKind(MI, MIOpNo) == OpKind::Reg) {
    regWriteZ(MI.getOperand(MIOpNo).getReg(), V);
    return;
  }
  writeOp(MI, MIOpNo, V);
}

Type *X86LiftContext::fltTy(unsigned Bits) {
  switch (Bits) {
  case 16: return Type::getHalfTy(Ctx);
  case 32: return Type::getFloatTy(Ctx);
  case 64: return Type::getDoubleTy(Ctx);
  case 80: return Type::getX86_FP80Ty(Ctx);
  case 128: return Type::getFP128Ty(Ctx);
  default: return iN(Bits);
  }
}

// ---- vector / scalar fp helpers --------------------------------------------------

static Type *vecTyOf(X86LiftContext::BuilderTy &, Type *IntTy,
                     unsigned EltBits) {
  unsigned W = IntTy->getIntegerBitWidth();
  return FixedVectorType::get(IntegerType::get(IntTy->getContext(), EltBits),
                              W / EltBits);
}

Value *X86LiftContext::vop(StringRef Name, Value *A, Value *Bv,
                           int64_t Imm) {
  Type *F32 = Type::getFloatTy(Ctx);
  Type *F64 = Type::getDoubleTy(Ctx);
  Value *I128 = A && A->getType()->isIntegerTy() ? toInt(A, 128) : A;

  auto Vec128 = [&](Type *Elt) {
    return FixedVectorType::get(Elt, 128 / Elt->getPrimitiveSizeInBits());
  };
  auto As = [&](Value *V, Type *T) { return B.CreateBitCast(V, T); };

  if (Name == "xlane0f32")
    return B.CreateExtractElement(As(I128, Vec128(F32)), uint64_t(0));
  if (Name == "xlane0f64")
    return B.CreateExtractElement(As(I128, Vec128(F64)), uint64_t(0));
  if (Name == "xsetlane0f32") {
    Value *Vec = As(I128, Vec128(F32));
    Value *S = Bv && Bv->getType()->isFloatTy()
                   ? Bv
                   : B.CreateBitCast(toInt(Bv, 32), F32);
    return B.CreateBitCast(
        B.CreateInsertElement(Vec, S, uint64_t(0)), iN(128));
  }
  if (Name == "xsetlane0f64") {
    Value *Vec = As(I128, Vec128(F64));
    Value *S = Bv && Bv->getType()->isDoubleTy()
                   ? Bv
                   : B.CreateBitCast(toInt(Bv, 64), F64);
    return B.CreateBitCast(
        B.CreateInsertElement(Vec, S, uint64_t(0)), iN(128));
  }
  if (Name == "xnewlane0f32") {
    Value *Vec = UndefValue::get(Vec128(F32));
    Value *S = Bv && Bv->getType()->isFloatTy()
                   ? Bv
                   : B.CreateBitCast(toInt(Bv, 32), F32);
    return B.CreateBitCast(
        B.CreateInsertElement(Vec, S, uint64_t(0)), iN(128));
  }
  if (Name == "xnewlane0f64") {
    Value *Vec = UndefValue::get(Vec128(F64));
    Value *S = Bv && Bv->getType()->isDoubleTy()
                   ? Bv
                   : B.CreateBitCast(toInt(Bv, 64), F64);
    return B.CreateBitCast(
        B.CreateInsertElement(Vec, S, uint64_t(0)), iN(128));
  }
  if (Name == "sitofpf32")
    return B.CreateSIToFP(toInt(A, 64), F32);
  if (Name == "sitofpf64")
    return B.CreateSIToFP(toInt(A, 64), F64);
  if (Name == "fminnum")
    return B.CreateBinaryIntrinsic(Intrinsic::minnum, A, Bv);
  if (Name == "fmaxnum")
    return B.CreateBinaryIntrinsic(Intrinsic::maxnum, A, Bv);
  if (Name == "fsqrt")
    return B.CreateIntrinsic(Intrinsic::sqrt, {A->getType()}, {A});
  if (Name == "fpext")
    return B.CreateFPExt(A, Bv ? Bv->getType() : F64);
  if (Name == "fptrunc")
    return B.CreateFPTrunc(A, Bv ? Bv->getType() : F32);

  // Packed shifts: cnt comes as a scalar; splat it over the element type.
  if (Name == "psll" || Name == "psrl" || Name == "psra") {
    unsigned EB = static_cast<unsigned>(Imm);
    Value *Cnt = toInt(Bv, EB);
    Value *Splat = B.CreateVectorSplat(128 / EB, Cnt);
    Value *Vec = As(I128, vecTyOf(B, iN(128), EB));
    Value *R = Name == "psll"  ? B.CreateShl(Vec, Splat)
               : Name == "psrl" ? B.CreateLShr(Vec, Splat)
                                : B.CreateAShr(Vec, Splat);
    return B.CreateBitCast(R, iN(128));
  }
  if (Name == "pslldq" || Name == "psrldq") {
    // Whole-register byte shift on the i128 image.
    Value *V = I128;
    uint64_t Sh = static_cast<uint64_t>(Imm) * 8;
    if (Sh >= 128)
      return constI(0, 128);
    return Name == "pslldq" ? B.CreateShl(V, constI(Sh, 128))
                            : B.CreateLShr(V, constI(Sh, 128));
  }

  // Shuffle helpers take A (and Bv) as i128 plus an 8-bit imm.
  auto Shuf = [&](ArrayRef<int> M, Value *Va, Value *Vb, unsigned EltBits) {
    Type *VT = vecTyOf(B, iN(128), EltBits);
    Value *X = As(toInt(Va, 128), VT);
    Value *Y = Vb ? As(toInt(Vb, 128), VT) : UndefValue::get(VT);
    return B.CreateBitCast(B.CreateShuffleVector(X, Y, M), iN(128));
  };
  if (Name == "pshufd") {
    int M[] = {int(Imm & 3), int((Imm >> 2) & 3), int((Imm >> 4) & 3),
               int((Imm >> 6) & 3)};
    return Shuf(M, A, nullptr, 32);
  }
  if (Name == "pshufhw") {
    int M[] = {0, 1, 2, 3,
               4 + int(Imm & 3), 4 + int((Imm >> 2) & 3),
               4 + int((Imm >> 4) & 3), 4 + int((Imm >> 6) & 3)};
    return Shuf(M, A, nullptr, 16);
  }
  if (Name == "pshuflw") {
    int M[] = {int(Imm & 3), int((Imm >> 2) & 3), int((Imm >> 4) & 3),
               int((Imm >> 6) & 3), 4, 5, 6, 7};
    return Shuf(M, A, nullptr, 16);
  }
  if (Name == "shufps") {
    // dest lanes 0-1 from A, lanes 2-3 from B.
    int M[] = {int(Imm & 3), int((Imm >> 2) & 3),
               4 + int((Imm >> 4) & 3), 4 + int((Imm >> 6) & 3)};
    return Shuf(M, A, Bv, 32);
  }
  if (Name == "shufpd") {
    int M[] = {int(Imm & 1), 2 + int((Imm >> 1) & 1)};
    return Shuf(M, A, Bv, 64);
  }
  if (Name == "punpckl" || Name == "punpckh") {
    unsigned EB = static_cast<unsigned>(Imm);
    unsigned N = 128 / EB;
    SmallVector<int, 32> M;
    unsigned Lo = Name == "punpckl" ? 0 : N / 2;
    for (unsigned I = 0; I < N / 2; ++I) {
      M.push_back(Lo + I);
      M.push_back(N + Lo + I);
    }
    return Shuf(M, A, Bv, EB);
  }
  if (Name == "pshufb") {
    // Select each byte with a dynamic lane index. A control byte with its high
    // bit set produces zero.
    Type *VT = FixedVectorType::get(iN(8), 16);
    Value *Data = As(I128, VT);
    Value *Mask = As(toInt(Bv, 128), VT);
    Value *Result = UndefValue::get(VT);
    for (unsigned I = 0; I < 16; ++I) {
      Value *Control = B.CreateExtractElement(Mask, I);
      Value *Zero = constI(0, 8);
      Value *Index = B.CreateAnd(Control, constI(0x0f, 8));
      Value *Byte = B.CreateExtractElement(Data, Index);
      Value *HighBit = B.CreateICmpNE(
          B.CreateAnd(Control, constI(0x80, 8)), Zero);
      Result = B.CreateInsertElement(
          Result, B.CreateSelect(HighBit, Zero, Byte), I);
    }
    return B.CreateBitCast(Result, iN(128));
  }
  if (Name == "pinsrw") {
    Type *VT = vecTyOf(B, iN(128), 16);
    Value *Vec = As(I128, VT);
    return B.CreateBitCast(
        B.CreateInsertElement(Vec, B.CreateTrunc(toInt(Bv, 16), iN(16)),
                              static_cast<uint64_t>(Imm)),
        iN(128));
  }
  if (Name == "pmovmskb") {
    Type *VT = vecTyOf(B, iN(128), 8);
    Value *Vec = As(I128, VT);
    Value *Signs = B.CreateICmpSLT(Vec, Constant::getNullValue(VT));
    return B.CreateZExt(B.CreateBitCast(Signs, iN(16)), iN(32));
  }
  if (Name == "movmskps" || Name == "movmskpd") {
    unsigned EB = Name == "movmskps" ? 32 : 64;
    Type *VT = vecTyOf(B, iN(128), EB);
    Value *Vec = As(I128, VT);
    Value *Signs = B.CreateICmpSLT(Vec, Constant::getNullValue(VT));
    return B.CreateZExt(
        B.CreateBitCast(Signs, iN(128 / EB)), iN(32));
  }
  if (Name == "ptest") {
    // Handled by ptestOp; fall through to undef.
    return nullptr;
  }
  if (Name == "vbroadcastb") {
    Value *X = toInt(A, 8);
    // x * 0x0101010101010101 splats the low byte over 64 bits, then splat
    // that qword into both halves.
    Value *Q = B.CreateMul(B.CreateZExt(X, iN(64)),
                           constI(0x0101010101010101ULL, 64));
    Value *D = B.CreateOr(B.CreateShl(B.CreateZExt(Q, iN(128)),
                                      constI(64, 128)),
                          B.CreateZExt(Q, iN(128)));
    return D;
  }
  if (Name == "vcvtdq2pd") {
    Type *IVT = vecTyOf(B, iN(128), 32);
    Value *V = As(I128, IVT);
    Value *Lo2 = B.CreateShuffleVector(V, V, {0, 1});
    return B.CreateBitCast(
        B.CreateSIToFP(Lo2, FixedVectorType::get(F64, 2)), iN(128));
  }
  return nullptr;
}

void X86LiftContext::ptestOp(Value *A, Value *Bv) {
  Value *X = toInt(A, 128), *Y = toInt(Bv, 128);
  flagWrite(ZF, B.CreateICmpEQ(B.CreateAnd(X, Y), constI(0, 128)));
  flagWrite(CF,
            B.CreateICmpEQ(B.CreateAnd(B.CreateNot(X), Y), constI(0, 128)));
  flagWrite(OF, B.getFalse());
  flagWrite(SF, B.getFalse());
  flagWrite(AF, B.getFalse());
  flagWrite(PF, B.getFalse());
}

// ---- mul/div families ------------------------------------------------------------

void X86LiftContext::mulPair(Value *Src, unsigned W, bool Signed) {
  MCRegister Acc =
      W == 8 ? X86::AL : W == 16 ? X86::AX : W == 32 ? X86::EAX : X86::RAX;
  MCRegister Hi =
      W == 8 ? X86::AH : W == 16 ? X86::DX : W == 32 ? X86::EDX : X86::RDX;
  Value *A = regRead(Acc, iN(W));
  Value *S = zextOrTrunc(Src, iN(W));
  Value *Wide =
      Signed ? B.CreateMul(B.CreateSExt(A, iN(2 * W)),
                           B.CreateSExt(S, iN(2 * W)))
             : B.CreateMul(B.CreateZExt(A, iN(2 * W)),
                           B.CreateZExt(S, iN(2 * W)));
  if (W == 8) {
    regWrite(X86::AX, B.CreateTrunc(Wide, iN(16)));
    Value *Ov = B.CreateICmpNE(
        Wide, B.CreateSExt(B.CreateTrunc(Wide, iN(8)), iN(16)));
    flagWrite(CF, Ov);
    flagWrite(OF, Ov);
  } else {
    Value *Lo = B.CreateTrunc(Wide, iN(W));
    Value *H = B.CreateTrunc(B.CreateLShr(Wide, constI(W, 2 * W)), iN(W));
    regWrite(Acc, Lo);
    regWrite(Hi, H);
    Value *Ov =
        Signed ? B.CreateICmpNE(H, B.CreateAShr(Lo, constI(W - 1, W)))
               : B.CreateICmpNE(H, constI(0, W));
    flagWrite(CF, Ov);
    flagWrite(OF, Ov);
  }
  for (FlagId F : {SF, ZF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
}

void X86LiftContext::divPair(Value *Src, unsigned W, bool Signed) {
  MCRegister Acc =
      W == 8 ? X86::AL : W == 16 ? X86::AX : W == 32 ? X86::EAX : X86::RAX;
  MCRegister Hi =
      W == 8 ? X86::AH : W == 16 ? X86::DX : W == 32 ? X86::EDX : X86::RDX;
  Value *S = zextOrTrunc(Src, iN(W));
  if (W == 8) {
    Value *Dividend = regRead(X86::AX, iN(16));
    Value *S2 = Signed ? B.CreateSExt(S, iN(16)) : B.CreateZExt(S, iN(16));
    Value *Q =
        Signed ? B.CreateSDiv(Dividend, S2) : B.CreateUDiv(Dividend, S2);
    Value *R =
        Signed ? B.CreateSRem(Dividend, S2) : B.CreateURem(Dividend, S2);
    regWrite(X86::AL, B.CreateTrunc(Q, iN(8)));
    regWrite(X86::AH, B.CreateTrunc(R, iN(8)));
    return;
  }
  Value *Lo = B.CreateZExt(regRead(Acc, iN(W)), iN(2 * W));
  Value *H =
      B.CreateShl(B.CreateZExt(regRead(Hi, iN(W)), iN(2 * W)), constI(W, 2 * W));
  Value *Dividend = B.CreateOr(H, Lo);
  Value *S2 = Signed ? B.CreateSExt(S, iN(2 * W)) : B.CreateZExt(S, iN(2 * W));
  Value *Q = Signed ? B.CreateSDiv(Dividend, S2) : B.CreateUDiv(Dividend, S2);
  Value *R = Signed ? B.CreateSRem(Dividend, S2) : B.CreateURem(Dividend, S2);
  regWrite(Acc, B.CreateTrunc(Q, iN(W)));
  regWrite(Hi, B.CreateTrunc(R, iN(W)));
  for (FlagId F : {CF, ZF, SF, OF, PF, AF})
    flagWrite(F, UndefValue::get(iN(1)));
}

void X86LiftContext::sextAcc(unsigned W) {
  // W in {16,32,64}: CBW/CWDE/CDQE extend the accumulator.
  // W in {116,132,164}: CWD/CDQ/CQO fill DX/EDX/RDX with the sign.
  switch (W) {
  case 16:
    regWrite(X86::AX, B.CreateSExt(regRead(X86::AL, iN(8)), iN(16)));
    return;
  case 32:
    regWrite(X86::EAX, B.CreateSExt(regRead(X86::AX, iN(16)), iN(32)));
    return;
  case 64:
    regWrite(X86::RAX, B.CreateSExt(regRead(X86::EAX, iN(32)), iN(64)));
    return;
  }
  unsigned AW = W - 100;
  MCRegister Acc = AW == 16 ? X86::AX : AW == 32 ? X86::EAX : X86::RAX;
  MCRegister Hi = AW == 16 ? X86::DX : AW == 32 ? X86::EDX : X86::RDX;
  regWrite(Hi, B.CreateAShr(regRead(Acc, iN(AW)), constI(AW - 1, AW)));
}

// ---- flag image --------------------------------------------------------------------

Value *X86LiftContext::packFlags(unsigned Width) {
  // rflags bit positions: CF=0 PF=2 AF=4 ZF=6 SF=7 DF=10 OF=11; bit1 = 1.
  Value *V = constI(0x2, 16);
  auto Put = [&](unsigned Bit, Value *F) {
    V = B.CreateOr(V, B.CreateShl(B.CreateZExt(F, iN(16)), constI(Bit, 16)));
  };
  Put(0, flagRead(CF));
  Put(2, flagRead(PF));
  Put(4, flagRead(AF));
  Put(6, flagRead(ZF));
  Put(7, flagRead(SF));
  Put(10, flagRead(DF));
  Put(11, flagRead(OF));
  return zextOrTrunc(V, iN(Width));
}

void X86LiftContext::unpackFlags(Value *V) {
  Value *W = zextOrTrunc(V, iN(16));
  flagWrite(CF, lowBit(B, W, 0));
  flagWrite(PF, lowBit(B, W, 2));
  flagWrite(AF, lowBit(B, W, 4));
  flagWrite(ZF, lowBit(B, W, 6));
  flagWrite(SF, lowBit(B, W, 7));
  flagWrite(DF, lowBit(B, W, 10));
  flagWrite(OF, lowBit(B, W, 11));
}

void X86LiftContext::sahfOp() {
  Value *Ah = regRead(X86::AH, iN(8));
  flagWrite(SF, lowBit(B, Ah, 7));
  flagWrite(ZF, lowBit(B, Ah, 6));
  flagWrite(AF, lowBit(B, Ah, 4));
  flagWrite(PF, lowBit(B, Ah, 2));
  flagWrite(CF, lowBit(B, Ah, 0));
}

Value *X86LiftContext::lahfVal() {
  Value *V = constI(0x2, 8); // bit 1 always set
  auto Put = [&](unsigned Bit, Value *F) {
    V = B.CreateOr(V, B.CreateShl(B.CreateZExt(F, iN(8)), constI(Bit, 8)));
  };
  Put(0, flagRead(CF));
  Put(2, flagRead(PF));
  Put(4, flagRead(AF));
  Put(6, flagRead(ZF));
  Put(7, flagRead(SF));
  return V;
}

// ---- cmpxchg ------------------------------------------------------------------------

Value *X86LiftContext::cmpxchgOp(Value *Addr, Value *Expected, Value *New) {
  Value *Old = memReadAddr(Addr, New->getType());
  Value *Eq = B.CreateICmpEQ(Old, Expected);
  // Flags come from the subtraction whether or not the store happens.
  flagsSub(Old, Expected);
  memWriteAddr(Addr, B.CreateSelect(Eq, New, Old));
  return Old;
}

void X86LiftContext::cmpxchgPair(Value *Addr, unsigned W) {
  if (W == 64) {
    Value *Acc = B.CreateOr(
        B.CreateZExt(regRead(X86::EAX, iN(32)), iN(64)),
        B.CreateShl(B.CreateZExt(regRead(X86::EDX, iN(32)), iN(64)),
                    constI(32, 64)));
    Value *New = B.CreateOr(
        B.CreateZExt(regRead(X86::EBX, iN(32)), iN(64)),
        B.CreateShl(B.CreateZExt(regRead(X86::ECX, iN(32)), iN(64)),
                    constI(32, 64)));
    Value *Old = memReadAddr(Addr, iN(64));
    Value *Eq = B.CreateICmpEQ(Old, Acc);
    flagWrite(ZF, Eq);
    memWriteAddr(Addr, B.CreateSelect(Eq, New, Old));
    Value *Res = B.CreateSelect(Eq, Acc, Old);
    regWrite(X86::EAX, B.CreateTrunc(Res, iN(32)));
    regWrite(X86::EDX, B.CreateTrunc(B.CreateLShr(Res, constI(32, 64)),
                                     iN(32)));
    return;
  }
  Value *Acc = B.CreateOr(
      B.CreateZExt(regRead(X86::RAX, iN(64)), iN(128)),
      B.CreateShl(B.CreateZExt(regRead(X86::RDX, iN(64)), iN(128)),
                  constI(64, 128)));
  Value *New = B.CreateOr(
      B.CreateZExt(regRead(X86::RBX, iN(64)), iN(128)),
      B.CreateShl(B.CreateZExt(regRead(X86::RCX, iN(64)), iN(128)),
                  constI(64, 128)));
  Value *Old = memReadAddr(Addr, iN(128));
  Value *Eq = B.CreateICmpEQ(Old, Acc);
  flagWrite(ZF, Eq);
  memWriteAddr(Addr, B.CreateSelect(Eq, New, Old));
  Value *Res = B.CreateSelect(Eq, Acc, Old);
  regWrite(X86::RAX, B.CreateTrunc(Res, iN(64)));
  regWrite(X86::RDX, B.CreateTrunc(B.CreateLShr(Res, constI(64, 128)),
                                   iN(64)));
}

// ---- atomics ---------------------------------------------------------------------------

Value *X86LiftContext::atomicRMW(StringRef Name, Value *Addr, Value *Val) {
  AtomicRMWInst::BinOp Op;
  if (Name == "add")
    Op = AtomicRMWInst::Add;
  else if (Name == "sub")
    Op = AtomicRMWInst::Sub;
  else if (Name == "and")
    Op = AtomicRMWInst::And;
  else if (Name == "or")
    Op = AtomicRMWInst::Or;
  else if (Name == "xor")
    Op = AtomicRMWInst::Xor;
  else
    Op = AtomicRMWInst::Xchg;
  return B.CreateAtomicRMW(Op, B.CreateIntToPtr(Addr, ptrTy()), Val,
                           MaybeAlign(1),
                           AtomicOrdering::SequentiallyConsistent);
}

void X86LiftContext::emitFence() {
  B.CreateFence(AtomicOrdering::SequentiallyConsistent);
}

// ---- bulk memory -----------------------------------------------------------------------

void X86LiftContext::memMove(Value *Dst, Value *Src, Value *Len) {
  B.CreateMemMove(B.CreateIntToPtr(Dst, ptrTy()), MaybeAlign(1),
                  B.CreateIntToPtr(Src, ptrTy()), MaybeAlign(1),
                  zextOrTrunc(Len, iN(64)));
}

void X86LiftContext::memSet(Value *Dst, Value *Val, Value *Len) {
  B.CreateMemSet(B.CreateIntToPtr(Dst, ptrTy()),
                 B.CreateTrunc(toInt(Val, 8), iN(8)),
                 zextOrTrunc(Len, iN(64)), MaybeAlign(1));
}

// ---- prefixes ----------------------------------------------------------------------------

void X86LiftContext::prefixOp(StringRef Kind, MCRegister Reg) {
  if (Kind == "lock")
    PendingLock = true;
  else if (Kind == "rep")
    PendingRep = Rep;
  else if (Kind == "repe")
    PendingRep = RepE;
  else if (Kind == "repne")
    PendingRep = RepNE;
  else if (Kind == "seg")
    PendingSeg = Reg;
}

void X86LiftContext::consumePrefixes() {
  PendingRep = NoRep;
  PendingLock = false;
  PendingSeg = MCRegister();
}

// ---- x87 FPU --------------------------------------------------------------------------------

void X86LiftContext::ensureX87() {
  if (X87Stack)
    return;
  IRBuilder<> EB(&*Fn->getEntryBlock().getFirstInsertionPt());
  X87Stack =
      EB.CreateAlloca(Type::getX86_FP80Ty(Ctx), constI(8, 32), "st");
  X87Top = regSlot("fptop", iN(8));
  X87CW = regSlot("fpcw", iN(16));
  for (int I = 0; I < 4; ++I)
    X87CFlags[I] = regSlot(("fpc" + Twine(I)).str(), iN(1));
}

Value *X86LiftContext::gepStack(Value *Idx) {
  ensureX87();
  return B.CreateGEP(Type::getX86_FP80Ty(Ctx), X87Stack, Idx);
}

static Value *toF80(X86LiftContext::BuilderTy &B, LLVMContext &Ctx,
                    Value *V) {
  Type *F80 = Type::getX86_FP80Ty(Ctx);
  if (V->getType()->isX86_FP80Ty())
    return V;
  if (V->getType()->isFloatingPointTy())
    return B.CreateFPExt(V, F80);
  if (V->getType()->isIntegerTy(80))
    return B.CreateBitCast(V, F80);
  // Integer (FILD) or narrower bit pattern: sign-extend then convert.
  return B.CreateSIToFP(V, F80);
}

Value *X86LiftContext::x87St(unsigned I) {
  ensureX87();
  Value *Top = B.CreateLoad(iN(8), X87Top);
  Value *Idx = B.CreateAnd(B.CreateAdd(Top, constI(I, 8)), constI(7, 8));
  return B.CreateLoad(Type::getX86_FP80Ty(Ctx), gepStack(Idx));
}

void X86LiftContext::x87StSet(unsigned I, Value *V) {
  ensureX87();
  Value *Top = B.CreateLoad(iN(8), X87Top);
  Value *Idx = B.CreateAnd(B.CreateAdd(Top, constI(I, 8)), constI(7, 8));
  B.CreateStore(toF80(B, Ctx, V), gepStack(Idx));
}

void X86LiftContext::x87Push(Value *V) {
  ensureX87();
  Value *Top = B.CreateLoad(iN(8), X87Top);
  Top = B.CreateAnd(B.CreateSub(Top, constI(1, 8)), constI(7, 8));
  B.CreateStore(Top, X87Top);
  B.CreateStore(toF80(B, Ctx, V), gepStack(Top));
}

Value *X86LiftContext::x87Pop() {
  Value *V = x87St(0);
  Value *Top = B.CreateLoad(iN(8), X87Top);
  B.CreateStore(B.CreateAnd(B.CreateAdd(Top, constI(1, 8)), constI(7, 8)),
                X87Top);
  return V;
}

void X86LiftContext::x87Init() {
  ensureX87();
  B.CreateStore(constI(0, 8), X87Top);
  B.CreateStore(constI(0x37F, 16), X87CW);
}

void X86LiftContext::x87IncTop() {
  ensureX87();
  Value *Top = B.CreateLoad(iN(8), X87Top);
  B.CreateStore(B.CreateAnd(B.CreateAdd(Top, constI(1, 8)), constI(7, 8)),
                X87Top);
}

void X86LiftContext::x87DecTop() {
  ensureX87();
  Value *Top = B.CreateLoad(iN(8), X87Top);
  B.CreateStore(B.CreateAnd(B.CreateSub(Top, constI(1, 8)), constI(7, 8)),
                X87Top);
}

Value *X86LiftContext::x87CFlag(unsigned I) {
  ensureX87();
  return B.CreateLoad(iN(1), X87CFlags[I]);
}

void X86LiftContext::x87SetCFlag(unsigned I, Value *V) {
  ensureX87();
  B.CreateStore(B.CreateZExtOrTrunc(V, iN(1)), X87CFlags[I]);
}

void X86LiftContext::x87FCom(Value *V) {
  // C3 = (st0 == v), C2 = unordered, C0 = (st0 < v).
  Value *S = x87St(0);
  Value *B_ = V ? toF80(B, Ctx, V)
                : ConstantFP::getNegativeZero(Type::getX86_FP80Ty(Ctx));
  x87SetCFlag(0, B.CreateFCmpOLT(S, B_));
  x87SetCFlag(2, B.CreateFCmpUNO(S, B_));
  x87SetCFlag(3, B.CreateFCmpOEQ(S, B_));
}

void X86LiftContext::x87FComI(Value *V) {
  Value *S = x87St(0);
  Value *B_ = toF80(B, Ctx, V);
  Value *Un = B.CreateFCmpUNO(S, B_);
  flagWrite(ZF, B.CreateOr(B.CreateFCmpOEQ(S, B_), Un));
  flagWrite(PF, Un);
  flagWrite(CF, B.CreateOr(B.CreateFCmpOLT(S, B_), Un));
  flagWrite(OF, B.getFalse());
  flagWrite(SF, B.getFalse());
  flagWrite(AF, B.getFalse());
}

Value *X86LiftContext::x87StatusWord() {
  ensureX87();
  Value *Top = B.CreateZExt(B.CreateLoad(iN(8), X87Top), iN(16));
  Value *W = B.CreateShl(Top, constI(11, 16));
  auto Put = [&](unsigned Bit, unsigned C) {
    W = B.CreateOr(W, B.CreateShl(B.CreateZExt(x87CFlag(C), iN(16)),
                                  constI(Bit, 16)));
  };
  Put(8, 0);
  Put(9, 1);
  Put(10, 2);
  Put(14, 3);
  return W;
}

void X86LiftContext::x87SetStatusWord(Value *V) {
  ensureX87();
  Value *W = zextOrTrunc(V, iN(16));
  B.CreateStore(B.CreateTrunc(B.CreateLShr(W, constI(11, 16)), iN(8)),
                X87Top);
  x87SetCFlag(0, lowBit(B, W, 8));
  x87SetCFlag(1, lowBit(B, W, 9));
  x87SetCFlag(2, lowBit(B, W, 10));
  x87SetCFlag(3, lowBit(B, W, 14));
}

void X86LiftContext::x87SetControlWord(Value *V) {
  ensureX87();
  B.CreateStore(B.CreateTrunc(V, iN(16)), X87CW);
}

Value *X86LiftContext::x87ControlWord() {
  ensureX87();
  return B.CreateLoad(iN(16), X87CW);
}

Value *X86LiftContext::x87Const(StringRef Name) {
  // x87 sign/exp is the high 16 bits of the 80-bit image.
  struct Ent {
    const char *N;
    uint64_t Hi;
    uint64_t Lo;
  };
  static const Ent Tab[] = {
      {"0", 0x0000, 0x0},
      {"z", 0x8000, 0x0},
      {"1", 0x3FFF, 0x8000000000000000ULL},
      {"pi", 0x4000, 0xC90FDAA22168C235ULL},
      {"l2t", 0x4000, 0xD49A784BCD1B8AFEULL},
      {"l2e", 0x3FFF, 0xB8AA3B295C17F0BCULL},
      {"lg2", 0x3FFD, 0x9A209A84FBCFF799ULL},
      {"ln2", 0x3FFE, 0xB17217F7D1CF79ACULL},
  };
  for (const Ent &E : Tab)
    if (Name == E.N) {
      APInt Bits(80, {E.Lo, E.Hi});
      return ConstantFP::get(
          Ctx, APFloat(APFloat::x87DoubleExtended(), Bits));
    }
  return UndefValue::get(Type::getX86_FP80Ty(Ctx));
}

Value *X86LiftContext::x87Math(StringRef Name, Value *A, Value *B_) {
  Type *F80 = Type::getX86_FP80Ty(Ctx);
  Value *Fa = toF80(B, Ctx, A);
  if (Name == "sqrt")
    return B.CreateIntrinsic(Intrinsic::sqrt, {F80}, {Fa});
  if (Name == "sin")
    return B.CreateIntrinsic(Intrinsic::sin, {F80}, {Fa});
  if (Name == "cos")
    return B.CreateIntrinsic(Intrinsic::cos, {F80}, {Fa});
  if (Name == "abs")
    return B.CreateIntrinsic(Intrinsic::fabs, {F80}, {Fa});
  if (Name == "chs")
    return B.CreateFNeg(Fa);
  if (Name == "rnd")
    return B.CreateIntrinsic(Intrinsic::rint, {F80}, {Fa});
  if (B_) {
    Value *Fb = toF80(B, Ctx, B_);
    if (Name == "add")
      return B.CreateFAdd(Fa, Fb);
    if (Name == "sub")
      return B.CreateFSub(Fa, Fb);
    if (Name == "mul")
      return B.CreateFMul(Fa, Fb);
    if (Name == "div")
      return B.CreateFDiv(Fa, Fb);
    if (Name == "prem")
      return B.CreateFRem(Fa, Fb);
    if (Name == "scale")
      return B.CreateFMul(
          Fa, B.CreateIntrinsic(
                  Intrinsic::exp2, {F80},
                  {B.CreateIntrinsic(Intrinsic::floor, {F80}, {Fb})}));
  }
  return UndefValue::get(F80);
}

void X86LiftContext::x87Env(StringRef Dir, Value *Addr) {
  // Only the control/status words map onto modeled state; the ip/opcode/tag
  // fields are opaque to us either direction.
  if (Dir == "st") {
    memWriteAddr(Addr, x87ControlWord());
    memWriteAddr(B.CreateAdd(Addr, constI(2, 64)), x87StatusWord());
  } else {
    x87SetControlWord(memReadAddr(Addr, iN(16)));
    x87SetStatusWord(memReadAddr(B.CreateAdd(Addr, constI(2, 64)), iN(16)));
  }
}

unsigned X86LiftContext::x87IndexOf(MCRegister Reg) const {
  // ST0..ST7 are consecutive enum values.
  if (Reg >= X86::ST0 && Reg <= X86::ST7)
    return Reg - X86::ST0;
  return 0;
}

void X86LiftContext::x87Xchg(unsigned I) {
  Value *A = x87St(0);
  Value *Bv = x87St(I);
  x87StSet(0, Bv);
  x87StSet(I, A);
}
