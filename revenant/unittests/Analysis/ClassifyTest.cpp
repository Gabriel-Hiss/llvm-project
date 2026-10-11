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

// Board line 347: stripped static-MinGW PEs (no mingw/libgcc import DLLs)
// must still detect as MinGwGcc via .CRT/.tls sections + GCC comment +
// Mingw-w64 runtime strings; MSVC c-o2 and clang-cl json-cl-o2 must stay
// non-MinGW so the __try/__except gate keeps firing for them.
TEST(ClassifyTest, DetectRecognizesStrippedStaticMinGW) {
#if defined(REVENANT_TEST_C_GCC_O2) && defined(REVENANT_TEST_C_O2)
  auto Gcc = Session::open(REVENANT_TEST_C_GCC_O2);
  ASSERT_TRUE(!!Gcc) << "cannot open " REVENANT_TEST_C_GCC_O2;
  EXPECT_EQ((*Gcc)->compiler().Producer, Compiler::MinGwGcc)
      << "evidence: "
      << ((*Gcc)->compiler().Evidence.empty()
              ? std::string("<none>")
              : (*Gcc)->compiler().Evidence.front());
  auto Msvc = Session::open(REVENANT_TEST_C_O2);
  ASSERT_TRUE(!!Msvc) << "cannot open " REVENANT_TEST_C_O2;
  EXPECT_NE((*Msvc)->compiler().Producer, Compiler::MinGwGcc);
#ifdef REVENANT_TEST_JSON_CL_O2
  auto ClangCl = Session::open(REVENANT_TEST_JSON_CL_O2);
  ASSERT_TRUE(!!ClangCl) << "cannot open " REVENANT_TEST_JSON_CL_O2;
  EXPECT_NE((*ClangCl)->compiler().Producer, Compiler::MinGwGcc)
      << "clang-cl image must not score as MinGW";
#endif
#else
  GTEST_SKIP() << "REVENANT_TEST_C_GCC_O2/REVENANT_TEST_C_O2 not defined";
#endif
}

// clang-cl inlines a memset and jumps into the CRT helper for the rep-stos
// path, so a conditional branch (not a call) reaches memset_repstos. The
// runtime library defines that symbol: a user marking makes the emission
// collide with it at link time.
TEST(ClassifyTest, BranchIntoRuntimeHelperStaysLibrary) {
#ifdef REVENANT_TEST_C_CLANG_O2AVX
  auto S = Session::open(REVENANT_TEST_C_CLANG_O2AVX);
  ASSERT_TRUE(!!S) << "cannot open " REVENANT_TEST_C_CLANG_O2AVX;
  auto C = (*S)->classifyFunctions(
      Lang::C, "C:/Users/User/RustroverProjects/rv-scratch/LunaLibMatch3/libdb.json");
  ASSERT_TRUE(!!C) << toString(C.takeError());
  ASSERT_TRUE(C->UserEntry);
  auto Repstos = (*S)->functionByName("memset_repstos");
  if (!Repstos)
    for (const auto &[At, Name] : C->LibraryNames)
      if (Name == "memset_repstos")
        Repstos = At;
  ASSERT_TRUE(Repstos) << "memset_repstos not found by name";
  EXPECT_TRUE(C->LibraryStarts.count(*Repstos))
      << "memset_repstos lost its library mark";
  EXPECT_FALSE(C->UserReachable.count(*Repstos))
      << "memset_repstos marked user (branch target pulled it in)";
  EXPECT_EQ(C->LibraryNames.count(*Repstos), 1u)
      << "memset_repstos has no library name for the emitter";
#else
  GTEST_SKIP() << "REVENANT_TEST_C_CLANG_O2AVX not defined";
#endif
}

} // namespace
