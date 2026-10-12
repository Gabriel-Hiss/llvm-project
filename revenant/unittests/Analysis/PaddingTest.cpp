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
      {".text", 0x400, 0x200, E.Base, 0x200, /*RawSlack=*/{}, /*Executable=*/true});
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
      {".text", 0x400, 0x200, E.Base, 0x200, /*RawSlack=*/{}, /*Executable=*/true});
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
      {".text", 0x400, 0x200, E.Base, 0x200, /*RawSlack=*/{}, /*Executable=*/true});
  auto Runs = decodePadding(Pe, C, {});
  ASSERT_EQ(Runs.size(), 1u);
  EXPECT_EQ(Runs[0].Start, 1u);
  EXPECT_EQ(Runs[0].End, 4u);
}

// `call $+5` is NOT gap filler under the strict reference set: the gap
// stops at the call bytes, and with no aligned leading run it is dropped.
TEST(PaddingTest, CallNextIsNotGapPadding) {
  Env E({0x51, 0xCC, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x51});
  auto C = E.chunks({0, 1, 7, 8});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*RawSlack=*/{}, /*Executable=*/true});
  EXPECT_TRUE(decodePadding(Pe, C, {}).empty());
}

// The opt-in extended rule still accepts the 5-byte self-call.
TEST(PaddingTest, CallNextIsExtFiller) {
  Env E({0xE8, 0x00, 0x00, 0x00, 0x00});
  auto I = E.Dis->decodeOne(E.Base);
  ASSERT_TRUE(I.has_value());
  EXPECT_FALSE(isFiller(*I));
  EXPECT_TRUE(isFillerExt(*I));
}

// Multi-byte nop (`0F 1F 00`: 3 bytes) plus int3 is whole-piece filler.
TEST(PaddingTest, MultiByteNopIsPadding) {
  Env E({0x51, 0x0F, 0x1F, 0x00, 0xCC, 0xCC, 0x51});
  auto C = E.chunks({0, 1, 6, 7});
  PeFile Pe;
  Pe.Img = E.Img;
  Pe.FileSize = 0x1000;
  Pe.RawSections.push_back(
      {".text", 0x400, 0x200, E.Base, 0x200, /*RawSlack=*/{}, /*Executable=*/true});
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
      {".text", 0x400, 0x200, E.Base, 0x200, /*RawSlack=*/{}, /*Executable=*/true});
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

// Function boundary: a closed function ends in `ret`, the linker pads the
// alignment gap with int3, and the next function opens on the 16-byte
// boundary. A tail jump to that address must not absorb the callee.
TEST(PaddingTest, StartAfterRetAndInt3Padding) {
  std::vector<uint8_t> Text(0x20, 0xCC);
  Text[0] = 0xC3; // ret
  Text[0x10] = 0xC3;
  Env E(Text);
  EXPECT_TRUE(startsAfterInt3(*E.Dis, *E.Img, Va(E.Base.Value + 0x10)));
  // The ret itself opens nothing: no padding precedes it.
  EXPECT_FALSE(startsAfterInt3(*E.Dis, *E.Img, Va(E.Base.Value + 0x00)));
}

// An unconditional `jmp` over int3 padding aligns a continuation block
// inside one function; the padding run after a jmp opens no new function.
TEST(PaddingTest, StartAfterJmpOverInt3PaddingIsRejected) {
  std::vector<uint8_t> Text(0x20, 0xCC);
  Text[0] = 0xE9;
  Text[1] = 0x0B;
  Text[2] = 0x00;
  Text[3] = 0x00;
  Text[4] = 0x00; // jmp +0x0b -> 0x10
  Text[0x10] = 0xC3;
  Env E(Text);
  EXPECT_FALSE(startsAfterInt3(*E.Dis, *E.Img, Va(E.Base.Value + 0x10)));
}

// Loop and cold-block alignment inside a function uses the nop family,
// never int3, so a nop-padded block leader is not a function start.
TEST(PaddingTest, StartAfterNopAlignmentIsRejected) {
  std::vector<uint8_t> Text(0x20, 0x90);
  Text[0] = 0xC3; // ret
  Text[0x10] = 0xC3;
  Env E(Text);
  EXPECT_FALSE(startsAfterInt3(*E.Dis, *E.Img, Va(E.Base.Value + 0x10)));
}

// An aligned address with real code (push rcx) before it has no padding
// run and opens nothing.
TEST(PaddingTest, StartWithoutPaddingIsRejected) {
  std::vector<uint8_t> Text(0x20, 0x51);
  Env E(Text);
  EXPECT_FALSE(startsAfterInt3(*E.Dis, *E.Img, Va(E.Base.Value + 0x10)));
}

// An int3 run that closes on a non-16-byte address is not a function
// start even when it follows a ret.
TEST(PaddingTest, UnalignedStartAfterInt3IsRejected) {
  std::vector<uint8_t> Text(0x20, 0xCC);
  Text[0] = 0xC3; // ret
  Env E(Text);
  EXPECT_FALSE(startsAfterInt3(*E.Dis, *E.Img, Va(E.Base.Value + 0x0C)));
}

} // end anonymous namespace
