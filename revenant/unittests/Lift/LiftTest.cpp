//===- unittests/Lift/LiftTest.cpp - function lifter checks -------------===//
//
// Lift a hand-built leaf function over real c-o0.exe bytes: decode the
// first pdata function's entry bytes linearly into one block, lift with the
// function lifter, and check LLVM verification plus the IR contract
// (revenant.va on the function, bb_<VA> blocks, !revenant.va on every
// instruction, cleanup pipeline still verifying).
//
//===----------------------------------------------------------------------===//

#include "LiftTestUtil.h"
#include "revenant/Analysis/Analysis.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/IR/Contract.h"
#include "revenant/Lift/LiftFunction.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include "gtest/gtest.h"

#include <cstdlib>
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
using lifttest::emptyProgram;
using lifttest::singleBlockFunc;


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

TEST(LiftFunction, FullCfgCorpusSamplesVerify) {
  const struct {
    const char *Name;
    const char *Path;
  } Inputs[] = {{"c-o0", REVENANT_TEST_C_O0},
                {"c-o2", REVENANT_TEST_C_O2},
                {"cpp-o2", REVENANT_TEST_CPP_O2},
                {"c-clang-o2", REVENANT_TEST_C_CLANG_O2},
                {"c-gcc-o2", REVENANT_TEST_C_GCC_O2}};
  const char *Only = std::getenv("REVENANT_LIFT_ONLY");
  const char *DumpVa = std::getenv("REVENANT_LIFT_DUMP_VA");
  for (const auto &Input : Inputs) {
    if (Only && llvm::StringRef(Only) != Input.Name)
      continue;
    auto Fx = open(Input.Path);
    ASSERT_TRUE(Fx.has_value()) << Input.Path;
    auto Hooks = createX86TargetHooks();
    Program P = analyze(*Fx->Pe, *Hooks);
    ASSERT_FALSE(P.Functions.empty()) << Input.Name;

    llvm::LLVMContext C;
    llvm::Module M(Input.Name, C);
    std::map<std::string, unsigned> LiftErrors;
    std::map<std::string, unsigned> VerifierErrors;
    std::map<std::string, unsigned> Fallbacks;
    unsigned Verified = 0;
    unsigned LiftFailed = 0;
    for (const auto &[Start, Record] : P.Functions) {
      auto Lifted = lift::liftFunction(M, P, Record, *Fx->Dis, nullptr,
                                       nullptr, /*RunPasses=*/false);
      if (!Lifted) {
        std::string Message = toString(Lifted.takeError());
        llvm::StringRef Ref(Message);
        std::string Category =
            Ref.contains("inconsistent rsp delta") ? "stack-delta"
            : Ref.contains("decode")              ? "decode"
                                                   : "other";
        if (Ref.contains("LLVM verification failed")) {
          for (const BlockRec &BR : Record.Blocks) {
            llvm::errs() << "    block " << BR.startVa(*P.Img) << "\n";
            for (size_t N = BR.InstrsBegin; N < BR.InstrsEnd; ++N) {
              auto I = Fx->Dis->decodeOne(
                  P.Img->Base.wrappingAdd(Record.InstrRvas[N]));
              if (I)
                llvm::errs() << "      " << Fx->Dis->format(*I) << "\n";
            }
          }
        }
        ++LiftErrors[Category];
        llvm::errs() << "  lift-failure at " << Start.Value << ": "
                     << Message << "\n";
        ++LiftFailed;
        continue;
      }
      for (const auto &[VA, Text] : Lifted->AsmFallbacks) {
        llvm::StringRef S(Text);
        ++Fallbacks[S.take_front(S.find(" (")).trim().lower()];
      }
      if (llvm::verifyFunction(*Lifted->Fn, &llvm::errs())) {
        ++VerifierErrors["function-verifier"];
        llvm::errs() << "  verify-failure at " << Start.Value << "\n";
        continue;
      }
      ++Verified;
    }
    if (DumpVa) {
      uint64_t Want = 0;
      llvm::StringRef(DumpVa).getAsInteger(16, Want);
      uint64_t Best = 0;
      for (const auto &[Start, Record] : P.Functions)
        if (Start.Value <= Want)
          Best = Start.Value;
      for (llvm::Function &Fn : M)
        if (!Fn.isDeclaration() &&
            revenant::ir::getFunctionVA(Fn).value_or(0) == Best)
          Fn.print(llvm::errs());
    }
    bool ModuleVerified = !verifyModule(M, &llvm::errs());
    unsigned Total = P.Functions.size();
    llvm::errs() << Input.Name << ": " << Verified << "/" << Total
                 << " functions verified; lift failures=" << LiftFailed
                 << "; module verifier=" << (ModuleVerified ? "pass" : "fail")
                 << "\n";
    for (const auto &[Category, Count] : LiftErrors)
      llvm::errs() << "  lift-error " << Category << ": " << Count << "\n";
    for (const auto &[Category, Count] : VerifierErrors)
      llvm::errs() << "  verify-error " << Category << ": " << Count << "\n";
    for (const auto &[Mnemonic, Count] : Fallbacks)
      llvm::errs() << "  opaque " << Mnemonic << ": " << Count << "\n";
    EXPECT_TRUE(ModuleVerified) << Input.Name;
    EXPECT_GE(Verified * 100, Total * 99) << Input.Name;
  }
}

} // end anonymous namespace
