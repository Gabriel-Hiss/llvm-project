//===- unittests/Transforms/IdiomsTest.cpp - idiom pass tests ------------===//

#include "revenant/Transforms/Idioms/Idioms.h"

#include "gtest/gtest.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

TEST(IdiomsPassTest, LeavesVectorOrForOtherPasses) {
  LLVMContext Context;
  Module M("vector-or", Context);
  Type *VecTy = FixedVectorType::get(Type::getInt32Ty(Context), 4);
  FunctionType *FnTy = FunctionType::get(VecTy, {VecTy, VecTy}, false);
  Function *F = Function::Create(FnTy, Function::ExternalLinkage, "merge", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  IRBuilder<> Builder(Entry);
  Value *Merged = Builder.CreateOr(F->getArg(0), F->getArg(1), "merged");
  Builder.CreateRet(Merged);

  FunctionAnalysisManager AM;
  revenant::IdiomsPass().run(*F, AM);

  auto *Ret = cast<ReturnInst>(Entry->getTerminator());
  EXPECT_TRUE(isa<BinaryOperator>(Ret->getReturnValue()));
  EXPECT_TRUE(Ret->getReturnValue()->getType()->isVectorTy());
}
