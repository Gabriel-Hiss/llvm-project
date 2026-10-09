//==- unittests/Analysis/PaddingTest.cpp - padding checks ------------*- C++ -*-==//
//
// Port intent of crates/analysis padding rules: whole-piece nop/int3
// gaps decode as padding, mixed gaps keep only an aligned leading run,
// interior/suffix filler is never padding, filler_head walks back to the
// run head, and padLen counts zero/int3 bytes plus filler instructions.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Padding.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Disasm/Disasm.h"

#include "gtest/gtest.h"

using namespace revenant;

namespace {

struct Env {
  std::shared_ptr<Image> Img;
  std::optional<Disasm> Dis;
  Va Base{0x140000000};

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

  std::vector<std::pair<Va, Va>> chunks(std::vector<uint64_t> Bounds) {
    std::vector<std::pair<Va, Va>> Out;
    for (size_t I = 0; I + 1 < Bounds.size(); I += 2)
      Out.emplace_back(Va(Base.Value + Bounds[I]),
                       Va(Base.Value + Bounds[I + 1]));
    return Out;
  }
};

// Bytes: push rcx (0x51, not filler) then nops; chunk gap 1..9 is a whole
// nop piece and becomes one run.
TEST(PaddingTest, WholeNopGapIsPadding) {
  Env E({0x51, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x51});
  auto C = E.chunks({0, 1, 10, 11});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 10u);
  EXPECT_EQ(Runs[0].Instrs.size(), 9u);
}

// Mixed gap (nops then a ret) only keeps a leading run when it closes on a
// 16-byte boundary; here the filler ends mid-alignment, so nothing.
TEST(PaddingTest, MixedGapWithoutAlignedEndIsRejected) {
  Env E({0x51, 0x90, 0x90, 0xC3, 0x51});
  auto C = E.chunks({0, 1, 4, 5});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  EXPECT_TRUE(decodePadding(Pe, C, {}).empty());
}

// int3 run wholly inside the gap is a run; a non-filler byte (0x51)
// leaves only the unaligned prefix, which is dropped.
TEST(PaddingTest, Int3WholeGapIsPadding) {
  Env E({0x51, 0xCC, 0xCC, 0xCC, 0x51});
  auto C = E.chunks({0, 1, 4, 5});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 4u);
}

// The 5-byte `call $+5` decodes as one filler instruction.
TEST(PaddingTest, CallNextIsPadding) {
  // chunk, int3 + call $+5, chunk.
  Env E({0x51, 0xCC, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x51});
  auto C = E.chunks({0, 1, 7, 8});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 7u);
  ASSERT_EQ(Runs[0].Instrs.size(), 2u);
}

// A bare `call $+5` with no int3 slack is filler too: the decoder-only
// rule accepts any direct 5-byte self-call as `call $+5`.
TEST(PaddingTest, BareCallNextIsPadding) {
  Env E({0x51, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x51});
  auto C = E.chunks({0, 1, 6, 7});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 6u);
  ASSERT_EQ(Runs[0].Instrs.size(), 1u);
}

// Multi-byte nop (`0F 1F 00`: 3 bytes) plus int3 is whole-piece filler.
TEST(PaddingTest, MultiByteNopIsPadding) {
  Env E({0x51, 0x0F, 0x1F, 0x00, 0xCC, 0xCC, 0x51});
  auto C = E.chunks({0, 1, 6, 7});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 6u);
}

// `66 90` (xchg ax,ax) is filler.
TEST(PaddingTest, XchgAxAxIsPadding) {
  Env E({0x51, 0x66, 0x90, 0x51});
  auto C = E.chunks({0, 1, 3, 4});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 3u);
}

// fillerHead: an address inside a 3-byte nop resolves to the run head;
// an instruction boundary returns nullopt.
TEST(PaddingTest, FillerHeadWalksBack) {
  Env E({0x51, 0x0F, 0x1F, 0x00, 0xCC});
  Va Mid(E.Base.Value + 2);
  auto H = fillerHead(*E.Dis, Mid);
  ASSERT_TRUE(H.has_value());
  EXPECT_EQ(H->Value, E.Base.Value + 1);
  EXPECT_FALSE(fillerHead(*E.Dis, Va(E.Base.Value + 1)).has_value());
}

// padLen: zero and int3 bytes are 1; a nop decodes to its length;
// real code is 0.
TEST(PaddingTest, PadLenCountsBytesAndFillers) {
  Env E({0x00, 0xCC, 0x90, 0x51});
  EXPECT_EQ(padLen(*E.Dis, *E.Img, Va(E.Base.Value + 0)), 1u);
  EXPECT_EQ(padLen(*E.Dis, *E.Img, Va(E.Base.Value + 1)), 1u);
  EXPECT_EQ(padLen(*E.Dis, *E.Img, Va(E.Base.Value + 2)), 1u);
  EXPECT_EQ(padLen(*E.Dis, *E.Img, Va(E.Base.Value + 3)), 0u);
}

} // end anonymous namespace
