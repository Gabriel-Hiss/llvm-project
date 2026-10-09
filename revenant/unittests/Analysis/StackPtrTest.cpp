//==- unittests/Analysis/StackPtrTest.cpp - stack delta checks ---*- C++ -*-==//
//
// Port of the stack-delta unit test in crates/lift/tests/lift.rs
// (`stack_delta_unit`) plus coverage of the reference behaviors in
// crates/analysis/src/stackptr.rs: chkstk probes (const size adds,
// missing size marks dynamic), leave/mov-rsp-rbp restore, dynamic writes
// (sub rsp,reg), inconsistent joins failing, rbp-relative slots, and the
// StackInfo accessors (rspOff/rbpOff/isDynamicAt).
//
// The tests build synthetic single-block functions by decoding real bytes
// with RevenantDisasm (so MC operand shapes are exactly what the hooks
// see in production) and drive analyzeStack through a test TargetHooks
// that delegates the six stack queries to x86_stack (the same code
// LunaDiscovery wires into X86TargetHooks).
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/StackPtr.h"
#include "revenant/Core/Image.h"
#include "revenant/Disasm/Disasm.h"

#include "revenant/Target/X86StackEffects.h"

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"

#include "gtest/gtest.h"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

using namespace revenant;

namespace {

struct X86Tables {
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
  std::unique_ptr<llvm::MCInstrInfo> MII;
};

const X86Tables &x86Tables() {
  static std::once_flag Flag;
  static std::unique_ptr<X86Tables> Tables;
  std::call_once(Flag, [] {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    llvm::Triple TT("x86_64-pc-windows-msvc");
    std::string Err;
    const llvm::Target *T = llvm::TargetRegistry::lookupTarget(TT, Err);
    auto X = std::make_unique<X86Tables>();
    if (T) {
      X->MRI.reset(T->createMCRegInfo(TT));
      X->MII.reset(T->createMCInstrInfo());
    }
    Tables = std::move(X);
  });
  return *Tables;
}

/// Test hooks: stack queries delegate to x86_stack, the production code
/// under test; unrelated target queries are neutral.
class TestHooks final : public TargetHooks {
public:
  llvm::StringRef mnemonic(const Instr &) const override { return {}; }
  bool isFiller(const Instr &) const override { return false; }
  TrapKind trapKind(const Instr &) const override { return TrapKind::NotTrap; }
  bool isNopOrInt3(const Instr &) const override { return false; }
  bool isOpaqueTailJmp(const Image &, const Instr &) const override {
    return false;
  }
  std::optional<int64_t> stackDelta(const Instr &I) const override {
    return x86_stack::stackDelta(*x86Tables().MII, *x86Tables().MRI, I);
  }
  StackSlots stackSlots(const Instr &I) const override {
    return x86_stack::stackSlots(*x86Tables().MII, *x86Tables().MRI, I);
  }
  FpEstablish fpEstablish(const Instr &I) const override {
    return x86_stack::fpEstablish(*x86Tables().MII, *x86Tables().MRI, I);
  }
  bool restoresSpFromFp(const Instr &I) const override {
    return x86_stack::restoresSpFromFp(*x86Tables().MII, *x86Tables().MRI, I);
  }
  std::optional<int64_t> movEaxImm(const Instr &I) const override {
    return x86_stack::movEaxImm(*x86Tables().MII, *x86Tables().MRI, I);
  }
  bool writesSpDynamically(const Instr &I) const override {
    return x86_stack::writesSpDynamically(*x86Tables().MII,
                                          *x86Tables().MRI, I);
  }
  std::optional<JumpTableInfo>
  matchJumpTable(const Image &, llvm::ArrayRef<std::pair<Va, Va>>,
                 llvm::ArrayRef<BasicBlock>, llvm::ArrayRef<Instr>, size_t,
                 const std::vector<Va> &, const std::vector<Va> &,
                 const std::map<Va, uint64_t> &) const override {
    return std::nullopt;
  }
};

const TestHooks Hooks;

struct Env {
  std::shared_ptr<Image> Img;
  std::optional<Disasm> Dis;
  Va Base{0x140001000};

