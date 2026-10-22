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
 #include "revenant/Analysis/Signatures.h"
 #include "revenant/Disasm/Disasm.h"
 #include "revenant/IR/Contract.h"
 #include "revenant/Lift/LiftFunction.h"
 #include "revenant/Object/PeFile.h"
 #include "revenant/Target/TargetHooks.h"
 #include "X86MCTargetDesc.h"

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
#include "llvm/IR/Constants.h"
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

TEST(LiftFunction, TailThunkWithDifferentlyTypedReturnVerifies) {
  auto Fx = open(REVENANT_TEST_PARITY "/fmt-cl-o2gl/fmt-cl-o2gl.exe");
  ASSERT_TRUE(Fx.has_value());
  Program P = analyze(*Fx->Pe, *createX86TargetHooks());
  const Va Thunk(0x14007AA00);
  const Va Callee(0x1400FD430);
  ASSERT_TRUE(P.Functions.count(Thunk));
  ASSERT_TRUE(P.Functions.count(Callee));

  lift::Signatures Sigs;
  Sigs.Funcs[Thunk].Ret = lift::RetLoc{false, 64};
  Sigs.Funcs[Callee].Ret = lift::RetLoc{true, 64};
  llvm::LLVMContext C;
  llvm::Module M("tail_return", C);
  auto Lifted = lift::liftFunction(M, P, P.Functions.at(Thunk), *Fx->Dis,
                                   &Sigs, nullptr, /*RunPasses=*/false);
  if (!Lifted)
    FAIL() << toString(Lifted.takeError());
  EXPECT_TRUE(Lifted->Fn->getReturnType()->isIntegerTy(64));
  const auto *Ret =
      llvm::dyn_cast<llvm::ReturnInst>(Lifted->Fn->back().getTerminator());
  ASSERT_NE(Ret, nullptr);
  EXPECT_TRUE(llvm::isa<llvm::BitCastInst>(Ret->getReturnValue()));
  EXPECT_FALSE(verifyModule(M, &llvm::errs()));
}

