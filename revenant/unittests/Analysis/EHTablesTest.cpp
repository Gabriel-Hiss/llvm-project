//===- EHTablesTest.cpp - PE exception metadata tests ----------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/EHTables.h"
#include "gtest/gtest.h"

using namespace revenant;

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
