//==- unittests/Analysis/AnalysisTest.cpp - discovery integration ------==//
//
// Exercise the public analysis entry point against the prebuilt C++ parity
// binary. The test never rebuilds or reads its oracle.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Analysis.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include "gtest/gtest.h"

#include <memory>
#include <optional>

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace revenant;

TEST(AnalysisTest, DiscoversFunctionsFromCorpusBinary) {
  auto Buffer = MemoryBuffer::getFile(REVENANT_TEST_C_O2);
  ASSERT_TRUE(Buffer);
  StringRef Bytes = (*Buffer)->getBuffer();
  auto Parsed = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()));
  if (!Parsed) {
    consumeError(Parsed.takeError());
    FAIL() << "The C++ O2 PE fixture could not be parsed";
    return;
  }
  PeFile Pe = std::move(*Parsed);
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);

  Program P = analyze(Pe, *Hooks);
  ASSERT_FALSE(P.Functions.empty());
  EXPECT_NE(P.Functions.find(Pe.EntryPoint), P.Functions.end());
  EXPECT_NE(functionAt(P, Pe.EntryPoint), nullptr);
  EXPECT_TRUE(P.Strings);
  EXPECT_TRUE(P.Externs);
  EXPECT_TRUE(P.Names);

  auto Items = instructionItems(P);
  if (!Items) {
    consumeError(Items.takeError());
    FAIL() << "The analysis instruction listing could not be decoded";
    return;
  }
  EXPECT_FALSE(Items->empty());
}
