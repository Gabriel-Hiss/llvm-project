//===- unittests/Lift/LiftTest.cpp - function lifter checks -------------===//
//
// Lift a hand-built leaf function over real c-o0.exe bytes: decode the
// first pdata function's entry bytes linearly into one block, lift with the
// function lifter, and check LLVM verification plus the IR contract
// (revenant.va on the function, bb_<VA> blocks, !revenant.va on every
// instruction, cleanup pipeline still verifying).
//
//===----------------------------------------------------------------------===//

#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"
#include "revenant/Lift/LiftFunction.h"
#include "revenant/Object/PeFile.h"

#include "gtest/gtest.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

// revenant::Function/BasicBlock collide with llvm::Function/BasicBlock;
// qualify every LLVM type below.

namespace {
using namespace revenant;


struct Fixture {
  std::unique_ptr<llvm::MemoryBuffer> Buf;
  std::optional<PeFile> Pe;
  std::optional<Disasm> Dis;
};

static std::optional<Fixture> open(llvm::StringRef Path) {
  Fixture F;
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return std::nullopt;
  F.Buf = std::move(*Buf);
  llvm::StringRef Data = F.Buf->getBuffer();
  auto Pe = PeFile::parse(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  if (!Pe) {
    consumeError(Pe.takeError());
    return std::nullopt;
  }
  F.Pe = std::move(*Pe);
  auto D = Disasm::create(*F.Pe->Img);
  if (!D) {
    consumeError(D.takeError());
    return std::nullopt;
  }
  F.Dis = std::move(*D);
  return F;
}

// Linearly decode up to Max instructions from Start; stop at ret/jmp.
static std::vector<Instr> linearBlock(const Disasm &D, Va Start,
                                      size_t Max = 24) {
  std::vector<Instr> Out;
  Va Ip = Start;
  for (size_t K = 0; K < Max; ++K) {
    auto I = D.decodeOne(Ip);
    if (!I)
      break;
    FlowKind Fl = I->Flow;
    Out.push_back(std::move(*I));
    Ip = Out.back().nextVa();
    if (Fl == FlowKind::Return || Fl == FlowKind::Branch ||
        Fl == FlowKind::IndirectBranch || Fl == FlowKind::Trap)
      break;
  }
  return Out;
}

static FuncRecord singleBlockFunc(const Program &P,
                                  const std::vector<Instr> &Block) {
  FuncRecord R;
  R.Start = Block.front().Addr;
  R.End = Block.back().nextVa();
  auto RvaOf = [&](Va V) -> Rva { return *P.Img->rvaOf(V); };
  for (const Instr &I : Block)
    R.InstrRvas.push_back(RvaOf(I.Addr));
  for (uint32_t K = 0; K < Block.size(); ++K)
    R.Sorted.push_back(K);
  BlockRec BR;
  BR.Start = RvaOf(Block.front().Addr);
  BR.End = RvaOf(Block.back().nextVa());
  BR.InstrsBegin = 0;
  BR.InstrsEnd = (uint32_t)Block.size();
  BR.EdgesBegin = 0;
  BR.EdgesEnd = 0;
  BR.EndKind = BlockEnd::Return;
  R.Blocks.push_back(BR);
  R.Chunks.push_back({R.Start, R.End});
  return R;
}

static Program emptyProgram(std::shared_ptr<Image> Img) {
  Program P;
  P.Img = std::move(Img);
  P.Strings = std::make_shared<std::map<Va, std::string>>();
  P.Externs = std::make_shared<std::map<Va, Extern>>();
  P.Names = std::make_shared<std::map<Va, std::string>>();
  P.Resolver = std::make_shared<NoNames>();
  return P;
}

TEST(LiftFunction, LeafBlockOverRealBytes) {
  auto Fx = open(REVENANT_TEST_C_O0);
  ASSERT_TRUE(Fx.has_value());
  ASSERT_FALSE(Fx->Pe->Pdata.empty());
  Va Start = Fx->Pe->Pdata.front().Begin;

  std::vector<Instr> Block = linearBlock(*Fx->Dis, Start);
  ASSERT_GE(Block.size(), 2u);

  Program P = emptyProgram(Fx->Pe->Img);
  FuncRecord R = singleBlockFunc(P, Block);

  llvm::LLVMContext C;
  llvm::Module M("lift_test", C);
  auto Lifted = lift::liftFunction(M, P, R, *Fx->Dis, nullptr, nullptr,
                                   /*RunPasses=*/false);
  if (!Lifted)
    FAIL() << toString(Lifted.takeError());
  EXPECT_FALSE(verifyModule(M, &llvm::errs()));

  // Contract: function VA attr, bb_<VA> entry block, provenance on
  // every instruction.
  EXPECT_TRUE(Lifted->Fn->hasFnAttribute("revenant.va"));
  ASSERT_FALSE(Lifted->Fn->empty());
  EXPECT_TRUE(Lifted->Fn->front().getName().starts_with("bb_"));
  for (const llvm::BasicBlock &BB : *Lifted->Fn) {
    EXPECT_TRUE(BB.hasName());
    for (const llvm::Instruction &I : BB)
      EXPECT_TRUE(ir::getInstructionVA(I).has_value())
          << "missing !revenant.va in " << BB.getName().str();
  }

  // Opaque callees (if any) are declares with the marker on call sites.
  for (const llvm::Function &Fn : M.functions()) {
    if (!ir::isOpaqueAsmCallee(Fn))
      continue;
    EXPECT_TRUE(Fn.isDeclaration());
    EXPECT_FALSE(Fn.hasFnAttribute("revenant.va"));
  }
  unsigned Diags = ir::verifyModuleContract(
      M, [&](ir::ContractSeverity, llvm::StringRef) {});
  // liftFunction sets triple/datalayout/lang, so expect zero violations.
  EXPECT_EQ(Diags, 0u);
}

TEST(LiftFunction, CleanupPipelineVerifies) {
  auto Fx = open(REVENANT_TEST_C_O0);
  ASSERT_TRUE(Fx.has_value());
  ASSERT_FALSE(Fx->Pe->Pdata.empty());
  Va Start = Fx->Pe->Pdata.front().Begin;
  std::vector<Instr> Block = linearBlock(*Fx->Dis, Start);
  ASSERT_GE(Block.size(), 2u);

  Program P = emptyProgram(Fx->Pe->Img);
  FuncRecord R = singleBlockFunc(P, Block);

  llvm::LLVMContext C;
  llvm::Module M("lift_cleanup", C);
  auto Lifted = lift::liftFunction(M, P, R, *Fx->Dis, nullptr, nullptr,
                                   /*RunPasses=*/true);
  if (!Lifted)
    FAIL() << toString(Lifted.takeError());
  EXPECT_FALSE(verifyModule(M, &llvm::errs()));
}

TEST(LiftFunction, RealPdataSamplesVerify) {
  const struct {
    const char *Name;
    const char *Path;
  } Inputs[] = {{"c-o0", REVENANT_TEST_C_O0},
                {"c-o2", REVENANT_TEST_C_O2},
                {"cpp-o2", REVENANT_TEST_CPP_O2}};
  for (const auto &Input : Inputs) {
    auto Fx = open(Input.Path);
    ASSERT_TRUE(Fx.has_value()) << Input.Path;
    unsigned Tried = 0;
    unsigned Verified = 0;
    unsigned Failed = 0;
    for (const RuntimeFunction &RF : Fx->Pe->Pdata) {
      if (Tried == 12)
        break;
      std::vector<Instr> Block = Fx->Dis->decodeRange(RF.Begin, RF.End);
      if (Block.empty())
        continue;
      ++Tried;
      Program P = emptyProgram(Fx->Pe->Img);
      FuncRecord R = singleBlockFunc(P, Block);
      llvm::LLVMContext C;
      llvm::Module M(Input.Name, C);
      auto Lifted = lift::liftFunction(M, P, R, *Fx->Dis, nullptr, nullptr,
                                       /*RunPasses=*/false);
      if (!Lifted) {
        consumeError(Lifted.takeError());
        ++Failed;
        continue;
      }
      if (verifyModule(M, &llvm::errs()))
        ++Failed;
      else
        ++Verified;
    }
    llvm::errs() << Input.Name << ": " << Verified << "/" << Tried
                 << " pdata functions verified (" << Failed << " failed)\n";
    EXPECT_EQ(Verified + Failed, Tried);
    EXPECT_GT(Verified, 0u);
  }
}

} // end anonymous namespace
