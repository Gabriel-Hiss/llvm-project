//===- unittests/Analysis/NoreturnTest.cpp - noreturn checks ------------===//
//
// Port of the unit tests in crates/analysis/src/noreturn.rs: a trap exit
// proves only its own path, the fixpoint promotes tail-call chains, stale
// inferred flags clear on re-proof, and unprovable Noreturn blocks stay
// Unknown.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Noreturn.h"
#include "revenant/Target/TargetHooks.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

using namespace revenant;

namespace {

/// Test hooks add no target-specific traps; these tests exercise the
/// generic MC Trap flow kind.
class TestHooks final : public TargetHooks {
public:
  llvm::StringRef mnemonic(const Instr &) const override { return ""; }
  bool isFiller(const Instr &) const override { return false; }
  TrapKind trapKind(const Instr &) const override { return TrapKind::NotTrap; }
  bool isNopOrInt3(const Instr &) const override { return false; }
  bool isOpaqueTailJmp(const Image &, const Instr &) const override {
    return false;
  }
  std::optional<int64_t> stackDelta(const Instr &) const override {
    return std::nullopt;
  }
  StackSlots stackSlots(const Instr &) const override { return {}; }
  FpEstablish fpEstablish(const Instr &) const override { return {}; }
  bool restoresSpFromFp(const Instr &) const override { return false; }
  std::optional<int64_t> movEaxImm(const Instr &) const override {
    return std::nullopt;
  }
  bool writesSpDynamically(const Instr &) const override { return false; }
  std::optional<JumpTableInfo>
  matchJumpTable(const Image &, llvm::ArrayRef<std::pair<Va, Va>>,
                 llvm::ArrayRef<BasicBlock>, llvm::ArrayRef<Instr>, size_t,
                 const std::vector<Va> &, const std::vector<Va> &,
                 const std::map<Va, uint64_t> &) const override {
    return std::nullopt;
  }
};

const TestHooks Hooks;

Instr makeInstr(Va Addr, uint32_t Size, FlowKind Flow,
                std::optional<Va> Target = std::nullopt,
                std::optional<Va> MemTarget = std::nullopt) {
  Instr I;
  I.Addr = Addr;
  I.Size = Size;
  I.Flow = Flow;
  I.HasTarget = Target.has_value();
  I.Target = Target.value_or(Va());
  I.HasMemTarget = MemTarget.has_value();
  I.MemTarget = MemTarget.value_or(Va());
  return I;
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

Edge tailTo(Va Target) {
  Edge E;
  E.To = Target;
  E.Kind = EdgeKind::TailCall;
  return E;
}

Function makeFunc(Va Start, std::vector<Instr> Instrs,
                  std::vector<BasicBlock> Blocks) {
  Function F;
  F.Start = Start;
  Va End = Start;
  for (const BasicBlock &B : Blocks)
    if (B.End > End)
      End = B.End;
  F.End = End;
  F.Instrs = std::move(Instrs);
  F.Blocks = std::move(Blocks);
  return F;
}

std::map<Va, Extern> noExterns() { return {}; }
std::map<Va, std::string> noNames() { return {}; }

// Error trap plus a normal tail exit to an ordinary callee: the trap
// proves only its own exit, the live tailcall keeps the function
// returning. A trap-only function is noreturn, and a function tail-calling
// it is promoted by the fixpoint.
TEST(NoreturnTest, ErrorTrapAndTailcallStaysReturning) {
  std::vector<Function> Funcs;
  // F at 0x1000: branch block, tail exit to G at 0x2000, trap block.
  Funcs.push_back(makeFunc(
      Va(0x1000),
      {makeInstr(Va(0x1000), 2, FlowKind::CondBranch, Va(0x1007)),
       makeInstr(Va(0x1002), 5, FlowKind::Branch, Va(0x2000)),
       makeInstr(Va(0x1007), 2, FlowKind::Trap)},
      {makeBlock(Va(0x1000), Va(0x1002), 0, 1, BlockEnd::Branch),
       makeBlock(Va(0x1002), Va(0x1007), 1, 2, BlockEnd::TailCall,
                 {tailTo(Va(0x2000))}),
       makeBlock(Va(0x1007), Va(0x1009), 2, 3, BlockEnd::Noreturn)}));
  // G: ordinary returner.
  Funcs.push_back(makeFunc(
      Va(0x2000), {makeInstr(Va(0x2000), 1, FlowKind::Return)},
      {makeBlock(Va(0x2000), Va(0x2001), 0, 1, BlockEnd::Return)}));
  // Trap-only function: every exit proven dead, so noreturn.
  Funcs.push_back(makeFunc(
      Va(0x3000), {makeInstr(Va(0x3000), 2, FlowKind::Trap)},
      {makeBlock(Va(0x3000), Va(0x3002), 0, 1, BlockEnd::Noreturn)}));
  // Tail-calls the trap-only function: the fixpoint promotes it.
  Funcs.push_back(makeFunc(
      Va(0x4000),
      {makeInstr(Va(0x4000), 5, FlowKind::Branch, Va(0x3000))},
      {makeBlock(Va(0x4000), Va(0x4005), 0, 1, BlockEnd::TailCall,
                 {tailTo(Va(0x3000))})}));
  // Caller C: call F, then ret. Calls that continue are not exits.
  Funcs.push_back(makeFunc(
      Va(0x5000),
      {makeInstr(Va(0x5000), 5, FlowKind::Call, Va(0x1000)),
       makeInstr(Va(0x5005), 1, FlowKind::Return)},
      {makeBlock(Va(0x5000), Va(0x5005), 0, 1, BlockEnd::Fallthrough),
       makeBlock(Va(0x5005), Va(0x5006), 1, 2, BlockEnd::Return)}));

  std::set<Va> Set = propagateNoreturn(Funcs, noExterns(), noNames(), Hooks);
  EXPECT_TRUE(Set.find(Va(0x1000)) == Set.end());
  EXPECT_FALSE(Funcs[0].IsNoreturn);
  EXPECT_FALSE(Funcs[1].IsNoreturn);
  EXPECT_TRUE(Funcs[2].IsNoreturn);
  EXPECT_TRUE(Funcs[3].IsNoreturn);
  EXPECT_FALSE(Funcs[4].IsNoreturn);
  // The caller's call-site block still ends at the `call`; flow past it
  // (the `ret` block) survived.
  EXPECT_EQ(Funcs[4].Blocks[0].EndKind, BlockEnd::Fallthrough);
  EXPECT_EQ(Funcs[4].Blocks[1].EndKind, BlockEnd::Return);
}

// A second propagate with weakened evidence must clear a stale inferred
// flag, not keep it.
TEST(NoreturnTest, SecondPropagateClearsInferredFlag) {
  std::vector<Function> Funcs;
  Funcs.push_back(makeFunc(
      Va(0x1000), {makeInstr(Va(0x1000), 2, FlowKind::Trap)},
      {makeBlock(Va(0x1000), Va(0x1002), 0, 1, BlockEnd::Noreturn)}));
  std::set<Va> Set = propagateNoreturn(Funcs, noExterns(), noNames(), Hooks);
  EXPECT_NE(Set.find(Va(0x1000)), Set.end());
  EXPECT_TRUE(Funcs[0].IsNoreturn);
  // The exit turned ordinary (a live tailcall to a returning callee).
  // Mutate in place so the inferred flag survives to the second pass:
  // it must be propagate that clears it.
  Funcs[0].Instrs = {makeInstr(Va(0x1000), 5, FlowKind::Branch, Va(0x2000))};
  Funcs[0].Blocks = {makeBlock(Va(0x1000), Va(0x1005), 0, 1,
                               BlockEnd::TailCall, {tailTo(Va(0x2000))})};
  ASSERT_TRUE(Funcs[0].IsNoreturn);
  Set = propagateNoreturn(Funcs, noExterns(), noNames(), Hooks);
  EXPECT_TRUE(Set.find(Va(0x1000)) == Set.end());
  EXPECT_FALSE(Funcs[0].IsNoreturn);
}

// A `Noreturn` block whose terminal instruction is a plain `call`
// (marked when the callee was known noreturn at decode time) re-proves
// that callee on the next pass. If it turned out to return, the block
// contributes an ordinary Call exit, so the function stays returning.
TEST(NoreturnTest, NoreturnBlockEndingInCallReprovesCallee) {
  std::vector<Function> Funcs;
  Funcs.push_back(makeFunc(
      Va(0x1000),
      {makeInstr(Va(0x1000), 5, FlowKind::Call, Va(0x2000))},
      {makeBlock(Va(0x1000), Va(0x1005), 0, 1, BlockEnd::Noreturn)}));
  Funcs.push_back(makeFunc(
      Va(0x2000), {makeInstr(Va(0x2000), 1, FlowKind::Return)},
      {makeBlock(Va(0x2000), Va(0x2001), 0, 1, BlockEnd::Return)}));
  std::set<Va> Set = propagateNoreturn(Funcs, noExterns(), noNames(), Hooks);
  EXPECT_TRUE(Set.find(Va(0x1000)) == Set.end());
  EXPECT_FALSE(Funcs[0].IsNoreturn);
}

// A `Noreturn` block with no terminal instruction is not evidence:
// nothing provable means Unknown, so the function stays returning.
TEST(NoreturnTest, EmptyNoreturnBlockIsUnknownExit) {
  std::vector<Function> Funcs;
  Function F = makeFunc(
      Va(0x1000), {makeInstr(Va(0x1000), 2, FlowKind::Trap)},
      {makeBlock(Va(0x1000), Va(0x1002), 0, 0, BlockEnd::Noreturn)});
  F.Instrs.clear();
  Funcs.push_back(std::move(F));
  std::set<Va> Set = propagateNoreturn(Funcs, noExterns(), noNames(), Hooks);
  EXPECT_TRUE(Set.empty());
  EXPECT_FALSE(Funcs[0].IsNoreturn);
}

// A `Noreturn` block ending in an ordinary non-call non-trap instruction
// is Unknown: `Noreturn` was inferred, not certain.
TEST(NoreturnTest, NoreturnBlockEndingInNonTrapIsUnknownExit) {
  std::vector<Function> Funcs;
  Funcs.push_back(makeFunc(
      Va(0x1000), {makeInstr(Va(0x1000), 3, FlowKind::None)},
      {makeBlock(Va(0x1000), Va(0x1003), 0, 1, BlockEnd::Noreturn)}));
  std::set<Va> Set = propagateNoreturn(Funcs, noExterns(), noNames(), Hooks);
  EXPECT_TRUE(Set.empty());
  EXPECT_FALSE(Funcs[0].IsNoreturn);
}

// Known import names mark noreturn: exact, decorated and qualified forms.
TEST(NoreturnTest, KnownImportNames) {
  EXPECT_TRUE(isNoreturnImport("kernel32!ExitProcess"));
  EXPECT_TRUE(isNoreturnImport("ucrt!abort"));
  EXPECT_TRUE(isNoreturnImport("ucrt!abort@0"));
  EXPECT_TRUE(isNoreturnImport("fn::ExitProcess"));
  EXPECT_FALSE(isNoreturnImport("kernel32!CreateFileW"));
  EXPECT_FALSE(isNoreturnImport("kernel32!ExitProcessEx"));
  EXPECT_TRUE(isNoreturnImport("core::panicking::panic"));
  EXPECT_TRUE(nameIsNoreturn("alloc::rust_begin_unwind"));
  EXPECT_FALSE(nameIsNoreturn("sub_140001000"));
}

// Extern-provided noreturn and name-bit names feed the fixpoint: a
// function tail-calling a noreturn import promotes.
TEST(NoreturnTest, NoreturnExternPromotesTailCaller) {
  Extern E;
  E.Slot = Va(0x9000);
  E.Name = "kernel32!ExitProcess";
  E.Noreturn = true;
  std::map<Va, Extern> Externs{{Va(0x9000), E}};
  std::map<Va, std::string> Names{{Va(0x9000), E.Name}};

  std::vector<Function> Funcs;
  Funcs.push_back(makeFunc(
      Va(0x1000),
      {makeInstr(Va(0x1000), 5, FlowKind::Branch, Va(0x9000))},
      {makeBlock(Va(0x1000), Va(0x1005), 0, 1, BlockEnd::TailCall,
                 {tailTo(Va(0x9000))})}));
  std::set<Va> Set = propagateNoreturn(Funcs, Externs, Names, Hooks);
  EXPECT_NE(Set.find(Va(0x1000)), Set.end());
  EXPECT_TRUE(Funcs[0].IsNoreturn);
}

} // end anonymous namespace
