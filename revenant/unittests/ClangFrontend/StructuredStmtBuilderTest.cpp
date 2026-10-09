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
  B.CreateBr(Latch);
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
  EXPECT_NE(S.find("goto L_bb_8010"), std::string::npos);
  EXPECT_NE(S.find("L_bb_8010:"), std::string::npos);
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
  B.CreateCatchRet(Pad, Ret);
  B.SetInsertPoint(Ret);
  B.CreateRet(llvm::ConstantInt::get(I32, 0));
  std::string S = printFunction(M, *F);
  EXPECT_NE(S.find("try"), std::string::npos);
  EXPECT_NE(S.find("catch"), std::string::npos);
}

TEST(StructuredStmtBuilderTest, SehTryExcept) {
  LLVMContext &C = ctx();
  llvm::Module M("t_seh", C);
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

} // namespace
