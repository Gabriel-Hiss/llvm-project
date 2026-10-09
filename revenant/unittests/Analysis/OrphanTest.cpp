//===- unittests/Analysis/OrphanTest.cpp - orphan-sweep checks --------===//
//
// Port intent of the orphan rules in crates/analysis/src/lib.rs and its
// integration tests in crates/analysis/tests/analysis.rs: probe ranges
// carve executable gaps minus pdata/chunk/data/inline-data claims with a
// Go-gated blob barrier, and `orphanFunctionOk` applies the bound-edge
// acceptance rule (EndOfFunction only at the bound, indirect tails only
// in the opaque shapes, every other edge on a block start).
//
// The range tests build synthetic PEs; the two corpus tests run on
// C:\Users\User\RustroverProjects\Revenant\corpus\bin binaries:
// `orphan_channel_read` in corpus_orphan.exe must sit outside every
// .pdata range (reachable only through the sweep), and orphans in the
// corpus_c set must never claim bytes inside a .pdata range or another
// function's chunks.
//
//===----------------------------------------------------------------------===//

#include "../lib/Analysis/Orphan.h"

#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace revenant;
using namespace revenant::analysis_detail;

namespace {

struct Env {
  std::shared_ptr<Image> Img;
  std::optional<Disasm> Dis;
  Va Base{0x140000000};

  Env() {
    Img = std::make_shared<Image>();
    Img->Base = Base;
  }

  void addText(Va Start, std::vector<uint8_t> Bytes, uint64_t Vsize = 0) {
    Section S;
    S.Name = ".text";
    S.SecVa = Start;
    S.Vsize = Vsize ? Vsize : Bytes.size();
    S.Data = std::move(Bytes);
    S.Flags = SectionFlags::rx();
    Img->Sections.push_back(std::move(S));
  }

  void addData(Va Start, size_t Size) {
    Section S;
    S.Name = ".rdata";
    S.SecVa = Start;
    S.Vsize = Size;
    S.Data.assign(Size, 0);
    S.Flags = SectionFlags::r();
    Img->Sections.push_back(std::move(S));
  }

