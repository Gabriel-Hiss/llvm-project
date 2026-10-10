//===- StructuredStmtBuilderTest.cpp - region -> clang stmt tests --------===//
//
// Feeds hand-built contract IR (blocks named bb_<VA>, terminators carrying
// !revenant.region) through the structured StmtBuilder and prints the
// resulting function with Clang's own printer. Exercises if/else, else-if,
// while, do-while, for-shape recovery, switch, and the goto fallback.
// Uses Core's stub builders so expressions become typed temporaries.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/ClangFrontend.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"
#include "clang/AST/PrettyPrinter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <memory>
#include <string>

using namespace clang;
using namespace llvm;
using namespace revenant::clangx;
using llvm::Module;

namespace {

LLVMContext &ctx() {
  static LLVMContext C;
  return C;
}

void tagRegion(llvm::Instruction *T, StringRef Tag, llvm::ArrayRef<unsigned> Args) {
  LLVMContext &C = ctx();
  SmallVector<Metadata *, 4> Ops;
  Ops.push_back(MDString::get(C, Tag));
  for (unsigned A : Args)
    Ops.push_back(ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(C), A)));
  T->setMetadata("revenant.region", MDNode::get(C, Ops));
}

unsigned indexOf(llvm::Function &F, llvm::BasicBlock *BB) {
  unsigned I = 0;
  for (llvm::BasicBlock &B : F) {
    if (&B == BB)
      return I;
    ++I;
  }
  llvm_unreachable("block not in function");
}

/// Decompile the module and return the printed text of function `F`.
std::string printFunction(llvm::Module &M, llvm::Function &F) {
  auto T = createASTContext(M, ClangFrontendOptions{});
  EXPECT_TRUE(static_cast<bool>(T));
  FrontendContext &FCtx = (*T)->frontend();
  NullTypeBuilder Types(FCtx);
  std::unique_ptr<ExprBuilder> Exprs = createExprBuilder(FCtx, Types);
  std::unique_ptr<StmtBuilder> Stmts = createStmtBuilder(FCtx, Types);
  fillTranslationUnit(**T, M, Types, *Stmts, *Exprs);

  clang::FunctionDecl *FD = FCtx.functionDecl(F, Types);
  std::string Out;
  raw_string_ostream OS(Out);
  FD->print(OS, (*T)->ast().getPrintingPolicy());
  return Out;
}

