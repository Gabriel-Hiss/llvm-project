//==- unittests/Analysis/ClassifyTest.cpp - user/library classification -==//

// The user BFS must stop at libdb-matched callees: user code calling the
// CRT must not pull the whole static CRT into the user set. Uses a real
// corpus binary (c-o2, linked against the same static CRT as the libdb):
// _malloc_base is libdb-matched, directly called from user code, and must
// stay library, never user.

#include "revenant/Engine/Session.h"

#include "gtest/gtest.h"

#include "llvm/Support/Error.h"

#include <optional>

using namespace llvm;
using namespace revenant;

namespace {

TEST(ClassifyTest, CallsIntoMatchedLibraryStayLibrary) {
#ifdef REVENANT_TEST_C_O2
  auto S = Session::open(REVENANT_TEST_C_O2);
  ASSERT_TRUE(!!S) << "cannot open " REVENANT_TEST_C_O2;
  auto C = (*S)->classifyFunctions(
      Lang::C, "C:/Users/User/RustroverProjects/rv-scratch/LunaLibMatch3/libdb.json");
  if (!C) {
    consumeError(C.takeError());
    FAIL() << "classify failed";
    return;
  }
  ASSERT_TRUE(C->UserEntry);
  auto MallocBase = (*S)->functionByName("_malloc_base");
  if (!MallocBase)
    for (const auto &[At, Name] : C->LibraryNames)
      if (Name == "_malloc_base")
        MallocBase = At;
  ASSERT_TRUE(MallocBase) << "_malloc_base not libdb-matched";
  EXPECT_TRUE(C->LibraryStarts.count(*MallocBase))
      << "_malloc_base lost its library mark";
  EXPECT_FALSE(C->UserReachable.count(*MallocBase))
      << "_malloc_base marked user";
#else
  GTEST_SKIP() << "REVENANT_TEST_C_O2 not defined";
#endif
}

TEST(ClassifyTest, CalleeEvidenceKeepsKnownMatch) {
#ifdef REVENANT_TEST_C_O2
  // The evidence extension runs inside classify. It must keep known
  // byte matches library and must not drop them from the library set.
  auto S = Session::open(REVENANT_TEST_C_O2);
  ASSERT_TRUE(!!S) << "cannot open " REVENANT_TEST_C_O2;
  auto C = (*S)->classifyFunctions(
      Lang::C, "C:/Users/User/RustroverProjects/rv-scratch/LunaLibMatch3/libdb.json");
  if (!C) {
    consumeError(C.takeError());
    FAIL() << "classify failed";
    return;
  }
  ASSERT_TRUE(C->UserEntry);
  auto MallocBase = (*S)->functionByName("_malloc_base");
  if (!MallocBase)
    for (const auto &[At, Name] : C->LibraryNames)
      if (Name == "_malloc_base")
        MallocBase = At;
  ASSERT_TRUE(MallocBase);
  EXPECT_TRUE(C->LibraryStarts.count(*MallocBase));
#else
  GTEST_SKIP() << "REVENANT_TEST_C_O2 not defined";
#endif
}

} // namespace
