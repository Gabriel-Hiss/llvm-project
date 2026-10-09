#include "revenant/Lift/LiftContext.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace llvm::revenant;

namespace {
class TestLiftContext final : public LiftContext {
public:
  TestLiftContext(LLVMContext &C, Module &M, Function &F)
      : LiftContext(C, M, F) {}

  Value *regRead(MCRegister, Type *Ty) override { return UndefValue::get(Ty); }
  void regWrite(MCRegister, Value *) override {}
  OpKind opKind(const MCInst &, unsigned) const override {
    return OpKind::Invalid;
  }
  Value *memAddr(const MCInst &, unsigned) override {
    return constI(0, 64);
  }
};

static uint64_t instructionVA(Instruction *I, unsigned Kind) {
  MDNode *MD = I->getMetadata(Kind);
  if (!MD || MD->getNumOperands() != 1)
    return 0;
  auto *CAM = dyn_cast<ConstantAsMetadata>(MD->getOperand(0));
  auto *CI = CAM ? dyn_cast<ConstantInt>(CAM->getValue()) : nullptr;
  return CI ? CI->getZExtValue() : 0;
}
} // namespace

int main() {
  LLVMContext C;
  Module M("lift-provenance-test", C);
  Function *F = Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                                 GlobalValue::ExternalLinkage, "test", M);
  BasicBlock *Entry = BasicBlock::Create(C, "entry", F);
  TestLiftContext LC(C, M, *F);
  LC.builder().SetInsertPoint(Entry);

  AllocaInst *A = LC.builder().CreateAlloca(LC.iN(32));
  AllocaInst *B = LC.builder().CreateAlloca(LC.iN(32));
  unsigned Kind = C.getMDKindID("revenant.va");

  LC.setVA(0x1234);
  auto *Add = cast<Instruction>(
      LC.builder().CreateAdd(LC.builder().CreateLoad(LC.iN(32), A),
                             LC.builder().CreateLoad(LC.iN(32), B)));
  if (instructionVA(Add, Kind) != 0x1234)
    return 1;

  LC.setVA(0x5678);
  auto *Mul = cast<Instruction>(
      LC.builder().CreateMul(LC.builder().CreateLoad(LC.iN(32), A),
                             LC.builder().CreateLoad(LC.iN(32), B)));
  if (instructionVA(Mul, Kind) != 0x5678)
    return 2;

  LC.setVA(0);
  auto *Sub = cast<Instruction>(
      LC.builder().CreateSub(LC.builder().CreateLoad(LC.iN(32), A),
                             LC.builder().CreateLoad(LC.iN(32), B)));
  if (Sub->getMetadata(Kind))
    return 3;
  return 0;
}
