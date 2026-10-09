//===- LiftCore.cpp - hand-lifted core integer ops --------------------===//
//
// mov/lea/xchg, add/sub/adc/sbb, neg/not/inc/dec, and/or/xor, cmp/test,
// setcc/cmovcc, push/pop. Mirrors crates/lift intops.rs + bitops.rs against
// the MCInst logical-operand view. Anything else returns false and the
// caller falls back to the opaque revenant.asm.<mnemonic> call.
//
//===----------------------------------------------------------------------===//

#include "LiftState.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"

#include "X86MCTargetDesc.h"
#include "X86BaseInfo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"

namespace revenant::lift {

namespace {

/// TableGen opcode name for I ("ADD64rr", "JNE_1", ...).
llvm::StringRef opNameOf(const Instr &I, const LiftState &LS) {
  return LS.MC.MII->getName(I.Inst.getOpcode());
}

/// First logical operand's bit width.
unsigned dstBits(const Instr &I, LiftState &LS) {
  return LS.opBits(I, 0);
}

} // namespace

bool LiftState::liftMov(const Instr &I) {
  auto Ops = opsOf(I);
  if (Ops.empty() || Ops[0].Kind == LogOpKind::Invalid)
    return false;
  llvm::StringRef N = opNameOf(I, *this);
  unsigned Bits = opBits(I, 0);
  llvm::Value *V = opReadInt(I, 1, Bits);
  // MOVSX/MOVSXD sign-extend from the source width; plain MOV zero-extends
  // into the destination width.
  if (N.contains("MOVSX")) {
    unsigned SrcBits = opBits(I, 1);
    V = sextTo(opReadInt(I, 1, SrcBits), Bits);
  }
  opWriteInt(I, 0, V);
  return true;
}

bool LiftState::liftLea(const Instr &I) {
  auto Ops = opsOf(I);
  if (Ops.size() < 2 || Ops[0].Kind != LogOpKind::Reg ||
      Ops[1].Kind != LogOpKind::Mem)
    return false;
  MemOp M = opMem(I, 1);
  llvm::Value *Addr = nullptr;
  if (M.Base == llvm::X86::RIP) {
    Va Target = I.HasMemTarget ? I.MemTarget : I.nextVa().wrappingAdd(M.Disp);
    Addr = B.CreatePtrToInt(globalPtr(Target), i64(), "la");
  } else if ((M.Base == llvm::X86::RSP || M.Base == llvm::X86::ESP) &&
             M.Index == llvm::X86::NoRegister) {
    int64_t Off = Stack.rspOff(CurVa, M.Disp);
    Addr = B.CreatePtrToInt(framePtrOff(Off), i64(), "la");
  } else if ((M.Base == llvm::X86::RBP || M.Base == llvm::X86::EBP) &&
             M.Index == llvm::X86::NoRegister) {
    if (auto Off = Stack.rbpOff(M.Disp))
      Addr = B.CreatePtrToInt(framePtrOff(*Off), i64(), "la");
  }
  if (!Addr) {
    llvm::Value *A =
        M.Base == llvm::X86::NoRegister ? ci(0) : extTo(regRead(M.Base), 64);
    if (M.Index != llvm::X86::NoRegister) {
      llvm::Value *Iv = extTo(regRead(M.Index), 64);
      llvm::Value *Sc = B.CreateMul(sextTo(Iv, 64), ci((uint64_t)M.Scale), "s");
      A = B.CreateAdd(A, Sc, "a");
    }
    if (M.Disp)
      A = B.CreateAdd(A, ci((uint64_t)M.Disp), "d");
    Addr = A;
  }
  llvm::MCRegister Dst = opReg(I, 0);
  auto [Off, Bits] = regOffsetBits(Dst, *MC.MRI);
  (void)Off;
  regWrite(Dst, extTo(Addr, Bits));
  return true;
}

bool LiftState::liftXchg(const Instr &I) {
  auto Ops = opsOf(I);
  if (Ops.size() < 2)
    return false;
  unsigned Bits = opBits(I, 0);
  llvm::Value *A = opReadInt(I, 0, Bits);
  llvm::Value *Bv = opReadInt(I, 1, Bits);
  opWriteInt(I, 0, Bv);
  opWriteInt(I, 1, A);
  return true;
}

bool LiftState::liftAddSub(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  bool IsAdd = N.starts_with("ADD") || N.starts_with("ADC");
  bool IsSub = N.starts_with("SUB") || N.starts_with("SBB");
  bool WithCarry = N.starts_with("ADC") || N.starts_with("SBB");
  if (!IsAdd && !IsSub)
    return false;
  unsigned Bits = dstBits(I, *this);
  llvm::Value *A = opReadInt(I, 0, Bits);
  llvm::Value *Bv = opReadInt(I, 1, Bits);
  llvm::Value *Res = nullptr;
  if (IsAdd) {
    Res = B.CreateAdd(A, Bv, "add");
    if (WithCarry)
      Res = B.CreateAdd(Res, extTo(flagRead("cf"), Bits), "adc");
  } else {
    Res = B.CreateSub(A, Bv, "sub");
    if (WithCarry)
      Res = B.CreateSub(Res, extTo(flagRead("cf"), Bits), "sbb");
  }
  opWriteInt(I, 0, Res);
  produce(FlagProd{FlagProd::Arith, A, Bv, Res, IsAdd ? 'a' : 's'});
  return true;
}

bool LiftState::liftUnary(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  bool IsNeg = N.starts_with("NEG");
  bool IsNot = N.starts_with("NOT");
  bool IsInc = N.starts_with("INC");
  bool IsDec = N.starts_with("DEC");
  if (!IsNeg && !IsNot && !IsInc && !IsDec)
    return false;
  unsigned Bits = dstBits(I, *this);
  llvm::Value *A = opReadInt(I, 0, Bits);
  llvm::Value *Res = nullptr;
  char Mn = 'l';
  if (IsNeg) {
    Res = B.CreateNeg(A, "n");
    Mn = 'n';
  } else if (IsNot) {
    Res = B.CreateNot(A, "n");
  } else if (IsInc) {
    Res = B.CreateAdd(A, ci(1, Bits), "i");
    Mn = 'i';
  } else {
    Res = B.CreateSub(A, ci(1, Bits), "d");
    Mn = 'd';
  }
  opWriteInt(I, 0, Res);
  if (!IsNot)
    produce(FlagProd{FlagProd::Arith, A, nullptr, Res, Mn});
  else
    Prod.reset();
  return true;
}

bool LiftState::liftLogic(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  bool IsAnd = N.starts_with("AND");
  bool IsOr = N.starts_with("OR");
  bool IsXor = N.starts_with("XOR");
  if (!IsAnd && !IsOr && !IsXor)
    return false;
  unsigned Bits = dstBits(I, *this);
  llvm::Value *A = opReadInt(I, 0, Bits);
  llvm::Value *Bv = opReadInt(I, 1, Bits);
  llvm::Value *Res = IsAnd ? B.CreateAnd(A, Bv, "a")
                     : IsOr ? B.CreateOr(A, Bv, "o")
                            : B.CreateXor(A, Bv, "x");
  opWriteInt(I, 0, Res);
  produce(FlagProd{FlagProd::Arith, A, Bv, Res, 'l'});
  return true;
}

bool LiftState::liftCmp(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  bool IsTest = N.starts_with("TEST");
  bool IsCmp = N.starts_with("CMP");
  if (!IsTest && !IsCmp)
    return false;
  unsigned Bits = dstBits(I, *this);
  llvm::Value *A = opReadInt(I, 0, Bits);
  llvm::Value *Bv = opReadInt(I, 1, Bits);
  if (IsTest) {
    llvm::Value *Res = B.CreateAnd(A, Bv, "t");
    produce(FlagProd{FlagProd::Arith, A, Bv, Res, 'l'});
  } else {
    produce(FlagProd{FlagProd::Cmp, A, Bv, nullptr, 's'});
  }
  return true;
}

bool LiftState::liftPush(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  if (!N.starts_with("PUSH"))
    return false;
  auto Ops = opsOf(I);
  if (Ops.empty())
    return false;
  llvm::Value *V = opReadInt(I, 0, 64);
  pushVal(V, I);
  return true;
}

bool LiftState::liftPop(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  if (!N.starts_with("POP"))
    return false;
  auto Ops = opsOf(I);
  if (Ops.empty())
    return false;
  llvm::Value *V = popVal(I);
  opWriteInt(I, 0, extTo(V, opBits(I, 0)));
  return true;
}

/// SETcc/CMOVcc/Jcc condition codes. MC encodes the cc as an immediate
/// operand on SETcc/CMOVcc/Jcc (X86 OPERAND_COND_CODE); the immediate is the
/// X86::CondCode value (O=0..G=15) that condFor consumes.
static bool condCodeOf(const Instr &I, const LiftState &LS, unsigned &CC) {
  auto Ops = LS.opsOf(I);
  for (unsigned L = 0; L < Ops.size(); ++L) {
    if (Ops[L].Kind != LogOpKind::Imm)
      continue;
    const llvm::MCOperand &O = I.Inst.getOperand(Ops[L].MI);
    if (!O.isImm())
      continue;
    int64_t V = O.getImm();
    if (V >= 0 && V <= 15) {
      CC = (unsigned)V;
      return true;
    }
  }
  return false;
}

bool LiftState::liftCore(const Instr &I) {
  llvm::StringRef N = opNameOf(I, *this);
  // MOV family (MOV, MOVSX, MOVZX, MOVSXD). Vector/string forms (MOVS*,
  // MOVD/Q, MOVAP*, MOVDQ*) stay opaque until LunaLiftGen2's table lands.
  if (N.starts_with("MOV") && !N.starts_with("MOVS") &&
      !N.starts_with("MOVD") && !N.starts_with("MOVQ") &&
      !N.starts_with("MOVAP") && !N.starts_with("MOVUP") &&
      !N.starts_with("MOVDQ"))
    return liftMov(I);
  if (N.starts_with("LEA"))
    return liftLea(I);
  if (N.starts_with("XCHG"))
    return liftXchg(I);
  if (N.starts_with("ADD") || N.starts_with("SUB") || N.starts_with("ADC") ||
      N.starts_with("SBB"))
    return liftAddSub(I);
  if (N.starts_with("NEG") || N.starts_with("NOT") || N.starts_with("INC") ||
      N.starts_with("DEC"))
    return liftUnary(I);
  if (N.starts_with("AND") || N.starts_with("OR") || N.starts_with("XOR"))
    return liftLogic(I);
  if (N.starts_with("CMP") || N.starts_with("TEST"))
    return liftCmp(I);
  if (N.starts_with("PUSH"))
    return liftPush(I);
  if (N.starts_with("POP"))
    return liftPop(I);
  // SETcc: i8 = zext(cond).
  if (N.starts_with("SET")) {
    unsigned CC = 0;
    if (!condCodeOf(I, *this, CC))
      return false;
    llvm::Value *Cond = condFor(CC, I.Addr);
    opWriteInt(I, 0, B.CreateZExt(Cond, iN(8), "z"));
    Prod.reset();
    return true;
  }
  // CMOVcc: dst = cond ? src : dst.
  if (N.starts_with("CMOV")) {
    unsigned CC = 0;
    if (!condCodeOf(I, *this, CC))
      return false;
    llvm::Value *Cond = condFor(CC, I.Addr);
    unsigned Bits = dstBits(I, *this);
    llvm::Value *A = opReadInt(I, 0, Bits);
    llvm::Value *Bv = opReadInt(I, 1, Bits);
    opWriteInt(I, 0, B.CreateSelect(Cond, Bv, A, "cm"));
    Prod.reset();
    return true;
  }
  // Sign extensions: CBW/CWDE/CDQE/CWD/CDQ/CQO.
  if (N == "CBW") {
    regWrite(llvm::X86::AX, sextTo(regRead(llvm::X86::AL), 16));
    return true;
  }
  if (N == "CWDE") {
    regWrite(llvm::X86::EAX, sextTo(regRead(llvm::X86::AX), 32));
    return true;
  }
  if (N == "CDQE") {
    regWrite(llvm::X86::RAX, sextTo(regRead(llvm::X86::EAX), 64));
    return true;
  }
  if (N == "CDQ" || N == "CWD") {
    llvm::Value *V = regRead(llvm::X86::EAX);
    regWrite(llvm::X86::EDX, B.CreateAShr(V, ci(31, 32), "h"));
    return true;
  }
  if (N == "CQO") {
    llvm::Value *V = regRead(llvm::X86::RAX);
    regWrite(llvm::X86::RDX, B.CreateAShr(V, ci(63), "h"));
    return true;
  }
  // BSWAP.
  if (N.starts_with("BSWAP")) {
    unsigned Bits = dstBits(I, *this);
    llvm::Value *V = opReadInt(I, 0, Bits);
    if (llvm::Function *F = llvmIntrinsic("llvm.bswap", {iN(Bits)}))
      V = B.CreateCall(F, {V}, "bs");
    opWriteInt(I, 0, V);
    return true;
  }
  // NOP / hints / fences: no semantics, keep provenance only.
  if (N.starts_with("NOOP") || N == "NOP" || N.starts_with("HINT") ||
      N.starts_with("ENDBR") || N == "PAUSE" || N == "LFENCE" ||
      N == "MFENCE" || N == "SFENCE") {
    Prod.reset();
    return true;
  }
  return false;
}

llvm::Error LiftState::liftInstr(const Instr &I) {
  setVA(I.Addr);
  // A non-constant rsp point becomes the dynamic alloca.
  if (Stack.DynVA && *Stack.DynVA == I.Addr) {
    emitDynAlloca(I);
    return llvm::Error::success();
  }
  if (liftCore(I))
    return llvm::Error::success();
  // Calls are terminators here; mid-block calls still lower through emitCall
  // when LiftFunction routes them.
  if (I.Flow == FlowKind::Call) {
    emitCall(I);
    return llvm::Error::success();
  }
  liftOpaque(I);
  return llvm::Error::success();
}

void LiftState::liftOpaque(const Instr &I) {
  setVA(I.Addr);
  std::string Mn = mnemonicOf(I, D);
  // Preserve the reads the instruction may consume: pass the current value
  // of each explicit register/memory operand as i64.
  auto Ops = opsOf(I);
  llvm::SmallVector<llvm::Value *, 4> Args;
  for (unsigned L = 0; L < Ops.size() && Args.size() < 4; ++L) {
    switch (Ops[L].Kind) {
    case LogOpKind::Reg:
      Args.push_back(
          extTo(regRead(I.Inst.getOperand(Ops[L].MI).getReg()), 64));
      break;
    case LogOpKind::Mem:
      Args.push_back(memRead(I, L, 64));
      break;
    case LogOpKind::Imm:
      Args.push_back(ci((uint64_t)opImm(I, L)));
      break;
    default:
      break;
    }
  }
  llvm::SmallVector<llvm::Type *, 4> ArgTys;
  for (llvm::Value *A : Args)
    ArgTys.push_back(A->getType());
  llvm::Function *Stub = ir::getOrCreateOpaqueAsmFn(
      Mod, Mn, llvm::Type::getVoidTy(Mod.getContext()), ArgTys);
  llvm::CallInst *Call = B.CreateCall(Stub, Args);
  Call->setMetadata(ir::MDKindOpaque,
                    llvm::MDNode::get(Mod.getContext(), {}));
  ir::setInstructionVA(*Call, I.Addr.Value);
  Fallbacks.push_back({I.Addr, Mn + " (" + D.format(I, P.Resolver) + ")"});
  // Opaque instructions clobber flags; volatile regs become unknown like
  // after a call (the Rust opaque writes back used regs, this keeps the
  // conservative half until the generator lands).
  clobberVolatile();
  Prod.reset();
}

} // namespace revenant::lift
