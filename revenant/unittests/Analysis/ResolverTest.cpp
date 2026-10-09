//==- unittests/Analysis/ResolverTest.cpp - resolver checks --------------==//
//
// Ports crates/analysis/src/resolver.rs: exact hits (names, externs,
// string labels), interior-pointer fallback, and decorateLine substitution
// of branch targets and RIP-relative displacements in Intel lines.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Resolver.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"

#include "gtest/gtest.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace revenant;

namespace {

std::shared_ptr<std::map<Va, std::string>> names(
    std::vector<std::pair<uint64_t, std::string>> Entries = {}) {
  auto M = std::make_shared<std::map<Va, std::string>>();
  for (auto &[Addr, N] : Entries)
    (*M)[Va(Addr)] = N;
  return M;
}

std::shared_ptr<std::map<Va, Extern>> noExterns() {
  return std::make_shared<std::map<Va, Extern>>();
}

std::shared_ptr<std::map<Va, std::string>> noStrings() {
  return std::make_shared<std::map<Va, std::string>>();
}

TEST(ResolverTest, ExactNameHit) {
  Resolver R(names({{0x140001000ULL, "main"}}), noExterns(), noStrings());
  auto N = R.nameAt(Va(0x140001000ULL));
  ASSERT_TRUE(N.has_value());
  EXPECT_EQ(*N, "main");
  EXPECT_FALSE(R.nameAt(Va(0x140001001ULL)).has_value());
}

TEST(ResolverTest, ExternHit) {
  auto E = std::make_shared<std::map<Va, Extern>>();
  (*E)[Va(0x140002000ULL)] = {Va(0x140002000ULL), "kernel32!ExitProcess",
                              true};
  Resolver R(names(), E, noStrings());
  auto N = R.nameAt(Va(0x140002000ULL));
  ASSERT_TRUE(N.has_value());
  EXPECT_EQ(*N, "kernel32!ExitProcess");
}

TEST(ResolverTest, StringLabel) {
  auto S = std::make_shared<std::map<Va, std::string>>();
  (*S)[Va(0x140003000ULL)] = "Hello, World!";
  Resolver R(names(), noExterns(), S);
  auto N = R.nameAt(Va(0x140003000ULL));
  ASSERT_TRUE(N.has_value());
  // alphanumerics uppercased, punctuation dropped.
  EXPECT_EQ(*N, "aHELLOWORLD");
}

TEST(ResolverTest, StringLabelEscapes) {
  auto S = std::make_shared<std::map<Va, std::string>>();
  (*S)[Va(0x140003000ULL)] = ".a_b.c d";
  Resolver R(names(), noExterns(), S);
  auto N = R.nameAt(Va(0x140003000ULL));
  ASSERT_TRUE(N.has_value());
  // leading '.' dropped, '_' and interior '.' kept, space dropped.
  EXPECT_EQ(*N, "aA_B.CD");
}

TEST(ResolverTest, InteriorPointer) {
  Resolver R(names({{0x140001000ULL, "func"}}), noExterns(), noStrings());
  auto N = R.nameFor(Va(0x140001010ULL), RefKind::Data);
  ASSERT_TRUE(N.has_value());
  EXPECT_EQ(*N, "func+0x10");
}

TEST(ResolverTest, InteriorPointerBounds) {
  Resolver R(names({{0x140001000ULL, "func"}}), noExterns(), noStrings());
  // Exact hit wins over the interior rule.
  EXPECT_EQ(*R.nameFor(Va(0x140001000ULL), RefKind::Data), "func");
  // Offset 0 and huge offsets give nothing for a non-string kind.
  EXPECT_FALSE(R.nameFor(Va(0x140000fffULL), RefKind::Jump).has_value());
  EXPECT_FALSE(R.nameFor(Va(0x140011000ULL), RefKind::Jump).has_value());
  EXPECT_FALSE(R.nameFor(Va(0x150001000ULL), RefKind::Call).has_value());
}

TEST(ResolverTest, NamesBeatStrings) {
  auto S = std::make_shared<std::map<Va, std::string>>();
  (*S)[Va(0x140001000ULL)] = "data";
  Resolver R(names({{0x140001000ULL, "func"}}), noExterns(), S);
  EXPECT_EQ(*R.nameFor(Va(0x140001000ULL), RefKind::AddrOf), "func");
}

// Branch target address in the line becomes the resolved name.
TEST(ResolverTest, DecorateBranch) {
  Resolver R(names({{0x140001100ULL, "callee"}}), noExterns(), noStrings());
  Instr I;
  I.Addr = Va(0x140001000ULL);
  I.Size = 5;
  std::vector<std::pair<Va, RefKind>> Refs = {
      {Va(0x140001100ULL), RefKind::Call}};
  EXPECT_EQ(R.decorateLine(I, "call 0x140001100", Refs), "call callee");
}

// RIP-relative displacement becomes the referenced name.
TEST(ResolverTest, DecorateRipRelative) {
  Resolver R(names({{0x140002000ULL, "g_var"}}), noExterns(), noStrings());
  Instr I;
  I.Addr = Va(0x140001000ULL);
  I.Size = 7; // next = 0x140001007, disp = 0xff9
  std::vector<std::pair<Va, RefKind>> Refs = {
      {Va(0x140002000ULL), RefKind::Data}};
  EXPECT_EQ(R.decorateLine(I, "mov rax, [rip + 0xff9]", Refs),
            "mov rax, [g_var]");
}

// Unnamed targets leave the line alone; a longer hex number sharing the
// target as a prefix is not clobbered.
TEST(ResolverTest, DecorateLeavesUnknown) {
  Resolver R(names(), noExterns(), noStrings());
  Instr I;
  I.Addr = Va(0x140001000ULL);
  I.Size = 5;
  std::vector<std::pair<Va, RefKind>> Refs = {
      {Va(0x140001100ULL), RefKind::Call}};
  EXPECT_EQ(R.decorateLine(I, "call 0x140001100", Refs), "call 0x140001100");

  Resolver R2(names({{0x1400ULL, "low"}}), noExterns(), noStrings());
  EXPECT_EQ(R2.decorateLine(I, "mov rax, 0x14000", {}), "mov rax, 0x14000");
}

// End to end: formatted lines from the real disassembler keep their shape
// with a resolver attached, and named branch targets resolve.
TEST(ResolverTest, FormatWithResolver) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_C_O2);
  ASSERT_TRUE((bool)Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE((bool)Pe);
  auto D = Disasm::create(*Pe->Img);
  ASSERT_TRUE((bool)D);

  Va Start = Pe->Pdata[0].Begin;
  auto Instrs = D->decodeRange(Start, Pe->Pdata[0].End);
  ASSERT_FALSE(Instrs.empty());

  // Empty resolver: output equals the plain format.
  auto None = makeResolver(names(), noExterns(), noStrings());
  for (const Instr &I : Instrs)
    EXPECT_EQ(D->format(I, None), D->format(I));

  // Name every direct target `tgt`; each decorated line must contain no
  // raw target address anymore.
  auto N = names();
  for (const Instr &I : Instrs)
    for (auto [Target, Kind] : D->refs(I))
      if (Kind == RefKind::Call || Kind == RefKind::Jump)
        (*N)[Target] = "tgt";
  auto Named = makeResolver(N, noExterns(), noStrings());
  size_t Decorated = 0;
  for (const Instr &I : Instrs) {
    std::string Plain = D->format(I);
    std::string With = D->format(I, Named);
    bool HasNamedRef = false;
    for (auto [Target, Kind] : D->refs(I))
      if ((Kind == RefKind::Call || Kind == RefKind::Jump) &&
          N->count(Target))
        HasNamedRef = true;
    if (HasNamedRef) {
      EXPECT_NE(With.find("tgt"), std::string::npos) << Plain;
      ++Decorated;
    } else {
      EXPECT_EQ(With, Plain) << Plain;
    }
  }
  EXPECT_GT(Decorated, 0u);
}

} // end anonymous namespace
