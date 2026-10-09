//===- EHTablesTest.cpp - PE exception metadata tests ----------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/EHTables.h"
#include "gtest/gtest.h"

using namespace revenant;

namespace {
void put32(std::vector<uint8_t> &B, size_t Off, uint32_t V) {
  B[Off] = static_cast<uint8_t>(V);
  B[Off + 1] = static_cast<uint8_t>(V >> 8);
  B[Off + 2] = static_cast<uint8_t>(V >> 16);
  B[Off + 3] = static_cast<uint8_t>(V >> 24);
}

PeFile makePE(bool Cxx) {
  PeFile PE;
  PE.Img = std::make_shared<Image>();
  PE.Img->Base = Va(0x1000);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(0x1000);
  Text.Vsize = 0x1000;
  Text.Data.resize(0x1000);
  Text.Flags = SectionFlags::rx();
  Section Xdata;
  Xdata.Name = ".xdata";
  Xdata.SecVa = Va(0x2000);
  Xdata.Vsize = 0x1000;
  Xdata.Data.resize(0x1000);
  Xdata.Flags = SectionFlags::r();
  // UNWIND_INFO with UNW_FLAG_EHANDLER and a handler RVA of 0x1300.
  Xdata.Data[0] = 9;
  put32(Xdata.Data, 4, 0x1300);
  PE.Img->Sections.push_back(std::move(Text));
  PE.Img->Sections.push_back(std::move(Xdata));
  RuntimeFunction RF;
  RF.Begin = Va(0x1100);
  RF.End = Va(0x1180);
  RF.Unwind = Va(0x2000);
  PE.Pdata.push_back(RF);
  if (Cxx) {
    auto &D = PE.Img->Sections[1].Data;
    put32(D, 8, 0x19930524);
    put32(D, 12, 0);
    put32(D, 16, 0);
    put32(D, 20, 1);
    put32(D, 24, 0x1200);
    put32(D, 28, 0);
    put32(D, 32, 0);
    // One try record and one handler type entry.
    put32(D, 0x200, 0);
    put32(D, 0x204, 0);
    put32(D, 0x208, 0);
    put32(D, 0x20c, 1);
    put32(D, 0x210, 0x1220);
    put32(D, 0x220, 0);
    put32(D, 0x224, 0x1240);
    put32(D, 0x228, 0);
    put32(D, 0x22c, 0x1300);
    put32(D, 0x230, 0);
    // TypeDescriptor decorated name at descriptor + 16.
    const char Name[] = ".H";
    std::copy(Name, Name + sizeof(Name), D.begin() + 0x250);
  } else {
    auto &D = PE.Img->Sections[1].Data;
    put32(D, 8, 1);
    put32(D, 12, 0x100);
    put32(D, 16, 0x180);
    put32(D, 24, 0x1300);
  }
  return PE;
}
} // namespace

TEST(EHTablesTest, EmptyPdataHasNoFunctions) {
  PeFile PE;
  PE.Img = std::make_shared<Image>();
  EXPECT_TRUE(decodeEHTables(PE).empty());
}

TEST(EHTablesTest, ChainedRuntimeFunctionsAreNotReportedTwice) {
  PeFile PE;
  PE.Img = std::make_shared<Image>();
  RuntimeFunction Chained;
  Chained.Begin = Va(0x1000);
  Chained.End = Va(0x1010);
  Chained.Unwind = Va(0x2000);
  Chained.ChainedTo = Va(0x1000);
  PE.Pdata.push_back(Chained);
  EXPECT_TRUE(decodeEHTables(PE).empty());
}

TEST(EHTablesTest, DecodesFuncInfo3CatchHandlerAndTypeName) {
  auto EH = decodeEHTables(makePE(true));
  ASSERT_EQ(EH.size(), 1u);
  EXPECT_EQ(EH[0].Model, EHModel::MSVC);
  ASSERT_EQ(EH[0].TryRegions.size(), 1u);
  ASSERT_EQ(EH[0].Handlers.size(), 1u);
  EXPECT_EQ(EH[0].Handlers[0].Address, Va(0x2300));
  EXPECT_EQ(EH[0].Handlers[0].TypeName, ".H");
}

TEST(EHTablesTest, DecodesSehScopeRangeAndFinally) {
  auto EH = decodeEHTables(makePE(false));
  ASSERT_EQ(EH.size(), 1u);
  EXPECT_EQ(EH[0].Model, EHModel::SEH);
  ASSERT_EQ(EH[0].TryRegions.size(), 1u);
  EXPECT_EQ(EH[0].TryRegions[0].Begin, Va(0x1100));
  EXPECT_EQ(EH[0].TryRegions[0].End, Va(0x1180));
  ASSERT_EQ(EH[0].Handlers.size(), 1u);
  EXPECT_EQ(EH[0].Handlers[0].Kind, HandlerKind::Finally);
}