  Env(std::vector<uint8_t> Text) {
    Section S;
    S.Name = ".text";
    S.SecVa = Base;
    S.Vsize = Text.size();
    S.Data = std::move(Text);
    S.Flags = SectionFlags::rx();
    Img = std::make_shared<Image>();
    Img->Base = Va(0x140000000);
    Img->Sections.push_back(std::move(S));
    llvm::Expected<Disasm> D = Disasm::create(*Img);
    EXPECT_TRUE(!!D);
    if (D)
      Dis = std::move(*D);
    else
      llvm::consumeError(D.takeError());
  }

  std::vector<Instr> decode() {
    return Dis->decodeRange(Base, Base.wrappingAdd(Img->Sections[0].Vsize));
  }
};

Function makeFunction(Va Start, std::vector<Instr> Instrs,
                      std::vector<BasicBlock> Blocks) {
  Function F;
  F.Start = Start;
  F.End = Blocks.empty() ? Start : Blocks.back().End;
  F.Name = "f";
  F.Instrs = std::move(Instrs);
  F.Blocks = std::move(Blocks);
  F.Chunks.emplace_back(Start, F.End);
  return F;
}

BasicBlock makeBlock(Va Start, Va End, size_t Begin, size_t EndIdx,
                     BlockEnd Kind, std::vector<Edge> Edges = {}) {
  BasicBlock B;
  B.Start = Start;
  B.End = End;
  B.InstrsBegin = Begin;
  B.InstrsEnd = EndIdx;
  B.Edges = std::move(Edges);
  B.EndKind = Kind;
  return B;
}

Program makeProgram(std::shared_ptr<Image> Img) {
  Program P;
  P.Img = std::move(Img);
  P.Externs = std::make_shared<std::map<Va, Extern>>();
  P.Names = std::make_shared<std::map<Va, std::string>>();
  return P;
}

Edge edgeTo(Va To, EdgeKind Kind = EdgeKind::Fallthrough) {
  Edge E;
  E.To = To;
  E.Kind = Kind;
  return E;
}

// Port of lift.rs `stack_delta_unit`:
//   push rbp; sub rsp,0x30; lea rcx,[rsp+0x20]; add rsp,0x30; pop rbp; ret
TEST(StackPtrTest, PushSubLeaAddPop) {
  Env E({0x55, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0x48,
         0x83, 0xC4, 0x30, 0x5D, 0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 6u);
  Va End = E.Base.wrappingAdd(16);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 6, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  // At the lea: push (+8) + sub 0x30 = 0x38.
  EXPECT_EQ(Info->deltaAt(Instrs[2].Addr), 0x38);
  // Deepest touched offset: 0x20 - 0x38 = -0x18 -> frame_size 24.
  EXPECT_EQ(Info->FrameSize, 0x18);
  EXPECT_EQ(Info->rspOff(Instrs[2].Addr, 0x20), -0x18);
  // Balanced: back to 0 at ret.
  EXPECT_EQ(Info->deltaAt(Instrs[5].Addr), 0);
  EXPECT_FALSE(Info->DynVa.has_value());
}

// pushfq/popfq move the delta by 8 like push/pop (Rust models Pushfq with
// the Push arm).
TEST(StackPtrTest, PushfPopfDelta) {
  Env E({0x9C, 0x9D, 0xC3}); // pushfq; popfq; ret
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 3u);
  Va End = E.Base.wrappingAdd(3);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 3, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  EXPECT_EQ(Info->deltaAt(Instrs[1].Addr), 8);
  EXPECT_EQ(Info->deltaAt(Instrs[2].Addr), 0);
}

// mov rbp,rsp latches rbp_delta; leave restores delta to it; rbp slots
// widen the frame through rbpOff.
TEST(StackPtrTest, FramePointerAndLeave) {
  Env E({0x55,                   // push rbp
         0x48, 0x89, 0xE5,       // mov rbp,rsp
         0x48, 0x83, 0xEC, 0x20, // sub rsp,0x20
         0x48, 0x8B, 0x45, 0xF8, // mov rax,[rbp-8]
         0xC9,                   // leave
         0xC3});                 // ret
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 6u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 6, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  ASSERT_TRUE(Info->RbpDelta.has_value());
  EXPECT_EQ(*Info->RbpDelta, 8);
  // mov at delta 8+0x20=0x28; [rbp-8] -> -8-8 = -16 frame depth.
  EXPECT_EQ(Info->deltaAt(Instrs[3].Addr), 0x28);
  EXPECT_EQ(Info->rbpOff(-8), -16);
  EXPECT_GE(Info->FrameSize, 16);
  // leave restores rsp to the fp delta; ret runs at 8 (pop rbp unseen:
  // leave's pop is part of the leave semantics, reference keeps delta at
  // rbp_delta past it).
  EXPECT_EQ(Info->deltaAt(Instrs[5].Addr), 8);
}

// lea rbp,[rsp+disp] latches rbp_delta = delta - disp.
TEST(StackPtrTest, LeaFramePointer) {
  Env E({0x48, 0x83, 0xEC, 0x20,       // sub rsp,0x20
         0x48, 0x8D, 0x6C, 0x24, 0x10, // lea rbp,[rsp+0x10]
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 3u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 3, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  ASSERT_TRUE(Info->RbpDelta.has_value());
  EXPECT_EQ(*Info->RbpDelta, 0x20 - 0x10);
}

// call __chkstk with a preceding mov eax,imm adds the probe size.
TEST(StackPtrTest, ChkstkConstSize) {
  Va Chkstk = Va(0x140002000);
  Env E({0xB8, 0x00, 0x10, 0x00, 0x00, // mov eax,0x1000
         0xE8, 0x00, 0x00, 0x00, 0x00, // call $+5 (target patched below)
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 3u);
  // Point the call at the fake chkstk import.
  Instrs[1].HasTarget = true;
  Instrs[1].Target = Chkstk;
  Instrs[1].Flow = FlowKind::Call;
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 3, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  (*P.Externs)[Chkstk] = Extern{Chkstk, "ntdll!__chkstk", false};
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  EXPECT_EQ(Info->deltaAt(Instrs[2].Addr), 0x1000);
  EXPECT_FALSE(Info->DynVa.has_value());
}

// call __chkstk with no constant size marks DynVa and keeps going.
TEST(StackPtrTest, ChkstkDynamicSize) {
  Va Chkstk = Va(0x140002000);
  Env E({0xE8, 0x00, 0x00, 0x00, 0x00, // call $+5 (target patched below)
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 2u);
  Instrs[0].HasTarget = true;
  Instrs[0].Target = Chkstk;
  Instrs[0].Flow = FlowKind::Call;
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 2, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  (*P.Names)[Chkstk] = "__chkstk";
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  ASSERT_TRUE(Info->DynVa.has_value());
  EXPECT_EQ(*Info->DynVa, Instrs[0].Addr);
  EXPECT_TRUE(Info->isDynamicAt(Instrs[1].Addr));
  // DynVa marks the chkstk call itself: the call runs with a dynamic rsp
  // (reference `is_dynamic_at`: va >= dyn_va).
  EXPECT_TRUE(Info->isDynamicAt(Instrs[0].Addr));
}

// sub rsp,rax is a dynamic write: DynVa set, walk continues at the
// pre-dynamic delta.
TEST(StackPtrTest, DynamicSubReg) {
  Env E({0x48, 0x29, 0xC4, // sub rsp,rax
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 2u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 2, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  ASSERT_TRUE(Info->DynVa.has_value());
  EXPECT_EQ(*Info->DynVa, Instrs[0].Addr);
  EXPECT_EQ(Info->deltaAt(Instrs[1].Addr), 0);
}

// and rsp,-N likewise marks dynamic (Rust: any other rsp write).
TEST(StackPtrTest, DynamicAndRsp) {
  Env E({0x48, 0x83, 0xE4, 0xF0, // and rsp,-16
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 2u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 2, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  ASSERT_TRUE(Info->DynVa.has_value());
}

// lea rsp,[rax] (non-rsp base) is dynamic, not a delta.
TEST(StackPtrTest, DynamicLeaRspAbsolute) {
  Env E({0x48, 0x8D, 0x20, // lea rsp,[rax]
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 2u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 2, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  ASSERT_TRUE(Info->DynVa.has_value());
}

// lea rsp,[rsp+0x10] is alloca-style growth: delta decreases by disp.
// (Rust: delta - disp; positive disp shrinks the frame.)
TEST(StackPtrTest, LeaRspAlloca) {
  Env E({0x48, 0x8D, 0x64, 0x24, 0x10, // lea rsp,[rsp+0x10]
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 2u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 2, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  EXPECT_EQ(Info->deltaAt(Instrs[1].Addr), -0x10);
  EXPECT_FALSE(Info->DynVa.has_value());
}

// Joined blocks must agree: push on one path, nothing on the other fails.
TEST(StackPtrTest, InconsistentJoinFails) {
  // Layout: A = [push rbp; jmp Join] (delta +8 into Join),
  //         B = [nop; jmp Join]    (delta 0 into Join),
  //         Join = [ret]. The walk starts at F.Start = A; B is seeded by
  //         pushing it explicitly is impossible through the public API, so
  //         instead A itself contains the disagreement: block A is listed
  //         twice is not possible either. Model the join directly: two
  //         predecessor blocks A1 (push) and A2 (nop) both jumping to Join,
  //         with F.Start at a branch header that falls through to A1 and
  //         jumps to A2... simplest faithful shape: header block H with two
  //         successors A1/A2 is overkill; the reference fails the moment a
  //         block is entered with a second, differing delta, so build:
  //         H = [nop] with Fallthrough->A1, Jump->A2; A1 = [push rbp,
  //         jmp Join]; A2 = [nop, jmp Join]; Join = [ret].
  Env E({0x90,             // 0: H: nop
         0x55,             // 1: A1: push rbp
         0xEB, 0x01,       // 2: A1: jmp +1 -> 5
         0x90,             // 4: A2: nop
         0xEB, 0x00,       // 5: A2: jmp +0 -> 7 (Join)
         0x90,             // 6: (A1 jmp target filler, part of A2 block)
         0xC3});           // 7: Join: ret
  auto Instrs = E.decode();
  ASSERT_GE(Instrs.size(), 5u);
  Va A1 = Instrs[1].Addr;
  Va A2 = Instrs[3].Addr;
  Va Join = Instrs.back().Addr;
  Function F = makeFunction(
      E.Base, Instrs,
      {makeBlock(E.Base, A1, 0, 1, BlockEnd::Branch,
                 {edgeTo(A1, EdgeKind::Fallthrough),
                  edgeTo(A2, EdgeKind::Jump)}),
       makeBlock(A1, A2, 1, 3, BlockEnd::Branch,
                 {edgeTo(Join, EdgeKind::Jump)}),
       makeBlock(A2, Join, 3, 5, BlockEnd::Branch,
                 {edgeTo(Join, EdgeKind::Jump)}),
       makeBlock(Join, Join.wrappingAdd(1), 5, 6, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  EXPECT_TRUE(!Info);
  if (!Info)
    llvm::consumeError(Info.takeError());
}

// Incoming stack args: [rsp+0x30] at delta 0 widens ArgsSize.
TEST(StackPtrTest, ArgsSizeFromIncomingSlot) {
  Env E({0x48, 0x8B, 0x44, 0x24, 0x30, // mov rax,[rsp+0x30]
         0xC3});
  auto Instrs = E.decode();
  ASSERT_EQ(Instrs.size(), 2u);
  Va End = E.Base.wrappingAdd(E.Img->Sections[0].Vsize);
  Function F = makeFunction(E.Base, Instrs,
                            {makeBlock(E.Base, End, 0, 2, BlockEnd::Return)});
  Program P = makeProgram(E.Img);
  auto Info = analyzeStack(P, F, Hooks);
  ASSERT_TRUE(!!Info);
  EXPECT_EQ(Info->ArgsSize, 0x30);
  EXPECT_EQ(Info->FrameSize, 0);
}

// Unknown VAs read back delta 0 (Rust `delta_at` default).
TEST(StackPtrTest, UnknownVaDefaultsZero) {
  StackInfo S;
  EXPECT_EQ(S.deltaAt(Va(0x140001000)), 0);
  EXPECT_EQ(S.rspOff(Va(0x140001000), 8), 8);
  EXPECT_FALSE(S.rbpOff(8).has_value());
  EXPECT_FALSE(S.isDynamicAt(Va(0x140001000)));
}

} // end anonymous namespace
