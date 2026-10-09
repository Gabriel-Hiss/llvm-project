#include "revenant/Analysis/Signatures.h"
#include "revenant/Analysis/LibMatch.h"

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
      0x48, 0x8D, 0x44, 0x24, 0x08, // lea rax, [rsp+8]
      0x8B, 0x08,                   // mov ecx, [rax]
      0xC3,                         // ret
  };
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  P.Img->Sections.push_back(std::move(Text));

  Va Start(Base);
  P.Functions.emplace(
      Start, record(Start, 0, 0x1E, {0, 5, 0xA, 0xF, 0x14, 0x16, 0x1B, 0x1D}));
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));

  Signatures S = analyzeSignatures(P, *D);
  const Signature *Sig = S.of(Start);
  ASSERT_NE(Sig, nullptr);
  EXPECT_TRUE(Sig->VarArgs);
  ASSERT_EQ(Sig->Params.size(), 1u);
  EXPECT_EQ(Sig->Params[0].Bits, 32u);
}
TEST(SignaturesTest, OrdinaryHomeSpillsAreNotVarArgs) {
  constexpr uint64_t Base = 0x140003800;
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
  EXPECT_FALSE(Sig->VarArgs);
  EXPECT_EQ(Sig->Params.size(), 4u);
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



TEST(SignaturesTest, TailJumpThunkInheritsTargetParameters) {
  constexpr uint64_t Base = 0x140006000;
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
      0xE9, 0x0B, 0x00, 0x00, 0x00, // jmp target
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0x8B, 0xC1,                   // target: mov eax, ecx
      0xC3,                         // ret
  };
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  P.Img->Sections.push_back(std::move(Text));

  Va Thunk(Base), Target(Base + 0x10);
  P.Functions.emplace(Thunk, record(Thunk, 0, 5, {0}));
  P.Functions.emplace(Target, record(Target, 0x10, 3, {0x10, 0x12}));
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));

  Signatures S = analyzeSignatures(P, *D);
  const Signature *Sig = S.of(Thunk);
  ASSERT_NE(Sig, nullptr);
  ASSERT_EQ(Sig->Params.size(), 1u);
  EXPECT_EQ(Sig->Params[0].Loc.Tag, ParamLoc::Kind::Reg);
  EXPECT_EQ(Sig->Params[0].Bits, 32u);
}

TEST(SignaturesTest, LibraryPrototypeOverridesInferredSignature) {
  constexpr uint64_t Base = 0x140007000;
  Program P;
  P.Img = std::make_shared<Image>();
  P.Img->Base = Va(Base);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(Base);
  Text.Vsize = 0x10;
  Text.Flags = SectionFlags::rx();
  Text.Data = {0x8B, 0xC1, 0xC3};
  P.Img->Sections.push_back(std::move(Text));
  Va Start(Base);
  P.Functions.emplace(Start, record(Start, 0, 3, {0, 2}));
  auto D = Disasm::create(*P.Img);
  ASSERT_TRUE(static_cast<bool>(D));
  Signatures S = analyzeSignatures(P, *D);

  LibraryDatabase DB;
  LibraryFunction F;
  F.Name = "memcpy";
  F.MangledName = "memcpy";
  F.Size = 3;
  F.Bytes = {0x8B, 0xC1, 0xC3};
  LibraryPrototype Prototype;
  Prototype.ReturnType = "void *";
  Prototype.ReturnBits = 64;
  Prototype.Params = {{"destination", "void *", 64, false},
                      {"source", "const void *", 64, false},
                      {"count", "size_t", 64, false}};
  F.Prototype = Prototype;
  DB.Functions.push_back(F);
  LibraryMatch Match{Start, 0};
  applyLibraryMatches(P, S, llvm::ArrayRef<LibraryMatch>(&Match, 1), DB);

  ASSERT_NE(S.of(Start), nullptr);
  EXPECT_EQ(P.Names->at(Start), "memcpy");
  EXPECT_EQ(S.of(Start)->SigSource, Signature::Source::Library);
  ASSERT_EQ(S.of(Start)->Params.size(), 3u);
  EXPECT_EQ(S.of(Start)->Params[0].Name, "destination");
  EXPECT_EQ(S.of(Start)->Params[2].Loc.Tag, ParamLoc::Kind::Reg);
}

TEST(SignaturesTest, LibraryMatchesDisambiguateFunctionNames) {
  Program P;
  P.Names = std::make_shared<std::map<Va, std::string>>();
  Va First(0x140008000), Second(0x140008100);
  FuncRecord FirstFunction, SecondFunction;
  FirstFunction.Start = First;
  SecondFunction.Start = Second;
  FirstFunction.Name = SecondFunction.Name = "Widget::run";
  P.Functions.emplace(First, std::move(FirstFunction));
  P.Functions.emplace(Second, std::move(SecondFunction));
  (*P.Names)[First] = (*P.Names)[Second] = "Widget::run";

  Signatures S;
  LibraryDatabase DB;
  applyLibraryMatches(P, S, {}, DB);

  EXPECT_NE(P.Names->at(First), P.Names->at(Second));
  EXPECT_EQ(P.Functions.at(First).Name, P.Names->at(First));
  EXPECT_EQ(P.Functions.at(Second).Name, P.Names->at(Second));
}
TEST(SignaturesTest, LookupUsesFunctionStart) {
  Signatures S;
  Signature Sig;
  S.Funcs.emplace(Va(0x140001000), Sig);

  ASSERT_NE(S.of(Va(0x140001000)), nullptr);
  EXPECT_EQ(S.of(Va(0x140001001)), nullptr);
}
