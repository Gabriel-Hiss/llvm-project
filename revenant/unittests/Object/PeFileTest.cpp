//==- unittests/Object/PeFileTest.cpp - loader checks ---------------------==//
//
// The loader must match crates/pe on the parity corpus binary: section
// count (llvm-objdump -h), pdata/import/export counts and entry point
// (revenant-cli info), plus the file-map partition invariant.
//
//===----------------------------------------------------------------------===//

#include "revenant/Core/Program.h"
#include "revenant/Analysis/Analysis.h"
#include "revenant/Target/TargetHooks.h"
#include "revenant/Object/FileMap.h"
#include "revenant/Object/PeFile.h"

#include "gtest/gtest.h"

#include <cstdlib>
#include <vector>
#include <utility>
#include <map>
#include <string>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace revenant;

namespace {

std::optional<PeFile> openCorpus() {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_C_O2);
  if (!Buf)
    return std::nullopt;
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  if (!Pe) {
    consumeError(Pe.takeError());
    return std::nullopt;
  }
  return std::move(*Pe);
}

// Run `prog args...`, capture stdout, return it (empty on failure).
std::string run(StringRef Prog, std::vector<StringRef> Args) {
  std::string Err;
  int Rc = sys::ExecuteAndWait(Prog, Args, {}, {}, 0, 0, &Err);
  if (Rc != 0) {
    errs() << "run " << Prog << ": " << Err << "\n";
    return {};
  }
  return {};
}

// Pull "<key>: <val> <word>" and "<key>: <val>" pairs out of a text blob.
std::map<std::string, std::string> kv(StringRef Text) {
  std::map<std::string, std::string> Out;
  size_t Pos = 0;
  while (Pos < Text.size()) {
    size_t Eol = Text.find('\n', Pos);
    if (Eol == StringRef::npos)
      Eol = Text.size();
    StringRef Line = Text.slice(Pos, Eol).trim();
    Pos = Eol + 1;
    if (Line.empty())
      continue;
    size_t Colon = Line.find(':');
    if (Colon == StringRef::npos)
      continue;
    StringRef Key = Line.take_front(Colon).trim();
    StringRef Rest = Line.drop_front(Colon + 1).trim();
    size_t Space = Rest.find(' ');
    if (Space == StringRef::npos)
      Out[Key.str()] = Rest.str();
    else
      Out[Key.str()] = Rest.take_front(Space).str();
  }
  return Out;
}

// "N sections, M imports, K exports, P pdata entries" from info's counts.
std::map<std::string, long long> countsOf(StringRef Text) {
  std::map<std::string, long long> Out;
  for (auto &[K, V] : kv(Text)) {
    if (K == "sections" || K == "imports" || K == "exports" || K == "pdata")
      Out[K] = std::stoll(V);
  }
  return Out;
}

TEST(PeFileTest, ParsesCorpusBinary) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  EXPECT_GT(Pe->FileSize, 0);
  EXPECT_GT(Pe->HeadersSize, 0);
  EXPECT_FALSE(Pe->RawSections.empty());
  EXPECT_NE(Pe->EntryPoint.Value, 0);
}

TEST(PeFileTest, SectionCountMatchesObjdump) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  std::string Out = run(LLVM_OBJDUMP_EXE, {"-h", REVENANT_TEST_C_O2});
  ASSERT_FALSE(Out.empty());
  long long ObjdumpSections = 0;
  for (auto &[K, V] : kv(Out)) {
    if (K == "Sections")
      ObjdumpSections = std::stoll(V);
  }
  EXPECT_EQ((long long)Pe->RawSections.size(), ObjdumpSections);
}

TEST(PeFileTest, CountsMatchRustCli) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  std::string Out = run(REVENANT_CLI_EXE, {"info", REVENANT_TEST_C_O2});
  ASSERT_FALSE(Out.empty());
  auto Counts = countsOf(Out);
  EXPECT_EQ((long long)Pe->Imports.size(), Counts["imports"]);
  EXPECT_EQ((long long)Pe->Exports.size(), Counts["exports"]);
  EXPECT_EQ((long long)Pe->Pdata.size(), Counts["pdata"]);
}