TEST(LiftFunction, FloatTailThunkFromIntegerCalleeVerifies) {
  auto Fx = open(REVENANT_TEST_PARITY "/spdlog-gcc-o0/spdlog-gcc-o0.exe");
  ASSERT_TRUE(Fx.has_value());
  Program P = analyze(*Fx->Pe, *createX86TargetHooks());
  const Va Thunk(0x1400C0170);
  const Va Callee(0x1400BEE00);
  ASSERT_TRUE(P.Functions.count(Thunk));
  ASSERT_TRUE(P.Functions.count(Callee));

  lift::Signatures Sigs;
  Sigs.Funcs[Thunk].Ret = lift::RetLoc{true, 32};
  Sigs.Funcs[Callee].Ret = lift::RetLoc{false, 64};
  llvm::LLVMContext C;
  llvm::Module M("float_tail_return", C);
  auto Lifted = lift::liftFunction(M, P, P.Functions.at(Thunk), *Fx->Dis,
                                   &Sigs, nullptr, /*RunPasses=*/false);
  if (!Lifted)
    FAIL() << toString(Lifted.takeError());
  EXPECT_TRUE(Lifted->Fn->getReturnType()->isFloatTy());
  const auto *Ret =
      llvm::dyn_cast<llvm::ReturnInst>(Lifted->Fn->back().getTerminator());
  ASSERT_NE(Ret, nullptr);
  EXPECT_TRUE(llvm::isa<llvm::BitCastInst>(Ret->getReturnValue()));
  EXPECT_FALSE(verifyModule(M, &llvm::errs()));
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
  // The five-binary matrix plus the binaries behind earlier lifter crashes
  // (entry block that is a branch target; invalid casts in Rust code).
  const struct {
    const char *Name;
    const char *Path;
  } Inputs[] = {
      {"c-o0", REVENANT_TEST_C_O0},
      {"c-o2", REVENANT_TEST_C_O2},
      {"cpp-o2", REVENANT_TEST_CPP_O2},
      {"c-clang-o2", REVENANT_TEST_C_CLANG_O2},
      {"c-gcc-o2", REVENANT_TEST_C_GCC_O2},
      {"json-cl-o2", REVENANT_TEST_PARITY "/json-cl-o2/json-cl-o2.exe"},
      {"rust-o0", REVENANT_TEST_PARITY "/rust-o0/corpus_rust.exe"},
      {"rust-o3", REVENANT_TEST_PARITY "/rust-o3/corpus_rust.exe"}};
  const char *Only = std::getenv("REVENANT_LIFT_ONLY");
  const char *DumpVa = std::getenv("REVENANT_LIFT_DUMP_VA");
  const bool Verbose = std::getenv("REVENANT_LIFT_VERBOSE") != nullptr;
  // REVENANT_LIFT_EXE=<pe> lifts one more binary (named "extra").
  std::vector<std::pair<std::string, std::string>> All;
  for (const auto &In : Inputs)
    All.push_back({In.Name, In.Path});
  if (const char *Extra = std::getenv("REVENANT_LIFT_EXE"))
    All.push_back({"extra", Extra});
  for (const auto &Input : All) {
    if (Only && llvm::StringRef(Only) != Input.first)
      continue;
    auto Fx = open(Input.second);
    ASSERT_TRUE(Fx.has_value()) << Input.second;
    auto Hooks = createX86TargetHooks();
    Program P = analyze(*Fx->Pe, *Hooks);
    ASSERT_FALSE(P.Functions.empty()) << Input.first;

    llvm::LLVMContext C;
    llvm::Module M(Input.first, C);
    std::map<std::string, unsigned> LiftErrors;
    std::map<std::string, unsigned> VerifierErrors;
    std::map<std::string, unsigned> Fallbacks;
    unsigned Verified = 0;
    unsigned LiftFailed = 0;
    for (const auto &[Start, Record] : P.Functions) {
      if (Verbose)
        llvm::errs() << "lift " << llvm::utohexstr(Start.Value) << "\n";
      auto Lifted = lift::liftFunction(M, P, Record, *Fx->Dis, nullptr,
                                       nullptr, /*RunPasses=*/false);
      if (!Lifted) {
        std::string Message = toString(Lifted.takeError());
        llvm::StringRef Ref(Message);
        std::string Category =
            Ref.contains("inconsistent rsp delta") ? "stack-delta"
            : Ref.contains("decode")              ? "decode"
                                                   : "other";
        if (Ref.contains("LLVM verification failed") ||
            (Verbose && Ref.contains("inconsistent rsp"))) {
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
    llvm::errs() << Input.first << ": " << Verified << "/" << Total
                 << " functions verified; lift failures=" << LiftFailed
                 << "; module verifier=" << (ModuleVerified ? "pass" : "fail")
                 << "\n";
    for (const auto &[Category, Count] : LiftErrors)
      llvm::errs() << "  lift-error " << Category << ": " << Count << "\n";
    for (const auto &[Category, Count] : VerifierErrors)
      llvm::errs() << "  verify-error " << Category << ": " << Count << "\n";
    for (const auto &[Mnemonic, Count] : Fallbacks)
      llvm::errs() << "  opaque " << Mnemonic << ": " << Count << "\n";
    EXPECT_TRUE(ModuleVerified) << Input.first;
    EXPECT_GE(Verified * 100, Total * 99) << Input.first;
  }
}

TEST(LiftFunction, SoftwareInterruptsLift) {
  // `int imm8` lifts to a call of llvm.x86.int(vector): int3 is vector 3,
  // int 0x29 (__fastfail) additionally marks the call site noreturn.
  const uint8_t Code[] = {0xCD, 0x29, 0xCC, 0xC3}; // int 0x29; int3; ret
  auto Img = std::make_shared<Image>();
  Img->Base = Va(0x140000000);
  Section Sec;
  Sec.Name = ".text";
  Sec.SecVa = Va(0x140001000);
  Sec.Vsize = sizeof(Code);
  Sec.Data.assign(Code, Code + sizeof(Code));
  Sec.Flags = SectionFlags::rx();
  Img->Sections.push_back(Sec);
  auto D = Disasm::create(*Img);
  ASSERT_FALSE(!D) << toString(D.takeError());
  std::vector<Instr> Block =
      D->decodeSlice(Sec.SecVa, llvm::ArrayRef(Code, sizeof(Code)));
  ASSERT_EQ(Block.size(), 3u);

  Program P = emptyProgram(Img);
  FuncRecord R = singleBlockFunc(P, Block);
  llvm::LLVMContext C;
  llvm::Module M("int_trap", C);
  auto Lifted = lift::liftFunction(M, P, R, *D, nullptr, nullptr,
                                   /*RunPasses=*/false);
  ASSERT_FALSE(!Lifted) << toString(Lifted.takeError());

  const llvm::Function *IntDecl = M.getFunction("llvm.x86.int");
  ASSERT_NE(IntDecl, nullptr) << "no llvm.x86.int declaration";
  std::vector<const llvm::CallInst *> Calls;
  for (const llvm::User *U : IntDecl->users())
    if (const auto *CI = llvm::dyn_cast<llvm::CallInst>(U))
      Calls.push_back(CI);
  ASSERT_EQ(Calls.size(), 2u);
  auto VectorOf = [](const llvm::CallInst *CI) {
    const auto *Imm =
        llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(0));
    return Imm ? Imm->getZExtValue() : ~0ull;
  };
  const llvm::CallInst *FastFail = nullptr;
  const llvm::CallInst *DebugTrap = nullptr;
  for (const llvm::CallInst *CI : Calls) {
    if (VectorOf(CI) == 0x29)
      FastFail = CI;
    if (VectorOf(CI) == 3)
      DebugTrap = CI;
  }
  ASSERT_NE(FastFail, nullptr);
  ASSERT_NE(DebugTrap, nullptr);
  EXPECT_TRUE(FastFail->doesNotReturn());
  EXPECT_FALSE(DebugTrap->doesNotReturn());
}

TEST(LiftFunction, CalleeClobberPreservesLoopIv) {
  // c-gcc-o2 map_call_vec (0x140001ea0) keeps its loop induction variable in
  // r8/r9 across a call to the local leaf sub_140001cc0, which only writes
  // eax/ecx. A call that clobbered every volatile register would leave the
  // loop's `cmp r8, r9` branch condition undefined.
  auto Fx = open(REVENANT_TEST_C_GCC_O2);
  ASSERT_TRUE(Fx.has_value());
  auto Hooks = createX86TargetHooks();
  Program P = analyze(*Fx->Pe, *Hooks);
  Va Start(0x140001ea0);
  auto It = P.Functions.find(Start);
  ASSERT_NE(It, P.Functions.end()) << "map_call_vec not recovered";

  llvm::LLVMContext C;
  llvm::Module M("callee_clobber", C);
  auto Lifted = lift::liftFunction(M, P, It->second, *Fx->Dis, nullptr,
                                   nullptr, /*RunPasses=*/false);
  ASSERT_FALSE(!Lifted) << toString(Lifted.takeError());
  ASSERT_FALSE(verifyModule(M, &llvm::errs()));

  // The call to the leaf must not store undef into r8/r9 (the loop IV).
  const llvm::Function *Leaf = M.getFunction("sub_140001CC0");
  ASSERT_NE(Leaf, nullptr);
  for (const llvm::User *U : Leaf->users())
    if (const auto *CI = llvm::dyn_cast<llvm::CallInst>(U))
      for (const llvm::User *CU : CI->users())
        if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(CU))
          ASSERT_FALSE(llvm::isa<llvm::UndefValue>(SI->getValueOperand()))
              << "call clobbered a register the leaf does not write";

  // No conditional branch may test an undefined condition.
  for (const llvm::BasicBlock &BB : *Lifted->Fn)
    if (const auto *BI = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator()))
      if (BI->isConditional())
        ASSERT_FALSE(llvm::isa<llvm::UndefValue>(BI->getCondition()))
            << "branch on undef in " << BB.getName().str();
}

