//===- unittests/Evidence/EvidenceTest.cpp - evidence query tests --------===//
//
// Part of the Revenant LLVM fork.
//
// One test per evidence kind in revenant/Evidence/Evidence.h: unroll
// factors, inline location/string/lib evidence, chunk ranges, EH regions,
// and the absent-metadata defaults that keep `--meta=none` output valid.
// Fixtures build IR in memory with the same metadata shapes the lifter
// attaches, so no binary is needed.
//
//===----------------------------------------------------------------------===//

#include "revenant/Evidence/Evidence.h"

#include "gtest/gtest.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;
namespace ev = revenant::evidence;

namespace {

Function *makeFn(Module &M, StringRef Name) {
  FunctionType *FT = FunctionType::get(Type::getVoidTy(M.getContext()), false);
  return Function::Create(FT, Function::ExternalLinkage, Name, M);
}

MDNode *i32Node(LLVMContext &C, int32_t V) {
  return MDNode::get(C, ConstantAsMetadata::get(
                            ConstantInt::get(Type::getInt32Ty(C), V)));
}

MDNode *i64PairNode(LLVMContext &C, uint64_t Lo, uint64_t Hi) {
  Metadata *Ends[] = {
      ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt64Ty(C), Lo)),
      ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(C), Hi))};
  return MDNode::get(C, Ends);
}

} // namespace

TEST(EvidenceTest, UnrollFactorReadsRecordedFactor) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "loop");
  F->setMetadata(ev::MDUnroll, i32Node(C, 4));
  EXPECT_EQ(ev::unrollFactor(*F), std::optional<unsigned>(4));
}

TEST(EvidenceTest, UnrollFactorAbsentWithoutMetadata) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "plain");
  EXPECT_EQ(ev::unrollFactor(*F), std::nullopt);
}

TEST(EvidenceTest, UnrollFactorRejectsMalformed) {
  LLVMContext C;
  Module M("e", C);
  // Factor below 2 is not an unroll.
  Function *One = makeFn(M, "one");
  One->setMetadata(ev::MDUnroll, i32Node(C, 1));
  EXPECT_EQ(ev::unrollFactor(*One), std::nullopt);
  // Wrong operand type is not evidence.
  Function *Str = makeFn(M, "str");
  Str->setMetadata(ev::MDUnroll,
                   MDNode::get(C, MDString::get(C, "four")));
  EXPECT_EQ(ev::unrollFactor(*Str), std::nullopt);
}

TEST(EvidenceTest, InlineEvidenceReadsLocationStringsLib) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "callee");
  Metadata *Loc[] = {MDString::get(C, "src/lib.rs"),
                     ConstantAsMetadata::get(
                         ConstantInt::get(Type::getInt32Ty(C), 10)),
                     ConstantAsMetadata::get(
                         ConstantInt::get(Type::getInt32Ty(C), 5))};
  F->setMetadata(ev::MDSource, MDNode::get(C, Loc));
  F->setMetadata(ev::MDStrings,
                 MDNode::get(C, MDString::get(C, "assertion failed")));
  F->setMetadata(ev::MDPanic,
                 MDNode::get(C, MDString::get(C, "index out of bounds")));
  F->addFnAttr(ev::FnAttrLib, "memcpy");
  ev::InlineEvidence E = ev::uninlineEvidence(*F);
  EXPECT_FALSE(E.empty());
  ASSERT_TRUE(E.Location.has_value());
  EXPECT_EQ(*E.Location, "src/lib.rs:10:5");
  EXPECT_EQ(E.Strings.size(), 2u);
  ASSERT_TRUE(E.Library.has_value());
  EXPECT_EQ(*E.Library, "memcpy");
}

TEST(EvidenceTest, InlineEvidenceEmptyWithoutMetadata) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "bare");
  EXPECT_TRUE(ev::uninlineEvidence(*F).empty());
}

TEST(EvidenceTest, RegionMatchesEvidenceOnSharedString) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "callee");
  F->setMetadata(ev::MDStrings,
                 MDNode::get(C, MDString::get(C, "assertion failed")));
  ev::InlineEvidence E = ev::uninlineEvidence(*F);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  auto *StrTy = ArrayType::get(Type::getInt8Ty(C), 17);
  auto *G = new GlobalVariable(M, StrTy, true,
                              GlobalValue::InternalLinkage,
                              ConstantDataArray::getString(C, "assertion failed"),
                              "str_assert");
  auto *Load = B.CreateLoad(PointerType::getUnqual(C), G);
  SmallVector<Instruction *, 2> Region;
  Region.push_back(Load);
  EXPECT_TRUE(ev::regionMatchesEvidence(E, Region));
}

TEST(EvidenceTest, RegionMatchesEvidenceFalseWithoutEvidence) {
  LLVMContext C;
  Module M("e", C);
  FunctionType *FT = FunctionType::get(Type::getInt32Ty(C),
                                       {Type::getInt32Ty(C)}, false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, "mw", M);
  ev::InlineEvidence E = ev::uninlineEvidence(*F);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Instruction *Other = cast<Instruction>(B.CreateAdd(F->getArg(0), B.getInt32(1)));
  SmallVector<Instruction *, 2> Region;
  Region.push_back(Other);
  EXPECT_FALSE(ev::regionMatchesEvidence(E, Region));
}

TEST(EvidenceTest, ChunkRangesReadSplitRanges) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "split");
  Metadata *Ranges[] = {i64PairNode(C, 0x140001000, 0x140001100),
                        i64PairNode(C, 0x140038000, 0x140038050)};
  F->setMetadata(ev::MDChunks, MDNode::get(C, Ranges));
  auto Got = ev::chunkRanges(*F);
  ASSERT_EQ(Got.size(), 2u);
  EXPECT_EQ(Got[0], std::make_pair(0x140001000ull, 0x140001100ull));
  EXPECT_EQ(Got[1], std::make_pair(0x140038000ull, 0x140038050ull));
}

TEST(EvidenceTest, ChunkRangesEmptyWhenContiguous) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "hot");
  EXPECT_TRUE(ev::chunkRanges(*F).empty());
}

TEST(EvidenceTest, EhRegionReadsInvokeRegion) {
  LLVMContext C;
  Module M("e", C);
  Function *F = makeFn(M, "eh");
  BasicBlock *BB = BasicBlock::Create(C, "bb_140001000", F);
  IRBuilder<> B(BB);
  FunctionType *VT = FunctionType::get(Type::getVoidTy(C), false);
  Function *Callee =
      Function::Create(VT, Function::ExternalLinkage, "may_throw", M);
  CallInst *Call = B.CreateCall(VT, Callee);
  EXPECT_EQ(ev::ehRegion(*Call), std::nullopt);
  Call->setMetadata("revenant.eh.region", i32Node(C, 3));
  EXPECT_EQ(ev::ehRegion(*Call), std::optional<int32_t>(3));
}
