//==- unittests/Analysis/AnalysisTest.cpp - discovery integration ------==//
//
// Exercise the public analysis entry point against the prebuilt C++ parity
// binary. The test never rebuilds or reads its oracle.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Analysis.h"
#include "revenant/Analysis/Padding.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include "gtest/gtest.h"

#include <memory>
#include <optional>
#include <random>

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

// LTCG can leave a leaf function reachable only through a tail jump into
// it: /GL specialized the caller and erased the function-pointer table the
// leaf was addressed through, and the leaf has no `.pdata` entry of its own.
// The builder must stop at such a target instead of absorbing the callee's
// bytes as blocks of the caller, otherwise the leaf is never discovered
// (c-o2gl: add_op/sub_op, whose addresses only survive in `call_ptr`'s
// specialized tail jump).
//
// Invariant over a real binary: every tail-jump target that opens a new
// function after inter-function int3 padding is a discovered start.
TEST(AnalysisTest, PaddingBoundedTailJumpTargetIsDiscovered) {
#ifdef REVENANT_TEST_C_O2GL
  auto Buffer = MemoryBuffer::getFile(REVENANT_TEST_C_O2GL);
  ASSERT_TRUE(Buffer);
  StringRef Bytes = (*Buffer)->getBuffer();
  auto Parsed = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()));
  if (!Parsed) {
    consumeError(Parsed.takeError());
    FAIL() << "The LTCG PE fixture could not be parsed";
    return;
  }
  PeFile Pe = std::move(*Parsed);
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);
  Program P = analyze(Pe, *Hooks);
  ASSERT_FALSE(P.Functions.empty());
  ASSERT_TRUE(Pe.Img);
  llvm::Expected<Disasm> DisOrErr = Disasm::create(*Pe.Img);
  ASSERT_TRUE(!!DisOrErr) << toString(DisOrErr.takeError());
  Disasm Dis = std::move(*DisOrErr);

  size_t Checked = 0;
  for (const auto &[Start, Rec] : P.Functions) {
    for (Rva R : Rec.InstrRvas) {
      auto I = Dis.decodeOne(Pe.Img->vaOfRva(R));
      if (!I || I->Flow != FlowKind::Branch || !I->HasTarget)
        continue;
      if (!startsAfterInt3(Dis, *Pe.Img, I->Target))
        continue;
      ++Checked;
      EXPECT_NE(P.Functions.find(I->Target), P.Functions.end())
          << "tail jump to the padding-bounded start "
          << format_hex(I->Target.Value, 0) << " was absorbed into "
          << format_hex(Start.Value, 0);
    }
  }
  EXPECT_GT(Checked, 0u) << "the fixture holds no padding-bounded tail jump";
#else
  GTEST_SKIP() << "REVENANT_TEST_C_O2GL not defined";
#endif
}

// ---- functionAt chunk index ------------------------------------------------
//
// The binary-search index must answer what the old linear scan over
// `P.Functions`/`Chunks` answered, at every boundary, after every mutation
// path, and under overlapping chunks.

namespace {

Function fnWithChunks(Va Start, std::vector<std::pair<Va, Va>> Chunks) {
  Function F;
  F.Start = Start;
  F.End = Start + 0x10;
  F.Chunks = std::move(Chunks);
  return F;
}

Program programWithImage() {
  Program P;
  P.Img = std::make_shared<Image>();
  return P;
}

} // namespace

TEST(AnalysisTest, FunctionAtMatchesChunkBoundaries) {
  Program P = programWithImage();
  insertFunction(P, fnWithChunks(Va(0x1000), {{Va(0x1000), Va(0x1100)},
                                            {Va(0x1200), Va(0x1300)}}));
  buildChunkIndex(P);
  EXPECT_EQ(functionAt(P, Va(0x0FFF)), nullptr);
  EXPECT_EQ(functionAt(P, Va(0x1000))->Start, Va(0x1000));
  EXPECT_EQ(functionAt(P, Va(0x10FF))->Start, Va(0x1000));
  // A half-open range does not contain its own end.
  EXPECT_EQ(functionAt(P, Va(0x1100)), nullptr);
  EXPECT_EQ(functionAt(P, Va(0x11FF)), nullptr);
  EXPECT_EQ(functionAt(P, Va(0x1200))->Start, Va(0x1000));
  EXPECT_EQ(functionAt(P, Va(0x1300)), nullptr);
  EXPECT_EQ(functionAt(programWithImage(), Va(0x1000)), nullptr);
}

TEST(AnalysisTest, FunctionAtSkipsEmptyChunks) {
  Program P = programWithImage();
  insertFunction(P, fnWithChunks(Va(0x1000), {{Va(0x1050), Va(0x1050)},
                                            {Va(0x1000), Va(0x1100)}}));
  EXPECT_EQ(functionAt(P, Va(0x1050))->Start, Va(0x1000));
}

// insertFunction must not leave a stale index.
TEST(AnalysisTest, FunctionAtSeesFunctionsInsertedAfterLookup) {
  Program P = programWithImage();
  insertFunction(P, fnWithChunks(Va(0x1000), {{Va(0x1000), Va(0x1100)}}));
  EXPECT_EQ(functionAt(P, Va(0x1050))->Start, Va(0x1000));
  EXPECT_EQ(functionAt(P, Va(0x2050)), nullptr);
  insertFunction(P, fnWithChunks(Va(0x2000), {{Va(0x2000), Va(0x2100)}}));
  EXPECT_EQ(functionAt(P, Va(0x2050))->Start, Va(0x2000));
}