  void decode() {
    llvm::Expected<Disasm> D = Disasm::create(*Img);
    EXPECT_TRUE(!!D);
    if (D)
      Dis = std::move(*D);
    else
      llvm::consumeError(D.takeError());
  }
};

PeFile makePe(const std::shared_ptr<Image> &Img, Va Entry) {
  PeFile Pe;
  Pe.Img = Img;
  Pe.EntryPoint = Entry;
  return Pe;
}

Function funcWithChunks(Va Start, std::vector<std::pair<Va, Va>> Chunks,
                        std::vector<std::pair<Va, Va>> Data = {}) {
  Function F;
  F.Start = Start;
  F.End = Start;
  F.Chunks = std::move(Chunks);
  F.DataRanges = std::move(Data);
  for (auto [A, B] : F.Chunks)
    F.End = std::max(F.End, B);
  return F;
}

Instr testInstr(Va Addr, uint32_t Size, FlowKind Flow,
                std::optional<Va> Target = std::nullopt) {
  Instr I;
  I.Addr = Addr;
  I.Size = Size;
  I.Flow = Flow;
  // `jmp reg` shape for tail tests: single register operand.
  if (Flow == FlowKind::IndirectBranch)
    I.Inst.addOperand(llvm::MCOperand::createReg(1));
  I.HasTarget = Target.has_value();
  I.Target = Target.value_or(Va());
  return I;
}

BasicBlock testBlock(Va Start, Va End, BlockEnd Kind,
                     std::vector<Edge> Edges = {}) {
  BasicBlock B;
  B.Start = Start;
  B.End = End;
  B.Edges = std::move(Edges);
  B.EndKind = Kind;
  return B;
}

Edge edgeTo(Va To, EdgeKind Kind = EdgeKind::Jump) {
  Edge E;
  E.To = To;
  E.Kind = Kind;
  return E;
}

// A pdata range splits the section: probes cover the gap around it but
// never the range itself.
TEST(OrphanTest, ProbeRangesSkipPdata) {
  Env E;
  E.addText(Va(0x140001000), std::vector<uint8_t>(0x400, 0x90));
  E.decode();
  PeFile Pe = makePe(E.Img, Va(0x140001000));
  RuntimeFunction R;
  R.Begin = Va(0x140001100);
  R.End = Va(0x140001200);
  R.Unwind = Va(0x140001100);
  Pe.Pdata.push_back(R);
  auto Ranges = planOrphanSweep(Pe, {}).Gaps;
  ASSERT_EQ(Ranges.size(), 2u);
  EXPECT_EQ(Ranges[0].first, Va(0x140001000));
  EXPECT_EQ(Ranges[0].second, Va(0x140001100));
  EXPECT_EQ(Ranges[1].first, Va(0x140001200));
  EXPECT_EQ(Ranges[1].second, Va(0x140001400));
  for (auto [A, B] : Ranges)
    EXPECT_FALSE(A < Va(0x140001200) && Va(0x140001100) < B);
}

// Chunks and data ranges both block probes: the data span between two
// functions stays unprobeable even where no chunk covers it.
TEST(OrphanTest, ProbeRangesSkipChunksAndData) {
  Env E;
  E.addText(Va(0x140001000), std::vector<uint8_t>(0x400, 0x90));
  E.decode();
  PeFile Pe = makePe(E.Img, Va(0x140001000));
  std::vector<Function> Funcs;
  Funcs.push_back(funcWithChunks(Va(0x140001000),
                                 {{Va(0x140001000), Va(0x140001080)}},
                                 {{Va(0x140001080), Va(0x1400010C0)}}));
  Funcs.push_back(funcWithChunks(Va(0x140001200),
                                 {{Va(0x140001200), Va(0x140001280)}}));
  auto Ranges = planOrphanSweep(Pe, Funcs).Gaps;
  ASSERT_EQ(Ranges.size(), 2u);
  EXPECT_EQ(Ranges[0].first, Va(0x1400010C0));
  EXPECT_EQ(Ranges[0].second, Va(0x140001200));
  EXPECT_EQ(Ranges[1].first, Va(0x140001280));
  EXPECT_EQ(Ranges[1].second, Va(0x140001400));
}

// Inline-data barriers (reloc slots, the Go build-id blob) stay in the
// gap but join the claimed map, so a probe running into one stops there.
TEST(OrphanTest, ProbeRangesClaimInlineData) {
  Env E;
  E.addText(Va(0x140001000), std::vector<uint8_t>(0x400, 0x90));
  E.decode();
  E.Img->InlineData.push_back({Va(0x140001100), Va(0x140001140)});
  PeFile Pe = makePe(E.Img, Va(0x140001000));
  OrphanSweep Plan = planOrphanSweep(Pe, {});
  ASSERT_EQ(Plan.Gaps.size(), 1u);
  EXPECT_EQ(Plan.Gaps[0].first, Va(0x140001000));
  EXPECT_EQ(Plan.Gaps[0].second, Va(0x140001400));
  ASSERT_EQ(Plan.Claimed.size(), 1u);
  EXPECT_EQ(Plan.Claimed.begin()->first, Va(0x140001100));
  EXPECT_EQ(Plan.Claimed.begin()->second, Va(0x140001140));
}

// Non-executable sections never produce probes.
TEST(OrphanTest, ProbeRangesIgnoreDataSections) {
  Env E;
  E.addText(Va(0x140001000), std::vector<uint8_t>(0x100, 0x90));
  E.addData(Va(0x140002000), 0x100);
  E.decode();
  PeFile Pe = makePe(E.Img, Va(0x140001000));
  auto Ranges = planOrphanSweep(Pe, {}).Gaps;
  ASSERT_EQ(Ranges.size(), 1u);
  EXPECT_EQ(Ranges[0].first, Va(0x140001000));
  EXPECT_EQ(Ranges[0].second, Va(0x140001100));
}

// The Go blob barrier only applies to Go binaries: without
// `Pe.GoBuildId` the jumped-over span stays probeable.
TEST(OrphanTest, BlobBarrierNeedsGoBuildId) {
  Env E;
  // jmp +5 (EB 05) landing on the last instruction (ret at +7): the
  // jumped-over bytes [2, 7) are the blob candidate.
  E.addText(Va(0x140001000), {0xEB, 0x05, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xC3});
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  F.End = Va(0x140001008);
  auto Jmp = E.Dis->decodeOne(Va(0x140001000));
  ASSERT_TRUE(Jmp.has_value());
  F.Instrs.push_back(*Jmp);
  auto Ret = E.Dis->decodeOne(Va(0x140001007));
  ASSERT_TRUE(Ret.has_value());
  F.Instrs.push_back(*Ret);
  std::vector<Function> Funcs{F};

  PeFile Plain = makePe(E.Img, Va(0x140001000));
  OrphanSweep PlainPlan = planOrphanSweep(Plain, Funcs);
  auto &PlainRanges = PlainPlan.Gaps;
  ASSERT_EQ(PlainRanges.size(), 1u);
  EXPECT_EQ(PlainRanges[0].first, Va(0x140001000));
  EXPECT_EQ(PlainRanges[0].second, Va(0x140001008));
  EXPECT_TRUE(PlainPlan.Claimed.empty());

  PeFile Go = makePe(E.Img, Va(0x140001000));
  Go.GoBuildId = VaRange{Va(0x140003000), Va(0x140003020)};
  OrphanSweep GoPlan = planOrphanSweep(Go, Funcs);
  ASSERT_EQ(GoPlan.Gaps.size(), 1u);
  EXPECT_EQ(GoPlan.Gaps[0].first, Va(0x140001000));
  EXPECT_EQ(GoPlan.Gaps[0].second, Va(0x140001008));
  ASSERT_EQ(GoPlan.Claimed.size(), 1u);
  EXPECT_EQ(GoPlan.Claimed.begin()->first, Va(0x140001002));
  EXPECT_EQ(GoPlan.Claimed.begin()->second, Va(0x140001007));
}

// A jump into the middle of the flow is real code: no barrier even for
// Go binaries.
TEST(OrphanTest, MidFunctionJumpIsNotABlob) {
  Env E;
  // jmp +2 (EB 02) landing at +4, before the last instruction (+7).
  E.addText(Va(0x140001000),
            {0xEB, 0x02, 0xAA, 0xAA, 0x90, 0x90, 0x90, 0xC3});
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  F.End = Va(0x140001008);
  auto Jmp = E.Dis->decodeOne(Va(0x140001000));
  ASSERT_TRUE(Jmp.has_value());
  F.Instrs.push_back(*Jmp);
  auto Ret = E.Dis->decodeOne(Va(0x140001007));
  ASSERT_TRUE(Ret.has_value());
  F.Instrs.push_back(*Ret);
  std::vector<Function> Funcs{F};
  PeFile Go = makePe(E.Img, Va(0x140001000));
  Go.GoBuildId = VaRange{Va(0x140003000), Va(0x140003020)};
  auto Ranges = planOrphanSweep(Go, Funcs).Gaps;
  ASSERT_EQ(Ranges.size(), 1u);
  EXPECT_EQ(Ranges[0].first, Va(0x140001000));
  EXPECT_EQ(Ranges[0].second, Va(0x140001008));
}

// Empty decode is never code.
TEST(OrphanTest, EmptyFunctionRejected) {
  Env E;
  E.addText(Va(0x140001000), {0xC3});
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  EXPECT_FALSE(orphanFunctionOk(*E.Img, F, Va(0x140001001)));
}

// EndOfFunction at exactly the bound is a clean stop (fallthrough into
// the next claimed chunk); mid-gap it means the decode hit junk.
TEST(OrphanTest, EndOfFunctionOnlyAtBound) {
  Env E;
  E.addText(Va(0x140001000), {0x90, 0x90});
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  F.Instrs.push_back(testInstr(Va(0x140001000), 1, FlowKind::None));
  F.Blocks.push_back(
      testBlock(Va(0x140001000), Va(0x140001001), BlockEnd::EndOfFunction));
  EXPECT_TRUE(orphanFunctionOk(*E.Img, F, Va(0x140001001)));
  EXPECT_FALSE(orphanFunctionOk(*E.Img, F, Va(0x140001002)));
}

// The indirect tail-call leaf (`jmp reg`) is accepted: the shape that
// used to sink the probe before `orphan_ok` learned it.
TEST(OrphanTest, IndirectTailJmpRegAccepted) {
  Env E;
  E.addText(Va(0x140001000), {0xFF, 0xE0});
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  auto Jmp = E.Dis->decodeOne(Va(0x140001000));
  ASSERT_TRUE(Jmp.has_value());
  ASSERT_EQ(Jmp->Flow, FlowKind::IndirectBranch);
  F.Instrs.push_back(*Jmp);
  F.Blocks.push_back(testBlock(Va(0x140001000), Va(0x140001002),
                               BlockEnd::UnresolvedIndirect));
  EXPECT_TRUE(orphanFunctionOk(*E.Img, F, Va(0x140001002)));
}

// An indexed dispatch (`jmp [base+idx*scale]`) stays unresolved: a
// failed table enumeration never validates a bogus root.
TEST(OrphanTest, IndexedDispatchRejected) {
  Env E;
  // jmp [rcx+rax*4]: FF 20 (modrm mod=00 reg=100 rm=000, sib scale=2
  // index=000 base=001).
  E.addText(Va(0x140001000), {0xFF, 0x20});
  E.addData(Va(0x140002000), 0x100);
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  auto Jmp = E.Dis->decodeOne(Va(0x140001000));
  ASSERT_TRUE(Jmp.has_value());
  ASSERT_EQ(Jmp->Flow, FlowKind::IndirectBranch);
  F.Instrs.push_back(*Jmp);
  F.Blocks.push_back(testBlock(Va(0x140001000), Va(0x140001002),
                               BlockEnd::UnresolvedIndirect));
  EXPECT_FALSE(orphanFunctionOk(*E.Img, F, Va(0x140001002)));
}

// A rip-relative slot into executable bytes is a failed table, not a
// call; into data it is a thunk and accepted.
TEST(OrphanTest, RipSlotIntoCodeRejectedSlotIntoDataAccepted) {
  Env E;
  E.addText(Va(0x140001000), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x90});
  E.addData(Va(0x140003000), 0x100);
  E.decode();
  // Slot at next_ip + 0 lands at 0x140001006 (executable): rejected.
  auto JmpCode = E.Dis->decodeOne(Va(0x140001000));
  ASSERT_TRUE(JmpCode.has_value());
  Function F;
  F.Start = Va(0x140001000);
  F.Instrs.push_back(*JmpCode);
  F.Blocks.push_back(testBlock(Va(0x140001000), Va(0x140001006),
                               BlockEnd::UnresolvedIndirect));
  EXPECT_FALSE(orphanFunctionOk(*E.Img, F, Va(0x140001006)));