TEST(LiftFunction, WhileLoopKeepsDistinctSuccessors) {
  // c-o2 fn 0x1400015a0: the bb_1400015e0 while-loop block once ended in a
  // `br i1` with identical successors (the loop's `cmp ecx, 5 / jg` exit
  // condition was dropped). The loop must keep a real exit comparison to
  // two different blocks.
  auto Fx = open(REVENANT_TEST_C_O2);
  ASSERT_TRUE(Fx.has_value());
  auto Hooks = createX86TargetHooks();
  Program P = analyze(*Fx->Pe, *Hooks);
  Va Start(0x1400015a0);
  auto It = P.Functions.find(Start);
  ASSERT_NE(It, P.Functions.end()) << "fn 0x1400015a0 not recovered";

  llvm::LLVMContext C;
  llvm::Module M("while_loop_exit", C);
  auto Lifted = lift::liftFunction(M, P, It->second, *Fx->Dis, nullptr,
                                   nullptr, /*RunPasses=*/false);
  ASSERT_FALSE(!Lifted) << toString(Lifted.takeError());
  ASSERT_FALSE(verifyModule(M, &llvm::errs()));

  const llvm::BasicBlock *Loop = nullptr;
  for (const llvm::BasicBlock &BB : *Lifted->Fn)
    if (BB.getName() == "bb_1400015e0")
      Loop = &BB;
  ASSERT_NE(Loop, nullptr) << "loop block bb_1400015e0 missing";
  const auto *BI = llvm::dyn_cast<llvm::BranchInst>(Loop->getTerminator());
  ASSERT_NE(BI, nullptr);
  ASSERT_TRUE(BI->isConditional())
      << "loop block lost its exit condition";
  EXPECT_NE(BI->getSuccessor(0), BI->getSuccessor(1))
      << "loop block branches to the same block twice";
  ASSERT_FALSE(llvm::isa<llvm::UndefValue>(BI->getCondition()))
      << "loop exit condition is undef";
}

