//===- EHTablesTest.cpp - PE exception metadata tests ----------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/EHTables.h"
#include "gtest/gtest.h"
#include "llvm/Support/Error.h"

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
    put32(D, 28, 4);
    put32(D, 32, 0x1300);
    // One try record and one handler type entry.
    put32(D, 0x200, 1);
    put32(D, 0x204, 1);
    put32(D, 0x208, 0);
    put32(D, 0x20c, 1);
    put32(D, 0x210, 0x1220);
    // Two adjacent IP-map entries have the active try state.
    put32(D, 0x300, 0x100);
    put32(D, 0x304, 0);
    put32(D, 0x308, 0x110);
    put32(D, 0x30c, 1);
    put32(D, 0x310, 0x118);
    put32(D, 0x314, 1);
    put32(D, 0x318, 0x120);
    put32(D, 0x31c, 0);
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
  ASSERT_EQ(EH[0].TryRegions[0].Handlers.size(), 1u);
  EXPECT_EQ(EH[0].TryRegions[0].Handlers[0], 0u);
  ASSERT_EQ(EH[0].Handlers.size(), 1u);
  EXPECT_EQ(EH[0].Handlers[0].Address, Va(0x2300));
  EXPECT_EQ(EH[0].Handlers[0].TypeName, ".H");
  EXPECT_EQ(EH[0].TryRegions[0].Begin, Va(0x1110));
  EXPECT_EQ(EH[0].TryRegions[0].End, Va(0x1120));
}
TEST(EHTablesTest, DecodesCleanupOnlyFuncInfo3RangesFromIPStates) {
  PeFile PE = makePE(true);
  auto &D = PE.Img->Sections[1].Data;
  put32(D, 12, 0);
  put32(D, 16, 0x1280);
  put32(D, 20, 0);
  put32(D, 24, 0);
  put32(D, 28, 4);
  put32(D, 32, 0x1300);
  put32(D, 0x280, 0);
  put32(D, 0x284, 0x1300);

  auto EH = decodeEHTables(PE);
  ASSERT_EQ(EH.size(), 1u);
  ASSERT_EQ(EH[0].Cleanups.size(), 1u);
  ASSERT_EQ(EH[0].TryRegions.size(), 2u);
  EXPECT_EQ(EH[0].TryRegions[0].Begin, Va(0x1100));
  EXPECT_EQ(EH[0].TryRegions[0].End, Va(0x1110));
  EXPECT_EQ(EH[0].TryRegions[1].Begin, Va(0x1120));
  EXPECT_EQ(EH[0].TryRegions[1].End, Va(0x1180));
}

TEST(EHTablesTest, DecodesSehScopeRangeAndFinally) {
  auto EH = decodeEHTables(makePE(false));
  ASSERT_EQ(EH.size(), 1u);
  EXPECT_EQ(EH[0].Model, EHModel::SEH);
  ASSERT_EQ(EH[0].TryRegions.size(), 1u);
  EXPECT_EQ(EH[0].TryRegions[0].Begin, Va(0x1100));
  EXPECT_EQ(EH[0].TryRegions[0].End, Va(0x1180));
  ASSERT_EQ(EH[0].Handlers.size(), 1u);
  ASSERT_EQ(EH[0].TryRegions[0].Handlers.size(), 1u);
  EXPECT_EQ(EH[0].TryRegions[0].Handlers[0], 0u);
  EXPECT_EQ(EH[0].Handlers[0].Kind, HandlerKind::Finally);
  ASSERT_EQ(EH[0].Cleanups.size(), 1u);
}

TEST(EHTablesTest, DecodesExceptionMetadataFromCorpus) {
  const char *Paths[] = {
      REVENANT_TEST_CPP_O2,
      REVENANT_TEST_CPP_CLANG_O2,
      REVENANT_TEST_CPP_GCC_O2,
      REVENANT_TEST_RUST_O2,
      REVENANT_TEST_JSON_O2,
      REVENANT_TEST_SPDLOG_O2,
  };
  for (const char *Path : Paths) {
    auto Loaded = PeFile::load(Path);
    ASSERT_TRUE(!!Loaded) << Path;
    auto EH = decodeEHTables(*Loaded);
    if (llvm::StringRef(Path) == REVENANT_TEST_CPP_O2) {
      const FunctionEH *Exceptions = nullptr;
      for (const FunctionEH &F : EH)
        if (F.Function == Va(0x140003200))
          Exceptions = &F;
      ASSERT_NE(Exceptions, nullptr);
      EXPECT_EQ(Exceptions->Model, EHModel::MSVC);
      EXPECT_EQ(Exceptions->TryRegions.size(), 1u);
      ASSERT_EQ(Exceptions->Handlers.size(), 2u);
      EXPECT_EQ(Exceptions->Handlers[0].Address, Va(0x14006fb00));
      EXPECT_EQ(Exceptions->Handlers[0].TypeName, ".?AVruntime_error@std@@");
      EXPECT_EQ(Exceptions->Handlers[1].Address, Va(0x14006fae0));
      EXPECT_EQ(Exceptions->Handlers[1].TypeName, ".?AVexception@std@@");
      ASSERT_EQ(Exceptions->Handlers[0].Continuations.size(), 1u);
      EXPECT_EQ(Exceptions->Handlers[0].Continuations[0], Va(0x140003219));
      ASSERT_EQ(Exceptions->Handlers[1].Continuations.size(), 1u);
      EXPECT_EQ(Exceptions->Handlers[1].Continuations[0], Va(0x14000320f));
    }
    if (llvm::StringRef(Path) == REVENANT_TEST_CPP_GCC_O2) {
      bool HasTypedItaniumCatch = false;
      for (const FunctionEH &F : EH)
        if (F.Model == EHModel::Itanium)
          for (const EHHandler &H : F.Handlers)
            HasTypedItaniumCatch |= H.Kind == HandlerKind::Catch &&
                                    H.TypeDescriptor.has_value() &&
                                    !H.TypeName.empty();
      EXPECT_TRUE(HasTypedItaniumCatch);
    }
    EXPECT_FALSE(EH.empty()) << Path;
    size_t WithContent = 0;
    for (const FunctionEH &F : EH) {
      if (!F.TryRegions.empty() || !F.Handlers.empty() ||
          !F.Cleanups.empty() || !F.CallSites.empty())
        ++WithContent;
      EXPECT_FALSE(F.TryRegions.empty() && F.Handlers.empty() &&
                   F.Cleanups.empty() && F.CallSites.empty());
      for (const EHTryRegion &T : F.TryRegions) {
        EXPECT_LT(T.Begin, T.End) << Path;
        EXPECT_GE(T.Begin, F.Function) << Path;
        EXPECT_LE(T.End, F.End) << Path;
      }
      for (const EHHandler &H : F.Handlers)
        EXPECT_NE(Loaded->Img->sectionOf(H.Address), nullptr) << Path;
      for (const EHCleanup &C : F.Cleanups)
        EXPECT_NE(Loaded->Img->sectionOf(C.Address), nullptr) << Path;
    }
    llvm::errs() << Path << " entries=" << EH.size()
                 << " withcontent=" << WithContent << "\n";
    EXPECT_EQ(WithContent, EH.size()) << Path;
  }
}