// A copied Program must not reuse the source's index pointers.
TEST(AnalysisTest, FunctionAtOnCopiedProgramUsesOwnRecords) {
  Program P = programWithImage();
  insertFunction(P, fnWithChunks(Va(0x1000), {{Va(0x1000), Va(0x1100)}}));
  buildChunkIndex(P);
  Program Q = P;
  const FuncRecord *Hit = functionAt(Q, Va(0x1050));
  ASSERT_NE(Hit, nullptr);
  EXPECT_EQ(Hit, &Q.Functions.find(Va(0x1000))->second);
  // After the source is gone the copy still answers.
  {
    Program Q2 = P;
    P.Functions.clear();
    EXPECT_EQ(functionAt(Q2, Va(0x1050))->Start, Va(0x1000));
  }
}

// With overlapping chunks the old linear scan won the first match in
// function-map order, which is not the chunk with the largest lo.
TEST(AnalysisTest, FunctionAtOverlapMatchesLinearScan) {
  Program P = programWithImage();
  insertFunction(P, fnWithChunks(Va(0x1000), {{Va(0x1000), Va(0x1300)}}));
  insertFunction(P, fnWithChunks(Va(0x2000), {{Va(0x1200), Va(0x1210)}}));
  // 0x1205 sits inside both chunks; the first function in map order wins,
  // not the later one a binary search would pick.
  EXPECT_EQ(functionAt(P, Va(0x1205))->Start, Va(0x1000));
  EXPECT_EQ(functionAt(P, Va(0x1100))->Start, Va(0x1000));
  EXPECT_EQ(functionAt(P, Va(0x1250))->Start, Va(0x1000));
  EXPECT_EQ(functionAt(P, Va(0x1300)), nullptr);
}

// Identical ranges in different functions resolve to the earlier start.
TEST(AnalysisTest, FunctionAtIdenticalRangesPicksFirstFunction) {
  Program P = programWithImage();
  insertFunction(P, fnWithChunks(Va(0x2000), {{Va(0x1000), Va(0x1100)}}));
  insertFunction(P, fnWithChunks(Va(0x1000), {{Va(0x1000), Va(0x1100)}}));
  EXPECT_EQ(functionAt(P, Va(0x1050))->Start, Va(0x1000));
}

// Brute-force reference: the pre-index linear scan.
static const FuncRecord *scanAt(const Program &P, Va Address) {
  for (const auto &[Start, F] : P.Functions) {
    (void)Start;
    for (const auto &[A, B] : F.Chunks)
      if (Address >= A && Address < B)
        return &F;
  }
  return nullptr;
}

// The segment index must answer like the linear scan on arbitrary chunk
// layouts: disjoint, partially overlapping, nested and identical ranges.
TEST(AnalysisTest, FunctionAtMatchesScanOnRandomChunkSets) {
  std::mt19937_64 Rng(0xC0FFEE);
  for (int Iter = 0; Iter < 300; ++Iter) {
    Program P = programWithImage();
    int FnCount = 1 + static_cast<int>(Rng() % 12);
    for (int Fi = 0; Fi < FnCount; ++Fi) {
      Function F;
      F.Start = Va((Rng() % 64) * 0x100);
      F.End = F.Start + 0x2000;
      int ChunkCount = static_cast<int>(Rng() % 4);
      for (int Ci = 0; Ci < ChunkCount; ++Ci) {
        uint64_t Lo = Rng() % 0x2000;
        uint64_t Span;
        switch (Iter % 4) {
        case 0:
          Span = 1 + Rng() % 0x800; // partially overlapping
          break;
        case 1:
          Span = 0x400; // same-size chunks: nesting and identical ranges
          break;
        case 2:
          Span = 0x1000; // large spans covering most of the field
          break;
        default:
          Span = 1 + Rng() % 0x200;
          break;
        }
        F.Chunks.push_back({Va(Lo), Va(Lo + Span)});
      }
      // A function may also duplicate one of its own chunks.
      if (!F.Chunks.empty() && Rng() % 3 == 0)
        F.Chunks.push_back(F.Chunks.front());
      insertFunction(P, std::move(F));
    }
    std::vector<uint64_t> Queries;
    for (uint64_t A = 0; A < 0x2400; A += 0x40)
      Queries.push_back(A);
    for (const auto &[Start, F] : P.Functions) {
      (void)Start;
      for (const auto &[A, B] : F.Chunks) {
        Queries.push_back(A.Value);
        Queries.push_back(B.Value);
        if (B > A) {
          Queries.push_back(A.Value + 1);
          Queries.push_back(B.Value - 1);
        }
      }
    }
    for (uint64_t A : Queries) {
      const FuncRecord *Got = functionAt(P, Va(A));
      const FuncRecord *Want = scanAt(P, Va(A));
      ASSERT_EQ(Got, Want)
          << "iter " << Iter << " addr " << format_hex(A, 0);
    }
  }
}