  // Same decode over a data section: slot into non-executable bytes.
  Env D;
  D.addText(Va(0x140004000), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
  D.addData(Va(0x140005000), 0x100);
  D.decode();
  auto JmpData = D.Dis->decodeOne(Va(0x140004000));
  ASSERT_TRUE(JmpData.has_value());
  ASSERT_TRUE(JmpData->HasMemTarget);
  EXPECT_FALSE(D.Img->isExecutable(JmpData->MemTarget));
  Function G;
  G.Start = Va(0x140004000);
  G.Instrs.push_back(*JmpData);
  G.Blocks.push_back(testBlock(Va(0x140004000), Va(0x140004006),
                               BlockEnd::UnresolvedIndirect));
  EXPECT_TRUE(orphanFunctionOk(*D.Img, G, Va(0x140004006)));
}

// Every non-tail edge must land on a block start.
TEST(OrphanTest, EdgeOffBlockStartRejected) {
  Env E;
  E.addText(Va(0x140001000), {0xEB, 0x01, 0x90, 0xC3});
  E.decode();
  Function F;
  F.Start = Va(0x140001000);
  F.Instrs.push_back(testInstr(Va(0x140001000), 2, FlowKind::Branch,
                               Va(0x140001003)));
  F.Instrs.push_back(testInstr(Va(0x140001002), 1, FlowKind::Return));
  F.Blocks.push_back(testBlock(Va(0x140001000), Va(0x140001002),
                               BlockEnd::Branch,
                               {edgeTo(Va(0x140001003))}));
  F.Blocks.push_back(
      testBlock(Va(0x140001002), Va(0x140001003), BlockEnd::Return));
  EXPECT_FALSE(orphanFunctionOk(*E.Img, F, Va(0x140001003)));
  F.Blocks[0].Edges[0].To = Va(0x140001002);
  EXPECT_TRUE(orphanFunctionOk(*E.Img, F, Va(0x140001003)));
}

#ifdef REVENANT_TEST_ORPHAN_EXE
// The fixture must sit outside every .pdata range: only the orphan sweep
// can claim it. Mirrors `orphan_indirect_tailcall_without_pdb`.
TEST(OrphanTest, FixtureOutsidePdata) {
  auto Buf = llvm::MemoryBuffer::getFile(REVENANT_TEST_ORPHAN_EXE);
  ASSERT_TRUE(!!Buf);
  llvm::StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE(!!Pe);
  if (!Pe) {
    llvm::consumeError(Pe.takeError());
    return;
  }
  auto Ranges = planOrphanSweep(*Pe, {}).Gaps;
  // Every pdata range splits the section, so the fixture's gap must show
  // up as a probe range covering it.
  EXPECT_FALSE(Ranges.empty());
  for (const RuntimeFunction &R : Pe->Pdata)
    for (auto [A, B] : Ranges)
      EXPECT_FALSE(A < R.End && R.Begin < B)
          << "probe range claims pdata bytes";
}
#endif

#ifdef REVENANT_TEST_C_ORPHAN_SET
// Orphan chunks never claim bytes inside a .pdata range or another
// function's chunks. Range-level half of `orphans_stay_outside_pdata`:
// probe ranges themselves already exclude every claim.
TEST(OrphanTest, ProbeRangesExcludeAllClaims) {
  for (const char *Path : REVENANT_TEST_C_ORPHAN_SET) {
    auto Buf = llvm::MemoryBuffer::getFile(Path);
    ASSERT_TRUE(!!Buf) << Path;
    llvm::StringRef Data = (*Buf)->getBuffer();
    auto Pe = PeFile::parse(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
    ASSERT_TRUE(!!Pe) << Path;
    if (!Pe) {
      llvm::consumeError(Pe.takeError());
      continue;
    }
    auto Ranges = planOrphanSweep(*Pe, {}).Gaps;
    for (const RuntimeFunction &R : Pe->Pdata) {
      for (auto [A, B] : Ranges)
        EXPECT_FALSE(A < R.End && R.Begin < B)
            << Path << ": range claims pdata";
    }
    for (const VaRange &R : Pe->Img->InlineData) {
      for (auto [A, B] : Ranges)
        EXPECT_FALSE(A < R.End && R.Start < B)
            << Path << ": range claims inline data";
    }
  }
}
#endif

} // end anonymous namespace
