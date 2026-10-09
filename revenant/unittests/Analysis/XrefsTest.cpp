//==- unittests/Analysis/XrefsTest.cpp - xref table checks --------------==//
//
// The builder must match crates/analysis collect_xrefs: direct calls/jumps
// land with Call/Jump kinds, RIP-relative data refs split Read/Write,
// out-of-image targets are dropped, and the table stays sorted with binary
// search lookups agreeing with a brute-force walk over the same instrs.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Xrefs.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"

#include "gtest/gtest.h"

#include <map>

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace revenant;

namespace {

struct Fixture {
  std::unique_ptr<MemoryBuffer> Buf;
  std::optional<PeFile> Pe;
  std::optional<Disasm> Dis;
};

std::optional<Fixture> openCorpus() {
  Fixture F;
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_C_O2);
  if (!Buf)
    return std::nullopt;
  F.Buf = std::move(*Buf);
  StringRef Data = F.Buf->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
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

// Synthetic image: one executable + one data section under a fixed base.
Image testImage() {
  Image Img;
  Img.Base = Va(0x140000000ULL);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(0x140001000ULL);
  Text.Vsize = 0x1000;
  Text.Data.assign(0x1000, 0x90);
  Text.Flags = SectionFlags::rx();
  Section Data;
  Data.Name = ".rdata";
  Data.SecVa = Va(0x140002000ULL);
  Data.Vsize = 0x1000;
  Data.Data.assign(0x1000, 0);
  Data.Flags = SectionFlags::r();
  Img.Sections = {Text, Data};
  return Img;
}

// Hand-built direct call: no MC opcode needed, refs() only reads Flow and
// the target fields for direct branches.
Instr directCall(Va From, Va Target) {
  Instr I;
  I.Addr = From;
  I.Size = 5;
  I.Flow = FlowKind::Call;
  I.HasTarget = true;
  I.Target = Target;
  return I;
}

Instr directJump(Va From, Va Target) {
  Instr I;
  I.Addr = From;
  I.Size = 2;
  I.Flow = FlowKind::Branch;
  I.HasTarget = true;
  I.Target = Target;
  return I;
}

TEST(XrefsTest, DirectCallsAndJumps) {
  Image Img = testImage();
  auto D = Disasm::create(Img);
  ASSERT_TRUE((bool)D);
  std::vector<Instr> Instrs = {
      directCall(Va(0x140001000ULL), Va(0x140001100ULL)),
      directJump(Va(0x140001005ULL), Va(0x140001100ULL)),
      directCall(Va(0x140001010ULL), Va(0x140002000ULL)),
  };
  Xrefs Xr = collectXrefs(Img, ArrayRef<Instr>(Instrs), *D);

  ASSERT_EQ(Xr.targetCount(), 2u);
  EXPECT_EQ(Xr.totalRefs(), 3u);
  // Sorted targets.
  EXPECT_LT(Xr.Targets[0], Xr.Targets[1]);
  // Offsets: leading 0, cumulative, trailing == total.
  ASSERT_EQ(Xr.Offsets.size(), 3u);
  EXPECT_EQ(Xr.Offsets[0], 0u);
  EXPECT_EQ(Xr.Offsets[2], 3u);

  auto ToCode = Xr.xrefsTo(Va(0x140001100ULL));
  ASSERT_EQ(ToCode.size(), 2u);
  EXPECT_EQ(ToCode[0].From, Va(0x140001000ULL));
  EXPECT_EQ(ToCode[0].Kind, XrefKind::Call);
  EXPECT_EQ(ToCode[1].From, Va(0x140001005ULL));
  EXPECT_EQ(ToCode[1].Kind, XrefKind::Jump);

  auto ToData = Xr.xrefsTo(Va(0x140002000ULL));
  ASSERT_EQ(ToData.size(), 1u);
  EXPECT_EQ(ToData[0].Kind, XrefKind::Call);
}

TEST(XrefsTest, OutOfImageTargetsDropped) {
  Image Img = testImage();
  auto D = Disasm::create(Img);
  ASSERT_TRUE((bool)D);
  std::vector<Instr> Instrs = {
      directCall(Va(0x140001000ULL), Va(0x180000000ULL)), // unmapped
      directCall(Va(0x140001005ULL), Va(0x140001100ULL)),
  };
  Xrefs Xr = collectXrefs(Img, ArrayRef<Instr>(Instrs), *D);
  EXPECT_EQ(Xr.targetCount(), 1u);
  EXPECT_TRUE(Xr.xrefsTo(Va(0x180000000ULL)).empty());
}

TEST(XrefsTest, MissesReturnEmpty) {
  Image Img = testImage();
  auto D = Disasm::create(Img);
  ASSERT_TRUE((bool)D);
  std::vector<Instr> Instrs = {
      directCall(Va(0x140001000ULL), Va(0x140001100ULL)),
  };
  Xrefs Xr = collectXrefs(Img, ArrayRef<Instr>(Instrs), *D);
  EXPECT_TRUE(Xr.xrefsTo(Va(0x140001000ULL)).empty()); // a source, no target
  EXPECT_TRUE(Xr.xrefsTo(Va(0x140000000ULL - 1)).empty()); // below base
}

// Every refs() pair over a real function range must be in the table with
// the same kind, and lookups must agree with a brute-force scan.
TEST(XrefsTest, RealFunctionRoundTrip) {
  auto F = openCorpus();
  ASSERT_TRUE(F.has_value());
  ASSERT_FALSE(F->Pe->Pdata.empty());
  Va Start = F->Pe->Pdata[0].Begin;
  Va End = F->Pe->Pdata[0].End;
  auto Instrs = F->Dis->decodeRange(Start, End);
  ASSERT_FALSE(Instrs.empty());

  Xrefs Xr = collectXrefs(*F->Pe->Img, ArrayRef<Instr>(Instrs), *F->Dis);

  // Table invariants.
  for (size_t I = 1; I < Xr.Targets.size(); ++I)
    EXPECT_LT(Xr.Targets[I - 1], Xr.Targets[I]);
  ASSERT_EQ(Xr.Offsets.size(), Xr.Targets.size() + 1);
  EXPECT_EQ(Xr.Offsets.front(), 0u);
  EXPECT_EQ(Xr.Offsets.back(), Xr.Froms.size());
  for (size_t I = 1; I < Xr.Offsets.size(); ++I)
    EXPECT_LE(Xr.Offsets[I - 1], Xr.Offsets[I]);

  // Brute-force expectation from the same refs().
  std::map<Va, std::vector<std::pair<Va, XrefKind>>> Want;
  for (const Instr &I : Instrs)
    for (auto [Target, Kind] : F->Dis->refs(I)) {
      if (!F->Pe->Img->sectionOf(Target))
        continue;
      XrefKind XK = XrefKind::Read;
      switch (Kind) {
      case RefKind::Call:
        XK = XrefKind::Call;
        break;
      case RefKind::Jump:
        XK = XrefKind::Jump;
        break;
      case RefKind::AddrOf:
        XK = XrefKind::AddrOf;
        break;
      case RefKind::Data:
        XK = F->Dis->isStore(I) ? XrefKind::Write : XrefKind::Read;
        break;
      }
      Want[Target].push_back({I.Addr, XK});
    }

  ASSERT_EQ(Xr.targetCount(), Want.size());
  for (auto &[Target, Refs] : Want) {
    auto Got = Xr.xrefsTo(Target);
    ASSERT_EQ(Got.size(), Refs.size()) << "at " << Target.Value;
    for (size_t I = 0; I < Refs.size(); ++I) {
      EXPECT_EQ(Got[I].From, Refs[I].first);
      EXPECT_EQ(Got[I].Kind, Refs[I].second);
    }
  }

  // Somewhere in the first function there is at least one code xref.
  bool SawCode = false;
  for (auto &[Target, Refs] : Want)
    for (auto &[From, Kind] : Refs)
      if (Kind == XrefKind::Call || Kind == XrefKind::Jump)
        SawCode = true;
  EXPECT_TRUE(SawCode);
}

// The Function overload walks functions in order; within-target ref order
// matches the flat instruction walk.
TEST(XrefsTest, FunctionOverloadOrder) {
  Image Img = testImage();
  auto D = Disasm::create(Img);
  ASSERT_TRUE((bool)D);
  Function A, B;
  A.Start = Va(0x140001000ULL);
  A.Instrs = {directCall(Va(0x140001000ULL), Va(0x140001100ULL))};
  B.Start = Va(0x140001050ULL);
  B.Instrs = {directJump(Va(0x140001050ULL), Va(0x140001100ULL))};
  std::vector<Function> Funcs = {A, B};
  Xrefs Xr = collectXrefs(Img, ArrayRef<Function>(Funcs), *D);
  auto Got = Xr.xrefsTo(Va(0x140001100ULL));
  ASSERT_EQ(Got.size(), 2u);
  EXPECT_EQ(Got[0].From, Va(0x140001000ULL));
  EXPECT_EQ(Got[1].From, Va(0x140001050ULL));
}

} // end anonymous namespace
