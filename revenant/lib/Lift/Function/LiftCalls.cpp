//===- LiftCalls.cpp - call lowering for the function lifter ---------===//
//
// Direct, import and indirect calls with Win64 argument passing, tail calls
// through the caller's return shape, and callee declarations typed from the
// recovered Signature (LunaSigs; until then the Rust default of four i64
// registers + i64 return). Mirrors crates/lift calls.rs.
//
//===----------------------------------------------------------------------===//

#include "LiftState.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"

#include "X86MCTargetDesc.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

namespace revenant::lift {

llvm::FunctionType *LiftState::fnType(const Signature *S) {
  llvm::SmallVector<llvm::Type *, 8> Params;
  if (S)
    for (const Param &P : S->Params)
      Params.push_back(P.Float
                           ? (P.Bits == 32
                                  ? llvm::Type::getFloatTy(Mod.getContext())
                                  : llvm::Type::getDoubleTy(Mod.getContext()))
                           : llvm::Type::getIntNTy(Mod.getContext(),
                                                   std::max(1u, P.Bits)));
  llvm::Type *Ret = llvm::Type::getVoidTy(Mod.getContext());
  if (S && S->Ret)
    Ret = S->Ret->Float
              ? (S->Ret->Bits == 32
                     ? llvm::Type::getFloatTy(Mod.getContext())
                     : llvm::Type::getDoubleTy(Mod.getContext()))
              : llvm::Type::getIntNTy(Mod.getContext(),
                                      std::max(1u, S->Ret->Bits));
  return llvm::FunctionType::get(Ret, Params, false);
}

static const llvm::MCRegister Win64Regs[4] = {llvm::X86::RCX, llvm::X86::RDX,
                                              llvm::X86::R8, llvm::X86::R9};
/// VA stored in a Control Flow Guard load-config slot, when the image
/// carries one. The lifter folds a `call [slot]`'s slot read to this
/// handler constant, so both forms must be recognized here.
static std::optional<Va> cfgHandler(const LiftState &LS,
                                    std::optional<Va> Slot) {
  if (!Slot || !LS.P.Img)
    return std::nullopt;
  if (auto H = LS.P.Img->readU64(*Slot))
    return Va(*H);
  return std::nullopt;
}

/// `call`/`jmp` target forms that hit the CFG check or dispatch wrappers:
/// the slot VA itself (`call [slot]`, `jmp [slot]`) or the stored handler
/// (`call _guard_dispatch_icall_nop`).
static bool isGuardCheck(const LiftState &LS, const Instr &I,
                         std::optional<Va> Target) {
  if (!LS.P.GuardCFCheckPtr)
    return false;
  if (I.HasMemTarget && I.MemTarget == *LS.P.GuardCFCheckPtr)
    return true;
  std::optional<Va> H = cfgHandler(LS, LS.P.GuardCFCheckPtr);
  return Target && H && *Target == *H;
}

static bool isGuardDispatch(const LiftState &LS, const Instr &I,
                            std::optional<Va> Target) {
  if (!LS.P.GuardCFDispatchPtr)
    return false;
  if (I.HasMemTarget && I.MemTarget == *LS.P.GuardCFDispatchPtr)
    return true;
  std::optional<Va> H = cfgHandler(LS, LS.P.GuardCFDispatchPtr);
  return Target && H && *Target == *H;
}

/// `ret` shaped to the caller's recovered signature. `_guard_check_icall`
/// is a nop here: `call [check]` is dropped outright and `jmp [check]`
/// becomes this return.
static void emitGuardReturn(LiftState &LS, Va At) {
  const Signature *Sig = LS.Sigs ? LS.Sigs->of(LS.F.Start) : nullptr;
  if (!Sig || !Sig->Ret) {
    llvm::ReturnInst *RI = LS.B.CreateRetVoid();
    ir::setInstructionVA(*RI, At.Value);
    return;
  }
  llvm::Value *Ret;
  unsigned Bits = std::max(1u, Sig->Ret->Bits);
  if (Sig->Ret->Float) {
    Ret = LS.regRead(llvm::X86::XMM0);
    llvm::Type *Ty = Bits == 32
                         ? llvm::Type::getFloatTy(LS.Mod.getContext())
                         : llvm::Type::getDoubleTy(LS.Mod.getContext());
    Ret = LS.B.CreateBitCast(LS.extTo(Ret, Bits), Ty);
  } else {
    Ret = LS.extTo(LS.regRead(llvm::X86::RAX), Bits);
  }
  llvm::ReturnInst *RI = LS.B.CreateRet(Ret);
  ir::setInstructionVA(*RI, At.Value);
}

/// Shared tail of `emitIndirectTailCall`: `jmp Target` as a tail call,
/// returning the call's result in the caller's own shape.
static void emitIndirectTail(LiftState &LS, llvm::Value *TV, Va At) {
  const Signature *Sig = LS.Sigs ? LS.Sigs->of(LS.F.Start) : nullptr;
  Signature Empty;
  if (!Sig) {
    for (unsigned K = 0; K < 4; ++K)
      Empty.Params.push_back(
          {ParamLoc::reg(Win64Regs[K]), 64, false, std::nullopt});
    Empty.Ret = RetLoc{false, 64};
    Sig = &Empty;
  }
  llvm::FunctionType *FT = LS.fnType(Sig);
  std::vector<llvm::Value *> Args = LS.callArgs(Sig, std::nullopt);
  Args.resize(std::min<size_t>(Args.size(), FT->getNumParams()));
  while (Args.size() < FT->getNumParams())
    Args.push_back(llvm::UndefValue::get(LS.i64()));
  llvm::Value *FP =
      LS.B.CreateIntToPtr(TV, llvm::PointerType::getUnqual(FT), "fp");
  llvm::CallInst *Call = LS.B.CreateCall(
      FT, FP, Args, FT->getReturnType()->isVoidTy() ? "" : "itc");
  ir::setInstructionVA(*Call, At.Value);
  if (Call->getType()->isVoidTy())
    LS.B.CreateRetVoid();
  else
    LS.B.CreateRet(Call);
}

llvm::Function *LiftState::calleeFn(Va Target, const Signature *S) {
  std::string Name =
      "sub_" + llvm::formatv("{0:x}", Target.Value).str();
  if (P.Names)
    if (auto It = P.Names->find(Target); It != P.Names->end())
      Name = sanitizeName(It->second);
  if (llvm::Function *Old = Mod.getFunction(Name))
    return Old;
  const Signature *Sig = S;
  if (!Sig && Sigs)
    Sig = Sigs->of(Target);
  return llvm::cast<llvm::Function>(
      Mod.getOrInsertFunction(Name, fnType(Sig)).getCallee());
}

llvm::Function *LiftState::calleeByName(llvm::StringRef Name,
                                        const Signature *S) {
  std::string N = sanitizeName(Name);
  if (llvm::Function *Old = Mod.getFunction(N))
    return Old;
  return llvm::cast<llvm::Function>(
      Mod.getOrInsertFunction(N, fnType(S)).getCallee());
}

std::vector<llvm::Value *> LiftState::callArgs(const Signature *S,
                                              std::optional<size_t> Prefix) {
  std::vector<llvm::Value *> Args;
  if (S) {
    for (const Param &P : S->Params) {
      llvm::Value *V = nullptr;
      if (P.Loc.IsStack) {
        V = readCallerArg(P.Loc.StackOff);
        V = extTo(V, std::min(P.Bits, 64u));
      } else if (P.Float) {
        V = regRead(P.Loc.Reg);
        llvm::Type *Ty = P.Bits == 32
                             ? llvm::Type::getFloatTy(Mod.getContext())
                             : llvm::Type::getDoubleTy(Mod.getContext());
        V = B.CreateBitCast(extTo(V, P.Bits), Ty, "f");
      } else {
        V = extTo(regRead(P.Loc.Reg), std::min(P.Bits, 64u));
      }
      Args.push_back(V);
    }
    return Args;
  }
  size_t N = Prefix.value_or(4);
  for (size_t K = 0; K < N && K < 4; ++K)
    Args.push_back(regRead(Win64Regs[K]));
  return Args;
}

void LiftState::coerceArgs(llvm::Function *F,
                           std::vector<llvm::Value *> &Args) {
  unsigned Want = F->arg_size();
  Args.resize(std::min<size_t>(Args.size(), Want));
  while (Args.size() < Want)
    Args.push_back(llvm::UndefValue::get(i64()));
  unsigned K = 0;
  for (llvm::Argument &A : F->args()) {
    llvm::Value *V = Args[K];
    llvm::Type *Want2 = A.getType();
    if (V->getType() == Want2) {
      ++K;
      continue;
    }
    if (V->getType()->isIntegerTy() && Want2->isIntegerTy())
      V = B.CreateZExtOrTrunc(V, Want2, "pa");
    else if (V->getType()->isIntegerTy() && Want2->isFloatingPointTy())
      V = B.CreateBitCast(extTo(V, Want2->getPrimitiveSizeInBits()), Want2,
                          "pa");
    else if (V->getType()->isFloatingPointTy() && Want2->isIntegerTy())
      V = B.CreateBitCast(
          V,
          llvm::IntegerType::get(Mod.getContext(),
                                 V->getType()->getPrimitiveSizeInBits()),
          "pa0"),
      V = B.CreateZExtOrTrunc(V, Want2, "pa");
    else
      V = llvm::Constant::getNullValue(Want2);
    Args[K++] = V;
  }
}

llvm::Value *LiftState::readCallerArg(int64_t Off) {
  // Incoming stack args live above entry rsp: frame offset Off counts from
  // entry rsp, and Sp0 sits at entry rsp inside the frame.
  return B.CreateAlignedLoad(i64(), framePtrOff(Off), llvm::Align(1), "sa");
}

void LiftState::emitCall(const Instr &I) {
  setVA(I.Addr);
  Va IP = I.Addr;
  // Control Flow Guard wrappers, identified by the load-config slots rather
  // than symbol names. `call [check]` is validation-only: the check handler
  // is a nop and the real `call rcx` follows, so the call itself is
  // dropped. `call [dispatch]` is an indirect call through the target in
  // rax (`_guard_dispatch_icall` ends in `jmp rax`).
  if (isGuardCheck(*this, I, std::nullopt))
    return;
  if (isGuardDispatch(*this, I, std::nullopt) ||
      (I.HasTarget && isGuardDispatch(*this, I, I.Target))) {
    llvm::Value *Ptr = regRead(llvm::X86::RAX);
    size_t Prefix = 0;
    if (Sigs)
      if (auto It = Sigs->IndirectArgs.find(IP);
          It != Sigs->IndirectArgs.end())
        Prefix = It->second;
    Signature S;
    for (size_t K = 0; K < Prefix && K < 4; ++K)
      S.Params.push_back(
          {ParamLoc::reg(Win64Regs[K]), 64, false, std::nullopt});
    S.Ret = RetLoc{false, 64};
    llvm::FunctionType *FT = fnType(&S);
    llvm::Value *FP =
        B.CreateIntToPtr(Ptr, llvm::PointerType::getUnqual(FT), "fp");
    std::vector<llvm::Value *> Args = callArgs(&S, Prefix);
    llvm::CallInst *Call = B.CreateCall(FT, FP, Args, "ic");
    ir::setInstructionVA(*Call, I.Addr.Value);
    clobberVolatile();
    regWrite(llvm::X86::RAX, extTo(Call, 64));
    Prod.reset();
    return;
  }
  if (I.HasTarget && isGuardCheck(*this, I, I.Target)) {
    // `call _guard_check_icall_nop` (a ret): same as `call [check]`, drop.
    return;
  }
  // Import through `call qword [rip+slot]`: MemTarget is the IAT slot.
  if (I.HasMemTarget && P.Externs) {
    if (auto It = P.Externs->find(I.MemTarget); It != P.Externs->end()) {
      const Extern &E = It->second;
      std::string Sym = E.Name;
      if (auto Bang = Sym.find('!'); Bang != std::string::npos)
        Sym = Sym.substr(Bang + 1);
      if (auto Hash = Sym.find('#'); Hash != std::string::npos)
        Sym = Sym.substr(0, Hash);
      const Signature *Sig = nullptr;
      Signature Default;
      if (Sigs) {
        if (auto S2 = Sigs->Externs.find(I.MemTarget);
            S2 != Sigs->Externs.end())
          Sig = &S2->second;
      }
      if (!Sig) {
        Default = defaultCalleeSignature();
        if (E.Noreturn)
          Default.Ret.reset();
        Sig = &Default;
      }
      llvm::Function *Callee = calleeByName(Sym, Sig);
      // Imports carry the IAT slot VA per the contract.
      Callee->addFnAttr(ir::FnAttrVA,
                         llvm::formatv("0x{0:x}", I.MemTarget.Value).str());
      std::vector<llvm::Value *> Args = callArgs(Sig, std::nullopt);
      coerceArgs(Callee, Args);
      llvm::CallInst *Call =
          B.CreateCall(Callee, Args,
                       Callee->getReturnType()->isVoidTy() ? "" : "c");
      ir::setInstructionVA(*Call, I.Addr.Value);
      if (llvm::StringRef(Callee->getName()) == "__security_check_cookie")
        clobberFlags();
      else
        clobberVolatile();
      if (Call->getType()->isIntegerTy())
        regWrite(llvm::X86::RAX, extTo(Call, 64));
      else if (Call->getType()->isFloatingPointTy()) {
        llvm::Value *BC =
            B.CreateBitCast(Call, iN(Call->getType()
                                         ->getPrimitiveSizeInBits()),
                            "cr");
        regWrite(llvm::X86::XMM0, extTo(BC, 128));
      }
      Prod.reset();
      (void)IP;
      return;
    }
  }
  if (I.HasTarget) {
    Va T = I.Target;
    const Signature *Sig = nullptr;
    Signature Default;
    if (Sigs)
      Sig = Sigs->of(T);
    if (!Sig && P.Functions.count(T)) {
      Default = defaultCalleeSignature();
      Sig = &Default;
    }
    // Zero-param degenerate decls recover arity from the call site.
    Signature Fixed;
    if (Sig && Sig->Params.empty() && Sigs) {
      if (auto It = Sigs->CallsiteArgs.find(IP);
          It != Sigs->CallsiteArgs.end() && It->second > 0) {
        Fixed = *Sig;
        for (size_t K = 0; K < It->second && K < 4; ++K)
          Fixed.Params.push_back(
              {ParamLoc::reg(Win64Regs[K]), 64, false, std::nullopt});
        Sig = &Fixed;
      }
    }
    Signature NoSig;
    if (!Sig) {
      NoSig.Ret = RetLoc{false, 64};
      Sig = &NoSig;
    }
    llvm::Function *Callee = calleeFn(T, Sigs ? Sigs->of(T) : nullptr);
    std::vector<llvm::Value *> Args = callArgs(Sig, std::nullopt);
    coerceArgs(Callee, Args);
    llvm::CallInst *Call =
        B.CreateCall(Callee, Args,
                     Callee->getReturnType()->isVoidTy() ? "" : "c");
    ir::setInstructionVA(*Call, I.Addr.Value);
    clobberVolatile();
    if (Call->getType()->isIntegerTy())
      regWrite(llvm::X86::RAX, extTo(Call, 64));
    else if (Call->getType()->isFloatingPointTy()) {
      llvm::Value *BC =
          B.CreateBitCast(Call, iN(Call->getType()->getPrimitiveSizeInBits()),
                          "cr");
      regWrite(llvm::X86::XMM0, extTo(BC, 128));
    }
    Prod.reset();
    return;
  }
  // Indirect call: target from operand 0.
  auto Ops = opsOf(I);
  llvm::Value *Ptr = Ops.empty() ? ci(0) : opReadInt(I, 0, 64);
  size_t Prefix = 0;
  if (Sigs)
    if (auto It = Sigs->IndirectArgs.find(IP); It != Sigs->IndirectArgs.end())
      Prefix = It->second;
  Signature S;
  for (size_t K = 0; K < Prefix && K < 4; ++K)
    S.Params.push_back({ParamLoc::reg(Win64Regs[K]), 64, false, std::nullopt});
  S.Ret = RetLoc{false, 64};
  llvm::FunctionType *FT = fnType(&S);
  llvm::Value *FP =
      B.CreateIntToPtr(Ptr, llvm::PointerType::getUnqual(FT), "fp");
  std::vector<llvm::Value *> Args = callArgs(&S, Prefix);
  llvm::CallInst *Call = B.CreateCall(FT, FP, Args, "ic");
  ir::setInstructionVA(*Call, I.Addr.Value);
  clobberVolatile();
  regWrite(llvm::X86::RAX, extTo(Call, 64));
  Prod.reset();
}

void LiftState::emitTailCall(Va Target) {
  // The CFG dispatch handler tail-jumps to rax (`jmp rax`), so `jmp
  // [dispatch]` and `jmp _guard_dispatch_icall_nop` are tail calls through
  // rax. `jmp [check]` reaches a plain `ret`, which is the caller's own
  // return.
  if (P.GuardCFDispatchPtr &&
      (Target == *P.GuardCFDispatchPtr ||
       cfgHandler(*this, P.GuardCFDispatchPtr) == Target)) {
    emitIndirectTail(*this, regRead(llvm::X86::RAX), CurVa);
    return;
  }
  if (P.GuardCFCheckPtr &&
      (Target == *P.GuardCFCheckPtr ||
       cfgHandler(*this, P.GuardCFCheckPtr) == Target)) {
    emitGuardReturn(*this, CurVa);
    return;
  }
  const Signature *Sig = Sigs ? Sigs->of(Target) : nullptr;
  llvm::Function *Callee = calleeFn(Target, Sig);
  Signature Empty;
  Empty.Ret.reset();
  std::vector<llvm::Value *> Args = callArgs(Sig ? Sig : &Empty, std::nullopt);
  coerceArgs(Callee, Args);
  llvm::CallInst *Call =
      B.CreateCall(Callee, Args,
                   Callee->getReturnType()->isVoidTy() ? "" : "tc");
  ir::setInstructionVA(*Call, CurVa.Value);
  const Signature *Caller = Sigs ? Sigs->of(F.Start) : nullptr;
  if (Caller && Caller->Ret) {
    llvm::Value *RV = Call;
    if (Call->getType()->isVoidTy())
      RV = llvm::Constant::getNullValue(
          Caller->Ret->Float
              ? (llvm::Type *)(Caller->Ret->Bits == 32
                                   ? llvm::Type::getFloatTy(Mod.getContext())
                                   : llvm::Type::getDoubleTy(Mod.getContext()))
              : (llvm::Type *)llvm::Type::getIntNTy(
                    Mod.getContext(), std::max(1u, Caller->Ret->Bits)));
    else if (Call->getType()->isIntegerTy() && !Caller->Ret->Float)
      RV = B.CreateZExtOrTrunc(
          Call,
          llvm::Type::getIntNTy(Mod.getContext(),
                                std::max(1u, Caller->Ret->Bits)),
          "tr");
    B.CreateRet(RV);
  } else {
    B.CreateRetVoid();
  }
}

void LiftState::emitIndirectTailCall(const Instr &I) {
  setVA(I.Addr);
  // `jmp [dispatch]` tail-calls the target in rax; `jmp [check]` runs the
  // nop check handler's `ret`.
  if (isGuardDispatch(*this, I, std::nullopt)) {
    emitIndirectTail(*this, regRead(llvm::X86::RAX), I.Addr);
    return;
  }
  if (isGuardCheck(*this, I, std::nullopt)) {
    emitGuardReturn(*this, I.Addr);
    return;
  }
  auto Ops = opsOf(I);
  llvm::Value *TV = Ops.empty() ? ci(0) : opReadInt(I, 0, 64);
  emitIndirectTail(*this, TV, I.Addr);
}

} // namespace revenant::lift
