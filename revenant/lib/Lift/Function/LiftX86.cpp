//===- LiftX86.cpp - generated x86 lift table on top of LiftState ----------===//
//
// See LiftX86.h. Every hook forwards to LiftState where the function lifter
// already owns the state (GPRs, low xmm lane, the five tracked flags, rsp,
// memory addressing) and falls through to X86LiftContext for the rest.
//
//===----------------------------------------------------------------------===//

#include "LiftX86.h"

#include "X86BaseInfo.h"
#include "X86MCTargetDesc.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/PrettyStackTrace.h"

using namespace llvm;

namespace llvm::revenant {
bool liftX86Inst(X86LiftContext &LC, const MCInst &MI, uint64_t VA,
                 uint64_t Size);
} // namespace llvm::revenant

namespace revenant::lift {

namespace {

constexpr const char *LiftFlagName[] = {"cf", "zf", "sf", "of", "pf"};

/// Crash-time context: names the decoded instruction and VA so an abort in a
/// generated body or the hand-lifted path points at one instruction.
class LiftInsnTrace final : public llvm::PrettyStackTraceEntry {
  const llvm::MCInst &MI;
  const llvm::MCInstrInfo &MII;
  uint64_t VA;

public:
  LiftInsnTrace(const llvm::MCInst &MI, const llvm::MCInstrInfo &MII,
                uint64_t VA)
      : MI(MI), MII(MII), VA(VA) {}

