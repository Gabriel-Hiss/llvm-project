#include "revenant/Analysis/Signatures.h"

#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"

#include <algorithm>
#include <iterator>
#include "gtest/gtest.h"

using namespace revenant;

namespace {
FuncRecord record(Va Start, uint32_t StartRva, uint32_t Length,
                  std::vector<Rva> InstrRvas) {
  FuncRecord F;
  F.Start = Start;
  F.End = Start.wrappingAdd(Length);
  F.InstrRvas = std::move(InstrRvas);
  F.Chunks.push_back({Start, F.End});
  BlockRec B;
  B.Start = StartRva;
  B.End = StartRva + Length;
  B.InstrsEnd = F.InstrRvas.size();
  B.EndKind = BlockEnd::Return;
  F.Blocks.push_back(B);
  return F;
}

} // namespace

TEST(SignaturesTest, EmptyProgramHasNoRecoveredEntries) {
  Program P;
  P.Img = std::make_shared<Image>();
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));

  Signatures S = analyzeSignatures(P, *D);
  EXPECT_TRUE(S.Funcs.empty());
  EXPECT_TRUE(S.Externs.empty());
  EXPECT_TRUE(S.IndirectArgs.empty());
  EXPECT_TRUE(S.CallsiteArgs.empty());
}

TEST(SignaturesTest, InfersRegisterParameterAndObservedReturn) {
  constexpr uint64_t Base = 0x140001000;
  Program P;
  P.Img = std::make_shared<Image>();
  P.Img->Base = Va(Base);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(Base);
  Text.Vsize = 0x20;
  Text.Flags = SectionFlags::rx();
  Text.Data.resize(Text.Vsize, 0xCC);
  // callee: mov eax, ecx; ret
  Text.Data[0] = 0x8B;
  Text.Data[1] = 0xC1;
  Text.Data[2] = 0xC3;
  // caller: call callee; test eax,eax; ret
  Text.Data[0x10] = 0xE8;
  Text.Data[0x11] = 0xEB;
  Text.Data[0x12] = 0xFF;
  Text.Data[0x13] = 0xFF;
  Text.Data[0x14] = 0xFF;
  Text.Data[0x15] = 0x85;
  Text.Data[0x16] = 0xC0;
  Text.Data[0x17] = 0xC3;
  P.Img->Sections.push_back(std::move(Text));

  Va Callee(Base), Caller(Base + 0x10);
  P.Functions.emplace(Callee, record(Callee, 0, 3, {0, 2}));
  P.Functions.emplace(Caller, record(Caller, 0x10, 8, {0x10, 0x15, 0x17}));
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));

  Signatures S = analyzeSignatures(P, *D);
  const Signature *Sig = S.of(Callee);
  ASSERT_NE(Sig, nullptr);
  ASSERT_EQ(Sig->Params.size(), 1u);
  EXPECT_EQ(Sig->Params[0].Loc.Tag, ParamLoc::Kind::Reg);
  EXPECT_EQ(Sig->Params[0].Bits, 32u);
  EXPECT_TRUE(Sig->HasRet);
  EXPECT_EQ(Sig->Ret.Bits, 32u);
}

TEST(SignaturesTest, RecognizesVarArgsHomeSpillsWithoutInventingParameters) {
  constexpr uint64_t Base = 0x140003000;
  Program P;
  P.Img = std::make_shared<Image>();
  P.Img->Base = Va(Base);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(Base);
  Text.Vsize = 0x20;
  Text.Flags = SectionFlags::rx();
  Text.Data.resize(Text.Vsize, 0xCC);
  const uint8_t Code[] = {
      0x48, 0x89, 0x4C, 0x24, 0x08, // mov [rsp+8], rcx
      0x48, 0x89, 0x54, 0x24, 0x10, // mov [rsp+16], rdx
      0x4C, 0x89, 0x44, 0x24, 0x18, // mov [rsp+24], r8
      0x4C, 0x89, 0x4C, 0x24, 0x20, // mov [rsp+32], r9
      0x8B, 0xC1,                   // mov eax, ecx
      0xC3,                         // ret
  };
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  P.Img->Sections.push_back(std::move(Text));

  Va Start(Base);
  P.Functions.emplace(
      Start, record(Start, 0, 0x17, {0, 5, 0xA, 0xF, 0x14, 0x16}));
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));

  Signatures S = analyzeSignatures(P, *D);
  const Signature *Sig = S.of(Start);
  ASSERT_NE(Sig, nullptr);
  EXPECT_TRUE(Sig->VarArgs);
  ASSERT_EQ(Sig->Params.size(), 1u);
  EXPECT_EQ(Sig->Params[0].Bits, 32u);
}

TEST(SignaturesTest, RmwDestinationReadCountsAsParameter) {
  constexpr uint64_t Base = 0x140004000;
  Program P;
  P.Img = std::make_shared<Image>();
  P.Img->Base = Va(Base);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(Base);
  Text.Vsize = 0x20;
  Text.Flags = SectionFlags::rx();
  Text.Data.resize(Text.Vsize, 0xCC);
  const uint8_t Code[] = {
      0x83, 0xE1, 0x3F, // and ecx, 0x3f
      0xC1, 0xE1, 0x18, // shl ecx, 24
      0x33, 0xD2,       // xor edx, edx (zeroing idiom, not a param)
      0x8B, 0xC1,       // mov eax, ecx
      0xC3,             // ret
  };
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  P.Img->Sections.push_back(std::move(Text));

  Va Start(Base);
  P.Functions.emplace(Start, record(Start, 0x0, 0xB, {0, 3, 6, 8, 0xA}));
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));

  Signatures S = analyzeSignatures(P, *D);
  const Signature *Sig = S.of(Start);
  ASSERT_NE(Sig, nullptr);
  // `and`/`shl` read their destination: rcx is a parameter.
  ASSERT_EQ(Sig->Params.size(), 1u);
  EXPECT_EQ(Sig->Params[0].Bits, 32u);
}

TEST(SignaturesTest, LookupUsesFunctionStart) {
  Signatures S;
  Signature Sig;
  S.Funcs.emplace(Va(0x140001000), Sig);

  ASSERT_NE(S.of(Va(0x140001000)), nullptr);
  EXPECT_EQ(S.of(Va(0x140001001)), nullptr);
}
