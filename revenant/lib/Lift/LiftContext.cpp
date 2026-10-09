//===- LiftContext.cpp - target-neutral lift state ------------------------===//

#include "revenant/Lift/LiftContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;
using namespace llvm::revenant;

LiftContext::LiftContext(LLVMContext &C, Module &M, Function &F)
    : Ctx(C), Mod(M), Fn(&F),
      Inserter([this](Instruction *I) {
        if (VAMD)
          I->setMetadata(VAMDKind, VAMD);
      }),
      B(C, ConstantFolder(), Inserter),
      VAMDKind(C.getMDKindID("revenant.va")),
      OpaqueMDKind(C.getMDKindID("revenant.opaque")) {}

void LiftContext::setVA(uint64_t VA) {
  CurVA = VA;
  VAMD = VA ? MDNode::get(Ctx, ConstantAsMetadata::get(constI(VA, 64)))
            : nullptr;
}

void LiftContext::emitOpaque(const MCInst &MI, StringRef Name) {
  FunctionCallee Fn = Mod.getOrInsertFunction(
      ("revenant.asm." + Name).str(),
      FunctionType::get(Type::getVoidTy(Ctx),
                        SmallVector<Type *>(MI.getNumOperands(), iN(64)),
                        false));
  SmallVector<Value *> Args;
  for (const MCOperand &Op : MI) {
    int64_t V = 0;
    if (Op.isImm())
      V = Op.getImm();
    else if (Op.isReg())
      V = Op.getReg().id();
    Args.push_back(constI(V, 64));
  }
  CallInst *C = B.CreateCall(Fn, Args);
  C->setMetadata(OpaqueMDKind, MDNode::get(Ctx, {}));
}

AllocaInst *LiftContext::regSlot(StringRef Name, Type *Ty) {
  BasicBlock &Entry = Fn->getEntryBlock();
  IRBuilder<> EB(&Entry, Entry.getFirstInsertionPt());
  return EB.CreateAlloca(Ty, nullptr, Name);
}

Value *LiftContext::readRegOp(const MCInst &MI, unsigned MIOpNo, Type *Ty) {
  return regRead(MI.getOperand(MIOpNo).getReg(), Ty);
}

Value *LiftContext::readImmOp(const MCInst &MI, unsigned MIOpNo, Type *Ty) {
  // The disassembler already sign-extended immediates to 64 bits.
  return constI(static_cast<uint64_t>(MI.getOperand(MIOpNo).getImm()),
                cast<IntegerType>(Ty)->getBitWidth());
}

Value *LiftContext::addrToPtr(Value *AddrI64) {
  return B.CreateIntToPtr(AddrI64, ptrTy());
}

Value *LiftContext::memReadAddr(Value *AddrI64, Type *Ty) {
  return B.CreateLoad(Ty, addrToPtr(AddrI64));
}

void LiftContext::memWriteAddr(Value *AddrI64, Value *V) {
  B.CreateStore(V, addrToPtr(AddrI64));
}

Value *LiftContext::memRead(const MCInst &MI, unsigned MIOpNo, Type *Ty) {
  return memReadAddr(memAddr(MI, MIOpNo), Ty);
}

void LiftContext::memWrite(const MCInst &MI, unsigned MIOpNo, Value *V) {
  memWriteAddr(memAddr(MI, MIOpNo), V);
}

Value *LiftContext::readOp(const MCInst &MI, unsigned MIOpNo, Type *Ty) {
  switch (opKind(MI, MIOpNo)) {
  case OpKind::Reg:
    return readRegOp(MI, MIOpNo, Ty);
  case OpKind::Imm:
    return readImmOp(MI, MIOpNo, Ty);
  case OpKind::Mem:
    return memRead(MI, MIOpNo, Ty);
  case OpKind::Invalid:
    return UndefValue::get(Ty);
  }
  llvm_unreachable("bad OpKind");
}

void LiftContext::writeOp(const MCInst &MI, unsigned MIOpNo, Value *V) {
  switch (opKind(MI, MIOpNo)) {
  case OpKind::Reg:
    regWrite(MI.getOperand(MIOpNo).getReg(), V);
    return;
  case OpKind::Mem:
    memWrite(MI, MIOpNo, V);
    return;
  case OpKind::Imm:
  case OpKind::Invalid:
    llvm_unreachable("cannot write to immediate/invalid operand");
  }
}

Value *LiftContext::resolveVA(uint64_t VA) {
  return ConstantExpr::getIntToPtr(constI(VA, 64), ptrTy());
}

Value *LiftContext::resolveVAI64(uint64_t VA) { return constI(VA, 64); }

void LiftContext::jumpToVA(Value *Target) {
  Flows.push_back({PendingFlow::Jump, CurVA, Target, nullptr});
}

void LiftContext::condJump(Value *Pred, Value *Target) {
  Flows.push_back({PendingFlow::CondJump, CurVA, Target, Pred});
}

void LiftContext::callOp(const MCInst &MI, unsigned MIOpNo) {
  Value *Target = nullptr;
  if (opKind(MI, MIOpNo) == OpKind::Imm)
    Target = resolveVAI64(NextVA + MI.getOperand(MIOpNo).getImm());
  else
    Target = readOp(MI, MIOpNo, iN(64));
  // Push the return address; the call itself is left to the driver.
  Flows.push_back({PendingFlow::Call, CurVA, Target, nullptr});
}

void LiftContext::retOp() { Flows.push_back({PendingFlow::Return, CurVA}); }
