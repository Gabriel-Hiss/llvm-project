//===- LiftEHTest.cpp - in-try call to invoke rewrite tests ------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Builds hand-written IR resembling lifter output (plain calls carrying
// !revenant.va) plus decoded FunctionEH metadata, runs liftEH, and checks
// the native LLVM EH shapes: MSVC catchswitch/catchpad, SEH catchswitch,
// cleanuppad for unwind-only ranges, landingpad for MinGW LSDA.
//
//===----------------------------------------------------------------------===//

#include "../../lib/Lift/Function/LiftEH.h"

#include "revenant/IR/Contract.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace revenant;
using namespace revenant::lift;

namespace {

LLVMContext &ctx() {
  static LLVMContext C;
  return C;
}

FunctionEH makeEH(EHModel Model, uint64_t Begin, uint64_t End,
                  uint64_t HandlerVa) {
  FunctionEH EH;
  EH.Function = Va(Begin);
  EH.End = Va(End);
  EH.Model = Model;
  EHTryRegion T;
  T.Begin = Va(Begin);
  T.End = Va(End);
  T.State = 0;
  T.Handlers.push_back(0);
  EH.TryRegions.push_back(std::move(T));
  EHHandler H;
  H.Address = Va(HandlerVa);
  H.Kind = HandlerKind::Catch;
  H.State = 0;
  EH.Handlers.push_back(H);
  EHCleanup C;
  C.Address = Va(HandlerVa);
  C.State = 0;
  EH.Cleanups.push_back(C);
  return EH;
}

/// A function with one call carrying !revenant.va inside [Begin, End).
Function *makeCaller(Module &M, StringRef Name, uint64_t CallVA) {
  auto *I32 = Type::getInt32Ty(ctx());
  auto *CalleeTy = FunctionType::get(I32, {I32}, false);
  auto *Callee = Function::Create(CalleeTy, GlobalValue::ExternalLinkage,
                                  "may_throw.test", M);
  // Defined (non-declaration) so liftEH treats it as throwing.
  BasicBlock::Create(ctx(), "entry", Callee);
  IRBuilder<>(&Callee->getEntryBlock()).CreateRet(ConstantInt::get(I32, 0));
  auto *F = Function::Create(FunctionType::get(I32, {I32}, false),
                             GlobalValue::ExternalLinkage, Name, M);
  auto *Entry = BasicBlock::Create(ctx(), "bb_1000", F);
  IRBuilder<> B(Entry);
  auto *Call = B.CreateCall(CalleeTy, Callee, {F->getArg(0)});
  ir::setInstructionVA(*Call, CallVA);
  B.CreateRet(Call);
  return F;
}

TEST(LiftEHTest, MsvcCallInTryBecomesInvokeWithCatchswitch) {
  LLVMContext &C = ctx();
  Module M("t_lifteh_msvc", C);
  Function *F = makeCaller(M, "f_msvc", 0x1100);
  FunctionEH EH = makeEH(EHModel::MSVC, 0x1000, 0x1200, 0x1300);
  EXPECT_EQ(liftEH(*F, EH), 1u);
  EXPECT_TRUE(!verifyFunction(*F, &errs()));
  unsigned Invokes = 0, Switches = 0, Pads = 0;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB) {
      if (isa<InvokeInst>(I))
        ++Invokes;
      if (isa<CatchSwitchInst>(I))
        ++Switches;
      if (isa<CatchPadInst>(I))
        ++Pads;
    }
  EXPECT_EQ(Invokes, 1u);
  EXPECT_EQ(Switches, 1u);
  EXPECT_EQ(Pads, 1u);
  EXPECT_TRUE(F->hasPersonalityFn());
}

TEST(LiftEHTest, CallOutsideTryStaysCall) {
  LLVMContext &C = ctx();
  Module M("t_lifteh_outside", C);
  Function *F = makeCaller(M, "f_outside", 0x5000);
  FunctionEH EH = makeEH(EHModel::MSVC, 0x1000, 0x1200, 0x1300);
  EXPECT_EQ(liftEH(*F, EH), 0u);
  EXPECT_TRUE(!verifyFunction(*F, &errs()));
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      EXPECT_FALSE(isa<InvokeInst>(I));
}

TEST(LiftEHTest, CleanupOnlyRangeEmitsCleanuppad) {
  LLVMContext &C = ctx();
  Module M("t_lifteh_cleanup", C);
  Function *F = makeCaller(M, "f_cleanup", 0x1100);
  FunctionEH EH;
  EH.Function = Va(0x1000);
  EH.End = Va(0x1200);
  EH.Model = EHModel::SEH;
  EHTryRegion T;
  T.Begin = Va(0x1000);
  T.End = Va(0x1200);
  EH.TryRegions.push_back(T);
  EHCleanup Cl;
  Cl.Address = Va(0x1300);
  EH.Cleanups.push_back(Cl);
  EXPECT_EQ(liftEH(*F, EH), 1u);
  EXPECT_TRUE(!verifyFunction(*F, &errs()));
  unsigned Pads = 0;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      if (isa<CleanupPadInst>(I))
        ++Pads;
  EXPECT_EQ(Pads, 1u);
}

TEST(LiftEHTest, ItaniumRangeEmitsLandingpad) {
  LLVMContext &C = ctx();
  Module M("t_lifteh_itanium", C);
  Function *F = makeCaller(M, "f_itanium", 0x1100);
  FunctionEH EH;
  EH.Function = Va(0x1000);
  EH.End = Va(0x1200);
  EH.Model = EHModel::Itanium;
  EHTryRegion T;
  T.Begin = Va(0x1000);
  T.End = Va(0x1200);
  EH.TryRegions.push_back(T);
  EHCleanup Cl;
  Cl.Address = Va(0x1300);
  EH.Cleanups.push_back(Cl);
  EXPECT_EQ(liftEH(*F, EH), 1u);
  EXPECT_TRUE(!verifyFunction(*F, &errs()));
  unsigned Pads = 0;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      if (isa<LandingPadInst>(I))
        ++Pads;
  EXPECT_EQ(Pads, 1u);
}

} // namespace