  void print(llvm::raw_ostream &OS) const override {
    OS << "insn=" << MII.getName(MI.getOpcode()) << " va=0x"
       << llvm::format_hex(VA, 0);
  }
};

/// The five EFLAGS bits LiftState keeps as allocas (AF and DF live in the
/// X86LiftContext base).
bool tracked(llvm::revenant::X86LiftContext::FlagId F) {
  return F <= llvm::revenant::X86LiftContext::PF;
}

bool isRspFamily(MCRegister R) {
  return R == X86::RSP || R == X86::ESP || R == X86::SP || R == X86::SPL;
}

/// Vector register slot (0-15) and kind, or ~0u for xmm16+/zmm.
struct VecReg {
  unsigned Index = ~0u;
  unsigned Bits = 0;
};

VecReg vecRegOf(MCRegister R) {
  if (X86II::isXMMReg(R)) {
    if (R >= X86::XMM0 && R <= X86::XMM15)
      return {R - X86::XMM0, 128};
    return {~0u, 128};
  }
  if (X86II::isYMMReg(R)) {
    if (R >= X86::YMM0 && R <= X86::YMM15)
      return {R - X86::YMM0, 256};
    return {~0u, 256};
  }
  if (X86II::isZMMReg(R))
    return {~0u, 512};
  return {};
}

bool isPrefixOpcode(unsigned Opc) {
  return Opc == X86::LOCK_PREFIX || Opc == X86::REP_PREFIX ||
         Opc == X86::REPNE_PREFIX || Opc == X86::XACQUIRE_PREFIX ||
         Opc == X86::XRELEASE_PREFIX || Opc == X86::REX64_PREFIX ||
         Opc == X86::DATA16_PREFIX || Opc == X86::CS_PREFIX ||
         Opc == X86::DS_PREFIX || Opc == X86::ES_PREFIX ||
         Opc == X86::FS_PREFIX || Opc == X86::GS_PREFIX ||
         Opc == X86::SS_PREFIX;
}

} // namespace

GenContext::GenContext(LiftState &LS)
    : llvm::revenant::X86LiftContext(LS.Mod.getContext(), LS.Mod, LS.Fn,
                                     *LS.MC.MII, *LS.MC.MRI),
      LS(LS) {
  for (const Instr &I : LS.Instrs)
    for (const MCOperand &Op : I.Inst)
      if (Op.isReg() && X86II::isYMMReg(Op.getReg()))
        UsesYmm = true;
}

bool GenContext::ownsScalar(MCRegister R) {
  return isRspFamily(R) || isTrackedGpr(R);
}

AllocaInst *GenContext::ymmHi(unsigned N) {
  if (!YmmHi[N])
    YmmHi[N] = regSlot("ymm" + std::to_string(N) + "h", iN(128));
  return YmmHi[N];
}

bool GenContext::lift(const Instr &I) {
  llvm::BasicBlock *BB = LS.B.GetInsertBlock();
  Instruction *Before = BB->empty() ? nullptr : &BB->back();
  std::optional<FlagProd> SavedProd = LS.Prod;
  B.SetInsertPoint(BB);
  Cur = &I;
  Opaque = false;
  PtrOf.clear();

  LiftInsnTrace Trace(I.Inst, *LS.MC.MII, I.Addr.Value);
  // The disassembler folds an F0 prefix into the locked instruction (flag
  // IP_HAS_LOCK) instead of decoding LOCK_PREFIX on its own, so the lock
  // reaches the table here.
  if (I.Inst.getFlags() & X86::IP_HAS_LOCK)
    prefixOp("lock", MCRegister());

  bool Handled = llvm::revenant::liftX86Inst(*this, I.Inst, I.Addr.Value,
                                             I.Size);
  Flows.clear();
  if (!Handled || Opaque) {
    // Roll back to the state before the table ran; the hand-lifted path or
    // the opaque fallback takes over.
    while (!BB->empty() && &BB->back() != Before) {
      Instruction &Dead = BB->back();
      Dead.replaceAllUsesWith(PoisonValue::get(Dead.getType()));
      Dead.eraseFromParent();
    }
    LS.Prod = SavedProd;
    consumePrefixes();
    Cur = nullptr;
    return false;
  }
  if (!isPrefixOpcode(I.Inst.getOpcode()))
    consumePrefixes();
  LS.B.SetInsertPoint(BB);
  Cur = nullptr;
  return true;
}

// ---- LiftContext ------------------------------------------------------------

Value *GenContext::regRead(MCRegister R, Type *Ty) {
  if (R == X86::RIP || R == X86::EIP)
    return X86LiftContext::regRead(R, Ty);
  if (ownsScalar(R))
    return fromInt(LS.regRead(R), Ty);
  VecReg V = vecRegOf(R);
  if (V.Bits) {
    if (V.Index == ~0u || V.Bits == 512) {
      Opaque = true;
      return UndefValue::get(Ty);
    }
    Value *Lo = LS.regRead(R);
    if (V.Bits == 128)
      return fromInt(Lo, Ty);
    Value *Hi = B.CreateLoad(iN(128), ymmHi(V.Index));
    Value *Wide = B.CreateOr(
        B.CreateZExt(Lo, iN(256)),
        B.CreateShl(B.CreateZExt(Hi, iN(256)), constI(128, 256)));
    return fromInt(Wide, Ty);
  }
  return X86LiftContext::regRead(R, Ty);
}

llvm::revenant::LiftContext::OpKind GenContext::opKind(const MCInst &MI,
                                                       unsigned MIOpNo) const {
  llvm::revenant::LiftContext::OpKind K = X86LiftContext::opKind(MI, MIOpNo);
  // A table case that reaches past the decoded operands is wrong for this
  // form; the instruction goes to the fallback path.
  if (K == llvm::revenant::LiftContext::OpKind::Invalid)
    Opaque = true;
  return K;
}

void GenContext::regWrite(MCRegister R, Value *V) {
  if (R == X86::RIP || R == X86::EIP)
    return;
  if (ownsScalar(R)) {
    unsigned Bits = lift::regOffsetBits(R, *LS.MC.MRI).second;
    LS.regWrite(R, toInt(V, Bits));
    return;
  }
  VecReg Vec = vecRegOf(R);
  if (Vec.Bits) {
    if (Vec.Index == ~0u || Vec.Bits == 512) {
      Opaque = true;
      return;
    }
    if (Vec.Bits == 128) {
      LS.regWrite(R, toInt(V, 128));
      return;
    }
    Value *Wide = toInt(V, 256);
    LS.regWrite(R, B.CreateTrunc(Wide, iN(128)));
    B.CreateStore(B.CreateTrunc(B.CreateLShr(Wide, constI(128, 256)), iN(128)),
                  ymmHi(Vec.Index));
    return;
  }
  X86LiftContext::regWrite(R, V);
}

void GenContext::regWriteZ(MCRegister R, Value *V) {
  if (ownsScalar(R)) {
    auto [Off, Bits] = lift::regOffsetBits(R, *LS.MC.MRI);
    MCRegister Root = rootRegOf(R, *LS.MC.MRI);
    if (Off == 0 && Root.isValid() && Root != R)
      LS.regWrite(Root, B.CreateZExt(toInt(V, Bits), iN(64)));
    else
      regWrite(R, V);
    return;
  }
  VecReg Vec = vecRegOf(R);
  if (Vec.Bits == 128 && Vec.Index != ~0u) {
    // VEX.128 writes clear the upper ymm lane.
    LS.regWrite(R, toInt(V, 128));
    if (UsesYmm)
      B.CreateStore(constI(0, 128), ymmHi(Vec.Index));
    return;
  }
  regWrite(R, V);
}

Value *GenContext::memAddr(const MCInst &MI, unsigned MIOpNo) {
  if (Cur && &MI == &Cur->Inst) {
    std::vector<LogOp> Ops = LS.opsOf(*Cur);
    for (unsigned L = 0; L < Ops.size(); ++L) {
      if (Ops[L].MI != MIOpNo || Ops[L].Kind != LogOpKind::Mem)
        continue;
      Value *P = LS.memPtr(*Cur, L);
      Value *Addr;
      if (auto *ITP = dyn_cast<IntToPtrInst>(P)) {
        Addr = ITP->getOperand(0);
        if (ITP->use_empty())
          ITP->eraseFromParent();
      } else {
        Addr = B.CreatePtrToInt(P, iN(64));
        PtrOf[Addr] = P;
      }
      MCRegister Seg = LS.opMem(*Cur, L).Seg;
      if (Seg == X86::FS || Seg == X86::GS) {
        std::string Name = (Seg == X86::FS) ? "fs" : "gs";
        FunctionCallee F = Mod.getOrInsertFunction(
            "revenant.segbase." + Name, FunctionType::get(iN(64), false));
        Addr = B.CreateAdd(Addr, B.CreateCall(F));
      }
      return Addr;
    }
  }
  return X86LiftContext::memAddr(MI, MIOpNo);
}

Value *GenContext::addrToPtr(Value *AddrI64) {
  auto It = PtrOf.find(AddrI64);
  if (It != PtrOf.end())
    return It->second;
  return X86LiftContext::addrToPtr(AddrI64);
}

void GenContext::emitOpaque(const MCInst &, StringRef) { Opaque = true; }

// ---- flags --------------------------------------------------------------------

Value *GenContext::flagRead(FlagId F) {
  if (tracked(F))
    return LS.flagRead(LiftFlagName[F]);
  return X86LiftContext::flagRead(F);
}

void GenContext::flagWrite(FlagId F, Value *I1) {
  if (tracked(F)) {
    LS.Prod.reset();
    LS.flagWrite(LiftFlagName[F], I1);
    return;
  }
  X86LiftContext::flagWrite(F, I1);
}

Value *GenContext::condPred(unsigned CC) {
  return LS.condFor(CC, Cur ? Cur->Addr : Va());
}

Value *GenContext::flagsAdd(Value *A, Value *B_) {
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *R = B.CreateAdd(A, B2);
  LS.produce(FlagProd{FlagProd::Arith, A, B2, R, 'a'});
  return R;
}

Value *GenContext::flagsSub(Value *A, Value *B_) {
  Value *B2 = zextOrTrunc(B_, A->getType());
  Value *R = B.CreateSub(A, B2);
  LS.produce(FlagProd{FlagProd::Arith, A, B2, R, 's'});
  return R;
}

Value *GenContext::flagsCmp(Value *A, Value *B_) {
  Value *B2 = zextOrTrunc(B_, A->getType());
  LS.produce(FlagProd{FlagProd::Cmp, A, B2, nullptr, 's'});
  return UndefValue::get(A->getType());
}

Value *GenContext::flagsLogic(Value *R) {
  LS.produce(FlagProd{FlagProd::Arith, nullptr, nullptr, R, 'l'});
  return R;
}

Value *GenContext::flagsNeg(Value *A) {
  // NEG is 0 - A, so the flags are those of that subtraction.
  Value *Zero = ConstantInt::get(A->getType(), 0);
  Value *R = B.CreateNeg(A);
  LS.produce(FlagProd{FlagProd::Arith, Zero, A, R, 's'});
  return R;
}

Value *GenContext::flagsInc(Value *A) {
  Value *R = B.CreateAdd(A, ConstantInt::get(A->getType(), 1));
  Value *Cf = LS.flagRead("cf");
  LS.produce(FlagProd{FlagProd::Arith, A, nullptr, R, 'i'});
  LS.flagWrite("cf", Cf); // INC preserves CF
  return R;
}

Value *GenContext::flagsDec(Value *A) {
  Value *R = B.CreateSub(A, ConstantInt::get(A->getType(), 1));
  Value *Cf = LS.flagRead("cf");
  LS.produce(FlagProd{FlagProd::Arith, A, nullptr, R, 'd'});
  LS.flagWrite("cf", Cf); // DEC preserves CF
  return R;
}

// ---- stack ----------------------------------------------------------------------

void GenContext::pushVal(Value *V) {
  if (V->getType()->getPrimitiveSizeInBits() != 64) {
    Opaque = true;
    return;
  }
  LS.pushVal(toInt(V, 64), *Cur);
}

Value *GenContext::popVal(unsigned Width) {
  if (Width != 64) {
    Opaque = true;
    return UndefValue::get(iN(Width));
  }
  return LS.popVal(*Cur);
}

void GenContext::leaveOp() {
  // rsp = rbp; rbp = [rsp]: the saved rbp sits where rbp points.
  Value *Slot;
  if (auto Off = LS.Stack.rbpOff(0))
    Slot = LS.framePtrOff(*Off);
  else
    Slot = B.CreateIntToPtr(LS.regRead(X86::RBP), LS.ptrTy());
  LS.regWrite(X86::RBP, B.CreateAlignedLoad(iN(64), Slot, Align(1)));
}

} // namespace revenant::lift