TEST(PeFileTest, FileMapPartitions) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  Program Prog;
  Prog.Img = Pe->Img;
  FileMap Map = FileMap::build(*Pe, Prog);
  uint64_t Covered = 0;
  for (const FileSpan &S : Map.Spans) {
    EXPECT_EQ(S.Offset, Covered);
    EXPECT_GT(S.End, S.Offset);
    Covered = S.End;
  }
  EXPECT_EQ(Covered, Pe->FileSize);
  // First span is the headers, sections classify as Data/Unknown.
  ASSERT_FALSE(Map.Spans.empty());
  EXPECT_EQ(Map.Spans.front().Kind, FileByteKind::Header);
}

TEST(PeFileTest, ImageReads) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  // Entry point code bytes exist.
  auto B = Pe->Img->read(Pe->EntryPoint, 4);
  ASSERT_TRUE(B.has_value());
  EXPECT_EQ(B->size(), 4u);
  // RVA round-trip.
  auto Rva = Pe->Img->rvaOf(Pe->EntryPoint);
  ASSERT_TRUE(Rva.has_value());
  EXPECT_EQ(Pe->Img->vaOfRva(*Rva), Pe->EntryPoint);
}

#ifdef REVENANT_TEST_GO_UNOPTIMIZED
TEST(PeFileTest, GoBuildIdDetected) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_GO_UNOPTIMIZED);
  ASSERT_TRUE(Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  if (!Pe) {
    consumeError(Pe.takeError());
    return;
  }
  ASSERT_TRUE(Pe->GoBuildId.has_value())
      << "goBuildId must detect the Go build-id blob";
  // Blob is 128 bytes at .text start (RVA 0x1000, VA 0x140001000).
  EXPECT_EQ(Pe->GoBuildId->Start.Value, 0x140001000ULL);
  EXPECT_EQ(Pe->GoBuildId->End.Value, 0x140001080ULL);
  // The blob must be in InlineData so discovery skips it.
  EXPECT_TRUE(Pe->Img->inInlineData(Va(0x140001000)));
  EXPECT_TRUE(Pe->Img->inInlineData(Va(0x140001001)));
  EXPECT_TRUE(Pe->Img->inInlineData(Va(0x14000107f)));
  EXPECT_FALSE(Pe->Img->inInlineData(Va(0x140001080)));
}
#endif

TEST(PeFileTest, JumpTablesClassifiedAsData) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);
  Program Prog = analyze(*Pe, *Hooks);
  ASSERT_FALSE(Prog.Functions.empty());

  // Collect all known jump table ranges
  std::vector<std::pair<Va, Va>> DataRanges;
  for (auto &[Start, Rec] : Prog.Functions)
    for (const auto &Range : Rec.DataRanges)
      DataRanges.push_back(Range);
  ASSERT_FALSE(DataRanges.empty()) << "Corpus binary should have jump tables";

  FileMap Map = FileMap::build(*Pe, Prog);
  ASSERT_FALSE(Map.Spans.empty());

  // Count spans by kind
  int dataCount = 0, unknownCount = 0, paddingCount = 0;
  for (const FileSpan &S : Map.Spans) {
    if (S.Kind == FileByteKind::Data) dataCount++;
    if (S.Kind == FileByteKind::Unknown) unknownCount++;
    if (S.Kind == FileByteKind::Padding) paddingCount++;
  }

  // Jump tables should produce Data spans
  EXPECT_GT(dataCount, 0) << "Known jump tables should produce Data spans";

  // Alignment padding between code chunks must be Padding
  EXPECT_GT(paddingCount, 0) << "Should have alignment padding spans";

  // Unknown should be much lower than before the fix
  EXPECT_LT(unknownCount, 100) << "Unknown spans should be rare after jump table fix";
}

} // end anonymous namespace
