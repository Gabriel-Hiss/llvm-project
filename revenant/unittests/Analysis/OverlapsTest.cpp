//===- unittests/Analysis/OverlapsTest.cpp - overlap checks ------------===//
//
// Port of the `overlap_loser_drops_alone` unit test in
// crates/analysis/src/lib.rs plus coverage for the `finish()` tail this
// slice owns: a bogus block bridging two disjoint real blocks loses alone,
// data-covered instructions prune to a block prefix, and chunks clip to
// the function's own `.pdata` ranges.
//
//===----------------------------------------------------------------------===//

#include "../lib/Analysis/AnalysisImpl.h"

#include "revenant/Core/Image.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Target/TargetHooks.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "llvm/Support/Error.h"

using namespace revenant;
using namespace revenant::analysis_detail;

namespace {

class TestHooks final : public TargetHooks {
public:
  llvm::StringRef mnemonic(const Instr &) const override { return ""; }
  bool isFiller(const Instr &) const override { return false; }
  TrapKind trapKind(const Instr &) const override {
    return TrapKind::NotTrap;
  }
  bool isNopOrInt3(const Instr &) const override { return false; }
  bool isOpaqueTailJmp(const Image &, const Instr &) const override {
    return false;
  }
  std::optional<int64_t> stackDelta(const Instr &) const override {
    return std::nullopt;
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
  Va Base{0x1000};

  Env(std::vector<uint8_t> Text) {
    Section S;
    S.Name = ".text";
    S.SecVa = Base;
    S.Vsize = Text.size();
    S.Data = std::move(Text);
    S.Flags = SectionFlags::rx();
    Img = std::make_shared<Image>();
    Img->Base = Base;
    Img->Sections.push_back(std::move(S));
    llvm::Expected<Disasm> D = Disasm::create(*Img);
    EXPECT_TRUE(!!D);
    if (D)
      Dis = std::move(*D);
    else
      llvm::consumeError(D.takeError());
  }

  std::vector<Instr> decode(uint64_t From, uint64_t To) {
    std::vector<Instr> Out;
    Va Ip(From);
    while (Ip < Va(To)) {
      std::optional<Instr> I = Dis->decodeOne(Ip);
      EXPECT_TRUE(I.has_value());
      if (!I)
        break;
      Ip = I->nextVa();
      Out.push_back(*I);
    }
    EXPECT_EQ(Ip, Va(To));
    return Out;
  }
};

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

Edge flowTo(Va To, EdgeKind Kind) {
  Edge E;
  E.To = To;
  E.Kind = Kind;
  return E;
}

// The reference case: A (0x1000..0x1005, fallthrough to C) and C
// (0x1005..0x1015, unresolved dispatch) are disjoint; bogus B
// (0x1004..0x1015, starting on A's nop) bridges them. Pairwise
// arbitration drops B alone; transitive grouping would have kept only A.
TEST(OverlapsTest, LoserDropsAlone) {
  std::vector<uint8_t> Bytes(0x40, 0xcc);
  Bytes[0x00] = 0x55; // push rbp
  Bytes[0x01] = 0x48;
  Bytes[0x02] = 0x89;
  Bytes[0x03] = 0xe5; // mov rbp, rsp
  Bytes[0x04] = 0x90; // nop, B decodes from here
  Bytes[0x05] = 0x48;
  Bytes[0x06] = 0x8d;
  Bytes[0x07] = 0x0d;
  Bytes[0x08] = 0x00;
  Bytes[0x09] = 0x00;
  Bytes[0x0a] = 0x00;
  Bytes[0x0b] = 0x00; // lea rcx, [rip]
  Bytes[0x0c] = 0x48;
  Bytes[0x0d] = 0x63;
  Bytes[0x0e] = 0x04;
  Bytes[0x0f] = 0x81; // movsxd rax, [rcx+rax*4]
  Bytes[0x10] = 0x48;
  Bytes[0x11] = 0x01;
  Bytes[0x12] = 0xc8; // add rax, rcx
  Bytes[0x13] = 0xff;
  Bytes[0x14] = 0xe0; // jmp rax
  Env E(std::move(Bytes));

  std::vector<Instr> Instrs = E.decode(0x1000, 0x1005);
  std::vector<Instr> B = E.decode(0x1004, 0x1015);
  std::vector<Instr> C = E.decode(0x1005, 0x1015);
  Instrs.insert(Instrs.end(), B.begin(), B.end());
  Instrs.insert(Instrs.end(), C.begin(), C.end());

  Function F;
  F.Start = Va(0x1000);
  F.End = Va(0x1015);
  F.Instrs = std::move(Instrs);
  F.Blocks = {
      makeBlock(Va(0x1000), Va(0x1005), 0, 3, BlockEnd::Fallthrough,
                {flowTo(Va(0x1005), EdgeKind::Fallthrough)}),
      makeBlock(Va(0x1004), Va(0x1015), 3, 8, BlockEnd::UnresolvedIndirect),
      makeBlock(Va(0x1005), Va(0x1015), 8, 12, BlockEnd::UnresolvedIndirect),
  };

  std::set<Va> Dead;
  OverlapEnv OEnv;
  OEnv.DeadStarts = &Dead;
  resolveOverlaps(F, *E.Img, *E.Dis, Hooks, OEnv);

  ASSERT_EQ(F.Blocks.size(), 2u);
  EXPECT_EQ(F.Blocks[0].Start, Va(0x1000));
  EXPECT_EQ(F.Blocks[1].Start, Va(0x1005));
  EXPECT_NE(Dead.find(Va(0x1004)), Dead.end());
  EXPECT_EQ(F.Instrs.size(), 7u);
}

// pruneDataInstrs keeps only the prefix before the first data-covered
// instruction and marks the block end dead; a fully covered block dies.
TEST(OverlapsTest, PruneKeepsPrefixBeforeData) {
  Env E(std::vector<uint8_t>(0x20, 0x90));
  std::vector<Instr> Instrs = E.decode(0x1000, 0x1004);

  Function F;
  F.Start = Va(0x1000);
  F.End = Va(0x1004);
  F.Instrs = Instrs;
  F.Blocks = {
      makeBlock(Va(0x1000), Va(0x1002), 0, 2, BlockEnd::Fallthrough,
                {flowTo(Va(0x1002), EdgeKind::Fallthrough)}),
      makeBlock(Va(0x1002), Va(0x1004), 2, 4, BlockEnd::Return),
  };
  F.DataRanges.emplace_back(Va(0x1001), Va(0x1003));

  pruneDataInstrs(F, *E.Img);

  ASSERT_EQ(F.Blocks.size(), 1u);
  EXPECT_EQ(F.Blocks[0].Start, Va(0x1000));
  EXPECT_EQ(F.Blocks[0].End, Va(0x1001));
  EXPECT_EQ(F.Blocks[0].EndKind, BlockEnd::EndOfFunction);
  EXPECT_TRUE(F.Blocks[0].Edges.empty());
  EXPECT_EQ(F.Instrs.size(), 1u);
}

// buildChunks clips blocks to the own `.pdata` region and merges the
// clipped pieces; bytes past the region never become chunks.
TEST(OverlapsTest, ChunksClipToOwnRegion) {
  Env E(std::vector<uint8_t>(0x20, 0x90));
  std::vector<Instr> Instrs = E.decode(0x1000, 0x1006);

  Function F;
  F.Start = Va(0x1000);
  F.End = Va(0x1006);
  F.Instrs = std::move(Instrs);
  F.Blocks = {
      makeBlock(Va(0x1000), Va(0x1003), 0, 3, BlockEnd::Fallthrough,
                {flowTo(Va(0x1003), EdgeKind::Fallthrough)}),
      makeBlock(Va(0x1003), Va(0x1006), 3, 6, BlockEnd::Return),
  };

  buildChunks(F, {{Va(0x1000), Va(0x1004)}});
  ASSERT_EQ(F.Chunks.size(), 1u);
  EXPECT_EQ(F.Chunks[0].first, Va(0x1000));
  EXPECT_EQ(F.Chunks[0].second, Va(0x1004));
}

// A TailCall edge into the middle of a surviving instruction is stale:
// buildChunks drops it, and an edge-less TailCall block is out of bytes.
TEST(OverlapsTest, MidInstructionTailCallIsDropped) {
  // 0x1000..0x1002 are nops; 0x1002 is a 3-byte add, so an edge to 0x1003
  // lands mid-instruction.
  std::vector<uint8_t> Bytes(0x20, 0x90);
  Bytes[0x02] = 0x48;
  Bytes[0x03] = 0x01;
  Bytes[0x04] = 0xc8; // add rax, rcx at 0x1002..0x1005
  Env E(std::move(Bytes));
  std::vector<Instr> Instrs = E.decode(0x1000, 0x1002);
  std::vector<Instr> More = E.decode(0x1002, 0x1005);
  Instrs.insert(Instrs.end(), More.begin(), More.end());

  Function F;
  F.Start = Va(0x1000);
  F.End = Va(0x1005);
  F.Instrs = std::move(Instrs);
  F.Blocks = {
      makeBlock(Va(0x1000), Va(0x1002), 0, 2, BlockEnd::TailCall,
                {flowTo(Va(0x1003), EdgeKind::TailCall)}),
      makeBlock(Va(0x1002), Va(0x1005), 2, 3, BlockEnd::Return),
  };

  buildChunks(F, {});
  ASSERT_EQ(F.Blocks.size(), 2u);
  EXPECT_TRUE(F.Blocks[0].Edges.empty());
  EXPECT_EQ(F.Blocks[0].EndKind, BlockEnd::EndOfFunction);
}

} // end anonymous namespace