TEST(LiftFunction, ThunkCallLowersToDirectImport) {
  // c-gcc-o2 sub_140001A10 calls the single-`jmp [strcmp-slot]` XFG stub
  // sub_140003968. The call must lower as a direct import call (the shared
  // `call [mem]` path), not as an opaque stub reference: the stubs are
  // library-named alike, so stub references collide at recompile.
  auto Fx = open(REVENANT_TEST_C_GCC_O2);
  ASSERT_TRUE(Fx.has_value());
  auto Hooks = createX86TargetHooks();
  Program P = analyze(*Fx->Pe, *Hooks);
  Va Start(0x140001a10);
  auto It = P.Functions.find(Start);
  ASSERT_NE(It, P.Functions.end()) << "fn 0x140001a10 not recovered";

  llvm::LLVMContext C;
  llvm::Module M("thunk_import", C);
  auto Lifted = lift::liftFunction(M, P, It->second, *Fx->Dis, nullptr,
                                   nullptr, /*RunPasses=*/false);
  ASSERT_FALSE(!Lifted) << toString(Lifted.takeError());
  ASSERT_FALSE(verifyModule(M, &llvm::errs()));

  const llvm::Function *Strcmp = M.getFunction("strcmp");
  ASSERT_NE(Strcmp, nullptr) << "no direct strcmp import call";
  bool CallsStrcmp = false;
  for (const llvm::User *U : Strcmp->users())
    if (llvm::isa<llvm::CallInst>(U))
      CallsStrcmp = true;
  EXPECT_TRUE(CallsStrcmp) << "strcmp import declared but never called";
  EXPECT_EQ(M.getFunction("sub_140003968"), nullptr)
      << "opaque XFG stub still referenced";
}
// Production callee shapes for c-gcc-o2 main: real analyzeSignatures
// output converted the same way Stages.cpp convertSig does (hand-made
// fragments proved treacherous: a missing entry mints a void decl and
// the return writeback silently vanishes).
static lift::Signatures gccO2MainSigs(const Program &P, const Disasm &D) {
  revenant::Signatures A = revenant::analyzeSignatures(P, D);
  lift::Signatures Sigs;
  for (const auto &[At, S] : A.Funcs) {
    lift::Signature Out;
    for (const revenant::Param &Pm : S.Params) {
      lift::Param LP;
      LP.Loc = Pm.Loc.isStack() ? lift::ParamLoc::stack(Pm.Loc.Off)
                                : lift::ParamLoc::reg(llvm::MCRegister(Pm.Loc.Reg));
      LP.Bits = Pm.Bits;
      LP.Float = Pm.Float;
      Out.Params.push_back(std::move(LP));
    }
    if (S.HasRet)
      Out.Ret = lift::RetLoc{S.Ret.Float, S.Ret.Bits};
    Out.NoReturn = S.Noreturn;
    Sigs.Funcs.emplace(At, std::move(Out));
  }
  return Sigs;
}

} // end anonymous namespace
