//==- unittests/Analysis/StringsTest.cpp - string recovery checks --------==//
//
// Ports crates/analysis/src/strings.rs behavior: ASCII >= 4 printable bytes
// and UTF-16 >= 4 non-control chars, readable non-executable sections only,
// with the Rust side's rejection cases (short, non-printable, executable,
// lone surrogates, control chars).
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Strings.h"

#include "gtest/gtest.h"

using namespace revenant;

namespace {

// Image with .text (rx), .rdata (r) and .data (rw) sections.
Image testImage() {
  Image Img;
  Img.Base = Va(0x140000000ULL);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(0x140001000ULL);
  Text.Vsize = 0x1000;
  Text.Data.assign(0x1000, 0x90);
  Text.Flags = SectionFlags::rx();
  Section Rdata;
  Rdata.Name = ".rdata";
  Rdata.SecVa = Va(0x140002000ULL);
  Rdata.Vsize = 0x1000;
  Rdata.Data.assign(0x1000, 0);
  Rdata.Flags = SectionFlags::r();
  Section Data;
  Data.Name = ".data";
  Data.SecVa = Va(0x140003000ULL);
  Data.Vsize = 0x1000;
  Data.Data.assign(0x1000, 0);
  Data.Flags = SectionFlags::rw();
  Img.Sections = {Text, Rdata, Data};
  return Img;
}

void poke(Image &Img, Va Addr, std::vector<uint8_t> Bytes) {
  Section *S = nullptr;
  for (Section &Sec : Img.Sections)
    if (Sec.contains(Addr))
      S = &Sec;
  ASSERT_NE(S, nullptr);
  size_t Off = (size_t)(Addr - S->SecVa);
  ASSERT_LE(Off + Bytes.size(), S->Data.size());
  for (size_t I = 0; I < Bytes.size(); ++I)
    S->Data[Off + I] = Bytes[I];
}

std::vector<uint8_t> wide(const char *S) {
  std::vector<uint8_t> Out;
  for (const char *P = S; *P; ++P) {
    Out.push_back((uint8_t)*P);
    Out.push_back(0);
  }
  Out.push_back(0);
  Out.push_back(0);
  return Out;
}

TEST(StringsTest, AsciiAccepted) {
  Image Img = testImage();
  Va At(0x140002010ULL);
  poke(Img, At, {'H', 'e', 'l', 'l', 'o', 'W', 'o', 'r', 'l', 'd', 0});
  auto S = stringAt(Img, At);
  ASSERT_TRUE(S.has_value());
  EXPECT_EQ(*S, "HelloWorld");
}

TEST(StringsTest, ShortRejected) {
  Image Img = testImage();
  Va At(0x140002010ULL);
  poke(Img, At, {'a', 'b', 'c', 0});
  EXPECT_FALSE(stringAt(Img, At).has_value());
}

TEST(StringsTest, NonPrintableRejected) {
  Image Img = testImage();
  Va At(0x140002010ULL);
  poke(Img, At, {'a', 'b', 0x01, 'd', 'e', 0});
  EXPECT_FALSE(stringAt(Img, At).has_value());
}

TEST(StringsTest, TabAllowed) {
  Image Img = testImage();
  Va At(0x140002010ULL);
  poke(Img, At, {'a', 'b', '\t', 'd', 'e', 0});
  auto S = stringAt(Img, At);
  ASSERT_TRUE(S.has_value());
  EXPECT_EQ(*S, "ab\tde");
}

TEST(StringsTest, ExecutableSectionRejected) {
  Image Img = testImage();
  Va At(0x140001010ULL);
  poke(Img, At, {'H', 'e', 'l', 'l', 'o', 0});
  EXPECT_FALSE(stringAt(Img, At).has_value());
}

TEST(StringsTest, WritableDataAccepted) {
  Image Img = testImage();
  Va At(0x140003010ULL);
  poke(Img, At, {'d', 'a', 't', 'a', '!', 0});
  auto S = stringAt(Img, At);
  ASSERT_TRUE(S.has_value());
  EXPECT_EQ(*S, "data!");
}

TEST(StringsTest, UnmappedRejected) {
  Image Img = testImage();
  EXPECT_FALSE(stringAt(Img, Va(0x180000000ULL)).has_value());
}

TEST(StringsTest, WideAccepted) {
  Image Img = testImage();
  Va At(0x140002020ULL);
  poke(Img, At, wide("Wide"));
  auto S = stringAt(Img, At);
  ASSERT_TRUE(S.has_value());
  EXPECT_EQ(*S, "Wide");
}

TEST(StringsTest, WideShortRejected) {
  Image Img = testImage();
  Va At(0x140002020ULL);
  poke(Img, At, wide("abc"));
  EXPECT_FALSE(stringAt(Img, At).has_value());
}

TEST(StringsTest, WideControlRejected) {
  Image Img = testImage();
  Va At(0x140002020ULL);
  // "ab\x01de" in UTF-16.
  poke(Img, At,
       {'a', 0, 'b', 0, 0x01, 0, 'd', 0, 'e', 0, 0, 0});
  EXPECT_FALSE(stringAt(Img, At).has_value());
}

TEST(StringsTest, WideLoneSurrogateRejected) {
  Image Img = testImage();
  Va At(0x140002020ULL);
  // Lone lead surrogate 0xD800 followed by plain chars, then NUL.
  poke(Img, At,
       {0x00, 0xD8, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 0, 0});
  EXPECT_FALSE(stringAt(Img, At).has_value());
}

TEST(StringsTest, CollectKeepsOnlyStrings) {
  Image Img = testImage();
  Va Good(0x140002010ULL);
  Va Short(0x140002040ULL);
  Va Exec(0x140001020ULL);
  poke(Img, Good, {'g', 'o', 'o', 'd', '!', 0});
  poke(Img, Short, {'n', 'o', 0});
  poke(Img, Exec, {'e', 'x', 'e', 'c', '!', 0});

  Xrefs Xr;
  Xr.Base = Img.Base;
  auto ToRva = [&](Va V) { return (Rva)(V - Img.Base); };
  Xr.Targets = {ToRva(Good), ToRva(Short), ToRva(Exec),
                ToRva(Va(0x180000000ULL))};
  Xr.Offsets = {0, 1, 2, 3, 4};
  Xr.Froms = {ToRva(Va(0x140001000ULL)), ToRva(Va(0x140001005ULL)),
              ToRva(Va(0x14000100aULL)), ToRva(Va(0x14000100fULL))};
  Xr.Kinds = {XrefKind::AddrOf, XrefKind::AddrOf, XrefKind::AddrOf,
              XrefKind::AddrOf};

  auto Got = collectStrings(Img, Xr);
  ASSERT_EQ(Got.size(), 1u);
  EXPECT_EQ(Got.begin()->first, Good);
  EXPECT_EQ(Got.begin()->second, "good!");
}

} // end anonymous namespace