TEST(StructuredStmtBuilderTest, IfThen) {
  LLVMContext &C = ctx();
  llvm::Module M("t_if", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_if", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_1000", F);
  llvm::BasicBlock *Then = BasicBlock::Create(C, "bb_1010", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_1020", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), Then,
                 Follow);
  B.SetInsertPoint(Then);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(X);
  tagRegion(Entry->getTerminator(), "if", {indexOf(*F, Follow)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("if"), std::string::npos);
  EXPECT_NE(S.find("L_bb_1010:"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, IfElse) {
  LLVMContext &C = ctx();
  llvm::Module M("t_ifelse", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_ifelse",
                                 M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_2000", F);
  llvm::BasicBlock *T = BasicBlock::Create(C, "bb_2010", F);
  llvm::BasicBlock *E = BasicBlock::Create(C, "bb_2020", F);
  llvm::BasicBlock *Merge = BasicBlock::Create(C, "bb_2030", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), T, E);
  B.SetInsertPoint(T);
  B.CreateBr(Merge);
  B.SetInsertPoint(E);
  B.CreateBr(Merge);
  B.SetInsertPoint(Merge);
  B.CreateRet(X);
  tagRegion(Entry->getTerminator(), "if", {indexOf(*F, Merge)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("else"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, While) {
  LLVMContext &C = ctx();
  llvm::Module M("t_while", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_while", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_3000", F);
  llvm::BasicBlock *Head = BasicBlock::Create(C, "bb_3010", F);
  llvm::BasicBlock *Body = BasicBlock::Create(C, "bb_3020", F);
  llvm::BasicBlock *Exit = BasicBlock::Create(C, "bb_3030", F);
  llvm::IRBuilder<> B(Entry);
  llvm::AllocaInst *I = B.CreateAlloca(I32, nullptr, "i");
  B.CreateStore(X, I);
  B.CreateBr(Head);
  B.SetInsertPoint(Head);
  llvm::Value *Lv = B.CreateLoad(I32, I);
  B.CreateCondBr(B.CreateICmpULT(Lv, llvm::ConstantInt::get(I32, 10)), Body, Exit);
  B.SetInsertPoint(Body);
  B.CreateStore(B.CreateAdd(B.CreateLoad(I32, I), llvm::ConstantInt::get(I32, 1)),
                I);
  B.CreateBr(Head);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.CreateLoad(I32, I));
  tagRegion(Head->getTerminator(), "while",
            {indexOf(*F, Head), indexOf(*F, Body), indexOf(*F, Exit)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("while"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, DoWhile) {
  LLVMContext &C = ctx();
  llvm::Module M("t_dowhile", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_do", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_4000", F);
  llvm::BasicBlock *Head = BasicBlock::Create(C, "bb_4010", F);
  llvm::BasicBlock *Latch = BasicBlock::Create(C, "bb_4020", F);
  llvm::BasicBlock *Exit = BasicBlock::Create(C, "bb_4030", F);
  llvm::IRBuilder<> B(Entry);
  llvm::AllocaInst *I = B.CreateAlloca(I32, nullptr, "i");
  B.CreateStore(X, I);
  B.CreateBr(Head);
  B.SetInsertPoint(Head);
  B.CreateBr(Latch);
  B.SetInsertPoint(Latch);
  B.CreateStore(B.CreateAdd(B.CreateLoad(I32, I), llvm::ConstantInt::get(I32, 1)),
                I);
  B.CreateCondBr(B.CreateICmpULT(B.CreateLoad(I32, I),
                                 llvm::ConstantInt::get(I32, 10)),
                 Head, Exit);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.CreateLoad(I32, I));
  tagRegion(Head->getTerminator(), "dowhile",
            {indexOf(*F, Head), indexOf(*F, Latch), indexOf(*F, Exit)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("do"), std::string::npos);
  EXPECT_NE(S.find("while"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, ForShape) {
  LLVMContext &C = ctx();
  llvm::Module M("t_for", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_for", M);
  llvm::Argument *N = F->getArg(0);
  llvm::BasicBlock *Pre = BasicBlock::Create(C, "bb_5000", F);
  llvm::BasicBlock *Head = BasicBlock::Create(C, "bb_5010", F);
  llvm::BasicBlock *Body = BasicBlock::Create(C, "bb_5020", F);
  llvm::BasicBlock *Latch = BasicBlock::Create(C, "bb_5030", F);
  llvm::BasicBlock *Exit = BasicBlock::Create(C, "bb_5040", F);
  llvm::IRBuilder<> B(Pre);
  llvm::AllocaInst *I = B.CreateAlloca(I32, nullptr, "i");
  B.CreateStore(llvm::ConstantInt::get(I32, 0), I);
  B.CreateBr(Head);
  B.SetInsertPoint(Head);
  llvm::Value *Lv = B.CreateLoad(I32, I);
  B.CreateCondBr(B.CreateICmpSLT(Lv, N), Body, Exit);
  B.SetInsertPoint(Body);
  B.CreateCondBr(B.CreateICmpEQ(N, llvm::ConstantInt::get(I32, 0)), Head,
                 Latch);
  B.SetInsertPoint(Latch);
  B.CreateStore(B.CreateAdd(B.CreateLoad(I32, I), llvm::ConstantInt::get(I32, 1)),
                I);
  B.CreateBr(Head);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.CreateLoad(I32, I));
  tagRegion(Head->getTerminator(), "for",
            {indexOf(*F, Head), indexOf(*F, Latch), indexOf(*F, Exit)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("for"), std::string::npos);
  EXPECT_NE(S.find("continue"), std::string::npos);
  EXPECT_NE(S.find("++"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, ForCompoundAssignmentStep) {
  LLVMContext &C = ctx();
  llvm::Module M("t_for_compound", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_for_compound", M);
  llvm::Argument *N = F->getArg(0);
  llvm::BasicBlock *Pre = BasicBlock::Create(C, "bb_5100", F);
  llvm::BasicBlock *Head = BasicBlock::Create(C, "bb_5110", F);
  llvm::BasicBlock *Body = BasicBlock::Create(C, "bb_5120", F);
  llvm::BasicBlock *Latch = BasicBlock::Create(C, "bb_5130", F);
  llvm::BasicBlock *Exit = BasicBlock::Create(C, "bb_5140", F);
  llvm::IRBuilder<> B(Pre);
  llvm::AllocaInst *I = B.CreateAlloca(I32, nullptr, "i");
  B.CreateStore(llvm::ConstantInt::get(I32, 0), I);
  B.CreateBr(Head);
  B.SetInsertPoint(Head);
  B.CreateCondBr(B.CreateICmpSLT(B.CreateLoad(I32, I), N), Body, Exit);
  B.SetInsertPoint(Body);
  B.CreateCondBr(B.CreateICmpEQ(N, llvm::ConstantInt::get(I32, 0)), Head,
                 Latch);
  B.SetInsertPoint(Latch);
  B.CreateStore(B.CreateAdd(B.CreateLoad(I32, I), llvm::ConstantInt::get(I32, 2)),
                I);
  B.CreateBr(Head);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.CreateLoad(I32, I));
  tagRegion(Head->getTerminator(), "for",
            {indexOf(*F, Head), indexOf(*F, Latch), indexOf(*F, Exit)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("+="), std::string::npos);
}


TEST(StructuredStmtBuilderTest, Switch) {
  LLVMContext &C = ctx();
  llvm::Module M("t_switch", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_sw", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_6000", F);
  llvm::BasicBlock *A = BasicBlock::Create(C, "bb_6010", F);
  llvm::BasicBlock *Bb = BasicBlock::Create(C, "bb_6020", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_6030", F);
  llvm::IRBuilder<> B(Entry);
  llvm::SwitchInst *SW = B.CreateSwitch(X, Follow, 2);
  SW->addCase(llvm::ConstantInt::get(cast<llvm::IntegerType>(I32), 0), A);
  SW->addCase(llvm::ConstantInt::get(cast<llvm::IntegerType>(I32), 1), Bb);
  B.SetInsertPoint(A);
  B.CreateBr(Follow);
  B.SetInsertPoint(Bb);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(X);
  tagRegion(Entry->getTerminator(), "switch", {indexOf(*F, Follow)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("switch"), std::string::npos);
  EXPECT_NE(S.find("case"), std::string::npos);
}
TEST(StructuredStmtBuilderTest, ExternalGotoToRegionHeaderHasLabel) {
  LLVMContext &C = ctx();
  llvm::Module M("t_region_entry", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_region_entry", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_8000", F);
  llvm::BasicBlock *Header = BasicBlock::Create(C, "bb_8010", F);
  llvm::BasicBlock *Then = BasicBlock::Create(C, "bb_8020", F);
  llvm::BasicBlock *Bridge = BasicBlock::Create(C, "bb_8030", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_8040", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), Header,
                 Bridge);
  B.SetInsertPoint(Header);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), Then,
                 Follow);
  B.SetInsertPoint(Then);
  B.CreateBr(Follow);
  B.SetInsertPoint(Bridge);
  B.CreateBr(Header);
  B.SetInsertPoint(Follow);
  B.CreateRet(X);
  tagRegion(Header->getTerminator(), "if", {indexOf(*F, Follow)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("goto f_region_entry_L_bb_8010"), std::string::npos);
  EXPECT_NE(S.find("f_region_entry_L_bb_8010:"), std::string::npos);
}
TEST(StructuredStmtBuilderTest, SanitizedLabelsStayUnique) {
  LLVMContext &C = ctx();
  llvm::Module M("t_label_names", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_label_names", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_entry", F);
  llvm::BasicBlock *Dash = BasicBlock::Create(C, "bb-1", F);
  llvm::BasicBlock *Underscore = BasicBlock::Create(C, "bb_1", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), Dash,
                 Underscore);
  B.SetInsertPoint(Dash);
  B.CreateRet(llvm::ConstantInt::get(I32, 1));
  B.SetInsertPoint(Underscore);
  B.CreateRet(llvm::ConstantInt::get(I32, 2));
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("L_bb_1:"), std::string::npos);
  EXPECT_NE(S.find("L_bb_1_1:"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, GotoFallback) {
  LLVMContext &C = ctx();
  llvm::Module M("t_goto", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(llvm::FunctionType::get(I32, {I32}, false),
                                 llvm::GlobalValue::ExternalLinkage, "f_goto", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *A = BasicBlock::Create(C, "bb_7000", F);
  llvm::BasicBlock *Bb = BasicBlock::Create(C, "bb_7010", F);
  llvm::BasicBlock *Cc = BasicBlock::Create(C, "bb_7020", F);
  llvm::IRBuilder<> B(A);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), Bb, Cc);
  B.SetInsertPoint(Bb);
  B.CreateBr(Cc);
  B.SetInsertPoint(Cc);
  B.CreateRet(X);
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("goto"), std::string::npos);
  EXPECT_NE(S.find("L_bb_7000"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, TryCatch) {
  LLVMContext &C = ctx();
  llvm::Module M("t_try", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  llvm::Function *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "may_throw", M);
  llvm::Function *Pers = llvm::Function::Create(
      llvm::FunctionType::get(I32, true), llvm::GlobalValue::ExternalLinkage,
      "__CxxFrameHandler3", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_try", M);
  F->setPersonalityFn(Pers);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_8000", F);
  llvm::BasicBlock *Normal = BasicBlock::Create(C, "bb_8010", F);
  llvm::BasicBlock *Dispatch = BasicBlock::Create(C, "bb_8020", F);
  llvm::BasicBlock *Catch = BasicBlock::Create(C, "bb_8030", F);
  llvm::BasicBlock *Ret = BasicBlock::Create(C, "bb_8040", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateInvoke(Callee->getFunctionType(), Callee, Normal, Dispatch, {X});
  B.SetInsertPoint(Normal);
  B.CreateBr(Ret);
  B.SetInsertPoint(Dispatch);
  auto *SW = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(C), nullptr, 1);
  SW->addHandler(Catch);
  B.SetInsertPoint(Catch);
  llvm::Value *NullPtr = llvm::ConstantPointerNull::get(Ptr);
  auto *Pad = B.CreateCatchPad(SW, {NullPtr, llvm::ConstantInt::get(I32, 0),
                                    NullPtr});
  Pad->setMetadata("revenant.catchtype",
                   MDNode::get(C, MDString::get(C, ".?AVruntime_error@std@@")));
  B.CreateCatchRet(Pad, Ret);
  B.SetInsertPoint(Ret);
  B.CreateRet(llvm::ConstantInt::get(I32, 0));
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("try"), std::string::npos);
  EXPECT_NE(S.find("catch"), std::string::npos);
  EXPECT_NE(S.find("catch (const ::std::runtime_error &e)"),
            std::string::npos);
}
TEST(StructuredStmtBuilderTest, TryCatchWithVoidContinuation) {
  LLVMContext &C = ctx();
  llvm::Module M("t_try_void", C);
  auto *VoidTy = llvm::Type::getVoidTy(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  llvm::Function *Callee = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "may_throw_void", M);
  llvm::Function *Pers = llvm::Function::Create(
      llvm::FunctionType::get(I32, true), llvm::GlobalValue::ExternalLinkage,
      "__CxxFrameHandler3", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "f_try_void", M);
  F->setPersonalityFn(Pers);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_8050", F);
  llvm::BasicBlock *Normal = BasicBlock::Create(C, "bb_8060", F);
  llvm::BasicBlock *Dispatch = BasicBlock::Create(C, "bb_8070", F);
  llvm::BasicBlock *Catch = BasicBlock::Create(C, "bb_8080", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateInvoke(Callee->getFunctionType(), Callee, Normal, Dispatch, {});
  B.SetInsertPoint(Dispatch);
  auto *Switch = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(C), nullptr,
                                     1);
  Switch->addHandler(Catch);
  B.SetInsertPoint(Catch);
  llvm::Value *NullPtr = llvm::ConstantPointerNull::get(Ptr);
  auto *Pad = B.CreateCatchPad(
      Switch, {NullPtr, llvm::ConstantInt::get(I32, 0), NullPtr});
  B.CreateCatchRet(Pad, Normal);
  B.SetInsertPoint(Normal);
  B.CreateRetVoid();

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("try"), std::string::npos);
  EXPECT_NE(S.find("catch"), std::string::npos);
}
TEST(StructuredStmtBuilderTest, GroupsInvokesInSameEhRegion) {
  LLVMContext &C = ctx();
  llvm::Module M("t_try_group", C);
  auto *VoidTy = llvm::Type::getVoidTy(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  llvm::Function *FirstCall = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "first_throw", M);
  llvm::Function *SecondCall = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "second_throw", M);
  llvm::Function *Pers = llvm::Function::Create(
      llvm::FunctionType::get(I32, true), llvm::GlobalValue::ExternalLinkage,
      "__CxxFrameHandler3", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "f_try_group", M);
  F->setPersonalityFn(Pers);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_8090", F);
  llvm::BasicBlock *NextCall = BasicBlock::Create(C, "bb_80a0", F);
  llvm::BasicBlock *Dispatch = BasicBlock::Create(C, "bb_80b0", F);
  llvm::BasicBlock *Catch = BasicBlock::Create(C, "bb_80c0", F);
  llvm::BasicBlock *Normal = BasicBlock::Create(C, "bb_80d0", F);
  llvm::IRBuilder<> B(Entry);
  auto *First = B.CreateInvoke(FirstCall->getFunctionType(), FirstCall,
                               NextCall, Dispatch, {});
  B.SetInsertPoint(NextCall);
  auto *Second = B.CreateInvoke(SecondCall->getFunctionType(), SecondCall,
                                Normal, Dispatch, {});
  MDNode *Region = MDNode::get(
      C, ConstantAsMetadata::get(llvm::ConstantInt::get(I32, 7)));
  First->setMetadata("revenant.eh.region", Region);
  Second->setMetadata("revenant.eh.region", Region);
  B.SetInsertPoint(Dispatch);
  auto *Switch = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(C), nullptr,
                                     1);
  Switch->addHandler(Catch);
  B.SetInsertPoint(Catch);
  llvm::Value *NullPtr = llvm::ConstantPointerNull::get(Ptr);
  auto *Pad = B.CreateCatchPad(
      Switch, {NullPtr, llvm::ConstantInt::get(I32, 0), NullPtr});
  B.CreateCatchRet(Pad, Normal);
  B.SetInsertPoint(Normal);
  B.CreateRetVoid();

  std::string S = printFunction(M, *F);
  EXPECT_EQ(StringRef(S).count("try {"), 1u);
  EXPECT_EQ(StringRef(S).count("catch ("), 1u);
  EXPECT_NE(S.find("first_throw"), std::string::npos);
  EXPECT_NE(S.find("second_throw"), std::string::npos);
}
TEST(StructuredStmtBuilderTest, ItaniumTryCatch) {
  LLVMContext &C = ctx();
  llvm::Module M("t_itanium_try", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  llvm::Function *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "may_throw_itanium", M);
  llvm::Function *Pers = llvm::Function::Create(
      llvm::FunctionType::get(I32, true), llvm::GlobalValue::ExternalLinkage,
      "__gxx_personality_seh0", M);
  llvm::Function *BeginCatch = llvm::Function::Create(
      llvm::FunctionType::get(Ptr, {Ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "__cxa_begin_catch", M);
  llvm::Function *EndCatch = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(C), {}, false),
      llvm::GlobalValue::ExternalLinkage, "__cxa_end_catch", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_itanium_try", M);
  F->setPersonalityFn(Pers);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_8100", F);
  llvm::BasicBlock *Normal = BasicBlock::Create(C, "bb_8110", F);
  llvm::BasicBlock *Dispatch = BasicBlock::Create(C, "bb_8120", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateInvoke(Callee->getFunctionType(), Callee, Normal, Dispatch, {X});
  B.SetInsertPoint(Normal);
  B.CreateRet(llvm::ConstantInt::get(I32, 0));
  B.SetInsertPoint(Dispatch);
  auto *LP = B.CreateLandingPad(
      llvm::StructType::get(C, {Ptr, I32}), 1, "eh.lpad");
  LP->addClause(llvm::ConstantExpr::getIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 0x140010000), Ptr));
  LP->setMetadata("revenant.catchtype",
                  MDNode::get(C, MDString::get(C, "St9exception")));
  auto *ExceptionObject = B.CreateExtractValue(LP, 0);
  B.CreateCall(BeginCatch, {ExceptionObject});
  B.CreateCall(EndCatch);
  B.CreateBr(Normal);

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("try"), std::string::npos);
  EXPECT_NE(S.find("catch (const ::std::exception &e)"),
            std::string::npos);
}

TEST(StructuredStmtBuilderTest, SehTryExcept) {
  LLVMContext &C = ctx();
  llvm::Module M("t_seh", C);
  M.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);
  llvm::Function *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I32, {}, false),
      llvm::GlobalValue::ExternalLinkage, "may_throw_seh", M);
  llvm::Function *Pers = llvm::Function::Create(
      llvm::FunctionType::get(I32, true), llvm::GlobalValue::ExternalLinkage,
      "__C_specific_handler", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_seh", M);
  F->setPersonalityFn(Pers);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_9000", F);
  llvm::BasicBlock *Normal = BasicBlock::Create(C, "bb_9010", F);
  llvm::BasicBlock *Dispatch = BasicBlock::Create(C, "bb_9020", F);
  llvm::BasicBlock *Except = BasicBlock::Create(C, "bb_9030", F);
  llvm::BasicBlock *Ret = BasicBlock::Create(C, "bb_9040", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateInvoke(Callee->getFunctionType(), Callee, Normal, Dispatch, {});
  B.SetInsertPoint(Normal);
  B.CreateBr(Ret);
  B.SetInsertPoint(Dispatch);
  auto *SW = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(C), nullptr, 1);
  SW->addHandler(Except);
  B.SetInsertPoint(Except);
  llvm::Value *NullPtr = llvm::ConstantPointerNull::get(Ptr);
  auto *Pad = B.CreateCatchPad(SW, {NullPtr});
  B.CreateCatchRet(Pad, Ret);
  B.SetInsertPoint(Ret);
  B.CreateRet(llvm::ConstantInt::get(I32, 0));
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("__try"), std::string::npos);
  EXPECT_NE(S.find("__except"), std::string::npos);
  EXPECT_NE(S.find("__except (1)"), std::string::npos);
}
TEST(StructuredStmtBuilderTest, SehTryFinally) {
  LLVMContext &C = ctx();
  llvm::Module M("t_seh_finally", C);
  M.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  llvm::Type *VoidTy = llvm::Type::getVoidTy(C);
  llvm::Function *Throw = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "may_throw_finally", M);
  llvm::Function *Cleanup = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "cleanup_finally", M);
  llvm::Function *Pers = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(C), true),
      llvm::GlobalValue::ExternalLinkage, "__C_specific_handler", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, {}, false),
      llvm::GlobalValue::ExternalLinkage, "f_seh_finally", M);
  F->setPersonalityFn(Pers);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_a000", F);
  llvm::BasicBlock *Normal = BasicBlock::Create(C, "bb_a010", F);
  llvm::BasicBlock *Dispatch = BasicBlock::Create(C, "bb_a020", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateInvoke(Throw->getFunctionType(), Throw, Normal, Dispatch, {});
  B.SetInsertPoint(Normal);
  B.CreateRetVoid();
  B.SetInsertPoint(Dispatch);
  auto *Pad = B.CreateCleanupPad(llvm::ConstantTokenNone::get(C), {},
                                 "finally.pad");
  Pad->setMetadata("revenant.finally", MDNode::get(C, {}));
  B.CreateCall(Cleanup);
  B.CreateCleanupRet(Pad);

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("__try"), std::string::npos);
  EXPECT_NE(S.find("__finally"), std::string::npos);
  EXPECT_NE(S.find("cleanup_finally"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, EarlyReturnRegionUsesStructuredIf) {
  LLVMContext &C = ctx();
  llvm::Module M("t_early_return", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_early_return", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_b000", F);
  llvm::BasicBlock *Return = BasicBlock::Create(C, "bb_b010", F);
  llvm::BasicBlock *Continue = BasicBlock::Create(C, "bb_b020", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCondBr(B.CreateICmpEQ(X, llvm::ConstantInt::get(I32, 0)), Return,
                 Continue);
  B.SetInsertPoint(Return);
  B.CreateRet(llvm::ConstantInt::get(I32, 7));
  B.SetInsertPoint(Continue);
  B.CreateRet(B.CreateAdd(X, llvm::ConstantInt::get(I32, 1)));

  SmallVector<Metadata *, 2> Ops{
      MDString::get(C, "if"),
      ConstantAsMetadata::get(llvm::ConstantInt::getSigned(
          llvm::Type::getInt32Ty(C), -1))};
  Entry->getTerminator()->setMetadata("revenant.region", MDNode::get(C, Ops));

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("if"), std::string::npos);
  EXPECT_NE(S.find("return 7"), std::string::npos);
  EXPECT_NE(S.find("return"), std::string::npos);
  EXPECT_EQ(S.find("goto"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, SwitchDefaultAndCaseShareBody) {
  LLVMContext &C = ctx();
  llvm::Module M("t_switch_shared_default", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_switch_shared_default", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_c000", F);
  llvm::BasicBlock *Case = BasicBlock::Create(C, "bb_c010", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_c020", F);
  llvm::IRBuilder<> B(Entry);
  auto *SW = B.CreateSwitch(X, Case, 1);
  SW->addCase(llvm::ConstantInt::get(I32, 0), Case);
  B.SetInsertPoint(Case);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(X);
  tagRegion(Entry->getTerminator(), "switch", {indexOf(*F, Follow)});

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("switch"), std::string::npos);
  EXPECT_NE(S.find("case 0"), std::string::npos);
  EXPECT_NE(S.find("default"), std::string::npos);
  EXPECT_EQ(S.find("goto"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, SharedReturnSlotBecomesBranchReturns) {
  LLVMContext &C = ctx();
  llvm::Module M("t_shared_return", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_shared_return", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_d000", F);
  llvm::BasicBlock *ReturnOne = BasicBlock::Create(C, "bb_d010", F);
  llvm::BasicBlock *ReturnTwo = BasicBlock::Create(C, "bb_d020", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_d030", F);
  llvm::IRBuilder<> B(Entry);
  llvm::AllocaInst *Slot = B.CreateAlloca(I32, nullptr, "return.slot");
  B.CreateCondBr(B.CreateICmpEQ(X, llvm::ConstantInt::get(I32, 0)),
                 ReturnOne, ReturnTwo);
  B.SetInsertPoint(ReturnOne);
  B.CreateStore(llvm::ConstantInt::get(I32, 1), Slot);
  B.CreateBr(Follow);
  B.SetInsertPoint(ReturnTwo);
  B.CreateStore(llvm::ConstantInt::get(I32, 2), Slot);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(B.CreateLoad(I32, Slot));
  tagRegion(Entry->getTerminator(), "if", {indexOf(*F, Follow)});

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("return 1"), std::string::npos);
  EXPECT_NE(S.find("return 2"), std::string::npos);
  EXPECT_EQ(S.find("goto"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, NestedReturnArmsDoNotNeedGotoEdges) {
  LLVMContext &C = ctx();
  llvm::Module M("t_nested_shared_return", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_nested_shared_return", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_e000", F);
  llvm::BasicBlock *Nested = BasicBlock::Create(C, "bb_e010", F);
  llvm::BasicBlock *ReturnZero = BasicBlock::Create(C, "bb_e020", F);
  llvm::BasicBlock *ReturnOne = BasicBlock::Create(C, "bb_e030", F);
  llvm::BasicBlock *ReturnTwo = BasicBlock::Create(C, "bb_e040", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_e050", F);
  llvm::IRBuilder<> B(Entry);
  llvm::AllocaInst *Slot = B.CreateAlloca(I32, nullptr, "return.slot");
  B.CreateCondBr(B.CreateICmpEQ(X, llvm::ConstantInt::get(I32, 0)), Nested,
                 ReturnZero);
  B.SetInsertPoint(Nested);
  B.CreateCondBr(B.CreateICmpEQ(X, llvm::ConstantInt::get(I32, 1)),
                 ReturnOne, ReturnTwo);
  B.SetInsertPoint(ReturnZero);
  B.CreateStore(llvm::ConstantInt::get(I32, 0), Slot);
  B.CreateBr(Follow);
  B.SetInsertPoint(ReturnOne);
  B.CreateStore(llvm::ConstantInt::get(I32, 1), Slot);
  B.CreateBr(Follow);
  B.SetInsertPoint(ReturnTwo);
  B.CreateStore(llvm::ConstantInt::get(I32, 2), Slot);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(B.CreateLoad(I32, Slot));
  tagRegion(Entry->getTerminator(), "if", {indexOf(*F, Follow)});

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("return 0"), std::string::npos);
  EXPECT_NE(S.find("return 1"), std::string::npos);
  EXPECT_NE(S.find("return 2"), std::string::npos);
  EXPECT_EQ(S.find("goto"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, ForContinueEdgesEnterTheLatch) {
  LLVMContext &C = ctx();
  llvm::Module M("t_for_latch_continue", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32, I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_for_latch_continue", M);
  llvm::Argument *N = F->getArg(0);
  llvm::Argument *Skip = F->getArg(1);
  llvm::BasicBlock *Pre = BasicBlock::Create(C, "bb_f100", F);
  llvm::BasicBlock *Header = BasicBlock::Create(C, "bb_f110", F);
  llvm::BasicBlock *Body = BasicBlock::Create(C, "bb_f120", F);
  llvm::BasicBlock *Work = BasicBlock::Create(C, "bb_f130", F);
  llvm::BasicBlock *Latch = BasicBlock::Create(C, "bb_f140", F);
  llvm::BasicBlock *Exit = BasicBlock::Create(C, "bb_f150", F);
  llvm::IRBuilder<> B(Pre);
  llvm::AllocaInst *IV = B.CreateAlloca(I32, nullptr, "i");
  llvm::AllocaInst *Sum = B.CreateAlloca(I32, nullptr, "sum");
  B.CreateStore(llvm::ConstantInt::get(I32, 0), IV);
  B.CreateStore(llvm::ConstantInt::get(I32, 0), Sum);
  B.CreateBr(Header);
  B.SetInsertPoint(Header);
  llvm::Value *I = B.CreateLoad(I32, IV);
  B.CreateCondBr(B.CreateICmpSLT(I, N), Body, Exit);
  B.SetInsertPoint(Body);
  B.CreateCondBr(B.CreateICmpEQ(Skip, llvm::ConstantInt::get(I32, 0)),
                 Latch, Work);
  B.SetInsertPoint(Work);
  B.CreateStore(B.CreateAdd(B.CreateLoad(I32, Sum), I), Sum);
  B.CreateBr(Latch);
  B.SetInsertPoint(Latch);
  B.CreateStore(B.CreateAdd(B.CreateLoad(I32, IV),
                            llvm::ConstantInt::get(I32, 1)),
                IV);
  B.CreateBr(Header);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.CreateLoad(I32, Sum));
  tagRegion(Header->getTerminator(), "for",
            {indexOf(*F, Header), indexOf(*F, Latch), indexOf(*F, Exit)});

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("for"), std::string::npos);
  EXPECT_NE(S.find("continue"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, GenericLoopEmitsUnboundedLoop) {
  LLVMContext &C = ctx();
  llvm::Module M("t_generic_loop", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_generic_loop", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_f000", F);
  llvm::BasicBlock *Header = BasicBlock::Create(C, "bb_f010", F);
  llvm::BasicBlock *Body = BasicBlock::Create(C, "bb_f020", F);
  llvm::BasicBlock *BreakTest = BasicBlock::Create(C, "bb_f030", F);
  llvm::BasicBlock *ExitEdge = BasicBlock::Create(C, "bb_f038", F);
  llvm::BasicBlock *ContinueEdge = BasicBlock::Create(C, "bb_f039", F);
  llvm::BasicBlock *Exit = BasicBlock::Create(C, "bb_f040", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateBr(Header);
  B.SetInsertPoint(Header);
  B.CreateCondBr(B.CreateICmpSGT(X, llvm::ConstantInt::get(I32, 0)), Body,
                 BreakTest);
  B.SetInsertPoint(Body);
  B.CreateBr(ContinueEdge);
  B.SetInsertPoint(BreakTest);
  B.CreateCondBr(B.CreateICmpEQ(X, llvm::ConstantInt::get(I32, 0)),
                 ExitEdge, Body);
  B.SetInsertPoint(ExitEdge);
  B.CreateBr(Exit);
  B.SetInsertPoint(ContinueEdge);
  B.CreateBr(Header);
  B.SetInsertPoint(Exit);
  B.CreateRet(X);
  tagRegion(Header->getTerminator(), "loop",
            {indexOf(*F, Header), indexOf(*F, ContinueEdge),
             indexOf(*F, Exit)});

  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("while"), std::string::npos);
  EXPECT_NE(S.find("break"), std::string::npos);
  EXPECT_NE(S.find("continue"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, IfHeaderLeafStatementsEmitBeforeBranch) {
  // An if-region header that computes values must print them ahead of the
  // if: previously they were dropped entirely (spdlog-gcc-o0
  // @sub_140145C40 lost the loads that fed both the test and later
  // blocks). The callee call must survive and come before `if`.
  LLVMContext &C = ctx();
  llvm::Module M("t_ifhead", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I32, {}, false),
      llvm::GlobalValue::ExternalLinkage, "side_effect", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_hdr", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_9000", F);
  llvm::BasicBlock *Then = BasicBlock::Create(C, "bb_9010", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_9020", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCall(Callee);
  B.CreateCondBr(B.CreateICmpNE(X, llvm::ConstantInt::get(I32, 0)), Then,
                 Follow);
  B.SetInsertPoint(Then);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(X);
  tagRegion(Entry->getTerminator(), "if", {indexOf(*F, Follow)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("side_effect"), std::string::npos);
  EXPECT_NE(S.find("if"), std::string::npos);
  EXPECT_LT(S.find("side_effect"), S.find("if ("));
}

TEST(StructuredStmtBuilderTest, SwitchHeaderLeafStatementsEmitBeforeBranch) {
  // Same drop for switch headers: header statements run before the
  // dispatch and must not vanish.
  LLVMContext &C = ctx();
  llvm::Module M("t_swhead", C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  llvm::Function *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I32, {}, false),
      llvm::GlobalValue::ExternalLinkage, "side_effect", M);
  llvm::Function *F = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "f_swhead", M);
  llvm::Argument *X = F->getArg(0);
  llvm::BasicBlock *Entry = BasicBlock::Create(C, "bb_9100", F);
  llvm::BasicBlock *A = BasicBlock::Create(C, "bb_9110", F);
  llvm::BasicBlock *Follow = BasicBlock::Create(C, "bb_9130", F);
  llvm::IRBuilder<> B(Entry);
  B.CreateCall(Callee);
  llvm::SwitchInst *SW = B.CreateSwitch(X, Follow, 1);
  SW->addCase(llvm::ConstantInt::get(cast<llvm::IntegerType>(I32), 0), A);
  B.SetInsertPoint(A);
  B.CreateBr(Follow);
  B.SetInsertPoint(Follow);
  B.CreateRet(X);
  tagRegion(Entry->getTerminator(), "switch", {indexOf(*F, Follow)});
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("side_effect"), std::string::npos);
  EXPECT_NE(S.find("switch"), std::string::npos);
  EXPECT_LT(S.find("side_effect"), S.find("switch"));
}

} // namespace
