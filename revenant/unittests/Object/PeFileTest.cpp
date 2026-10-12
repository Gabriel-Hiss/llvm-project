//==- unittests/Object/PeFileTest.cpp - loader checks ---------------------==//
//
// The loader must match crates/pe on the parity corpus binary: section
// count (llvm-objdump -h), pdata/import/export counts and entry point
// (revenant-cli info), plus the file-map partition invariant.
//
//===----------------------------------------------------------------------===//

#include "revenant/Core/Program.h"
#include "revenant/Object/FileMap.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Analysis/Analysis.h"
#include "revenant/Target/TargetHooks.h"

#include "gtest/gtest.h"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <utility>

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
  SmallString<128> OutPath;
  if (sys::fs::createTemporaryFile("rv-test", "out", OutPath))
    return "";
  SmallVector<StringRef, 4> Argv;
  Argv.push_back(Prog);
  for (StringRef A : Args)
    Argv.push_back(A);
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(OutPath),
                                          std::nullopt};
  int Rc = sys::ExecuteAndWait(Prog, Argv, std::nullopt, Redirects);
  if (Rc != 0)
    return "";
  auto Buf = MemoryBuffer::getFile(OutPath);
  sys::fs::remove(OutPath);
  if (!Buf)
    return "";
  return (*Buf)->getBuffer().str();
}

// Pull "<key>: <val> <word>" and "<key>: <val>" pairs out of a text blob.
std::map<std::string, std::string> kv(StringRef Text) {
  std::map<std::string, std::string> Out;
  SmallVector<StringRef, 32> Lines;
  Text.split(Lines, '\n');
  for (StringRef L : Lines) {
    auto P = L.find(':');
    if (P == StringRef::npos)
      continue;
    Out[L.substr(0, P).trim().str()] = L.substr(P + 1).trim().str();
  }
  return Out;
}

// "N sections, M imports, K exports, P pdata entries" from info's counts.
std::map<std::string, long long> countsOf(StringRef Text) {
  std::map<std::string, long long> Out;
  SmallVector<StringRef, 32> Lines;
  Text.split(Lines, '\n');
  for (StringRef L : Lines) {
    L = L.trim();
    if (!L.starts_with("counts:"))
      continue;
    L = L.drop_front(StringRef("counts:").size()).trim();
    SmallVector<StringRef, 8> Parts;
    L.split(Parts, ',');
    for (StringRef P : Parts) {
      P = P.trim();
      auto [Number, Name] = P.split(' ');
      long long N;
      if (!Number.getAsInteger(10, N))
        Out[Name.trim().split(' ').first.str()] = N;
    }
  }
  return Out;
}

TEST(PeFileTest, ParsesCorpusBinary) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  EXPECT_GT(Pe->FileSize, 0u);
  EXPECT_NE(Pe->Img->Base.Value, 0u);
}

TEST(PeFileTest, SectionCountMatchesObjdump) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  std::string Dump = run(LLVM_OBJDUMP_EXE, {"-h", REVENANT_TEST_C_O2});
  ASSERT_FALSE(Dump.empty());
  size_t Sections = 0;
  SmallVector<StringRef, 32> Lines;
  StringRef(Dump).split(Lines, '\n');
  for (StringRef L : Lines) {
    L = L.trim();
    // Section lines start with an index then the name.
    uint64_t Idx;
    auto Sp = L.split(' ');
    if (Sp.first.getAsInteger(10, Idx) == 0 && !Sp.second.empty())
      ++Sections;
  }
  ASSERT_GT(Sections, 0u);
  EXPECT_EQ(Pe->Img->Sections.size(), Sections);
}

TEST(PeFileTest, CountsMatchRustCli) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  std::string Info =
      run(REVENANT_CLI_EXE, {"info", REVENANT_TEST_C_O2});
  ASSERT_FALSE(Info.empty()) << "revenant-cli info produced no output";
  auto Counts = countsOf(Info);
  ASSERT_TRUE(Counts.count("imports"));
  ASSERT_TRUE(Counts.count("pdata"));
  EXPECT_EQ(Pe->Imports.size(), (size_t)Counts["imports"]);
  EXPECT_EQ(Pe->Exports.size(), (size_t)Counts["exports"]);
  EXPECT_EQ(Pe->Pdata.size(), (size_t)Counts["pdata"]);

  // Entry point also shown by `info`.
  for (auto &[K, V] : kv(Info))
    if (K == "entry") {
      uint64_t Entry = 0;
      StringRef EntryText(V);
      if (EntryText.starts_with("0x"))
        EntryText = EntryText.drop_front(2);
      ASSERT_FALSE(EntryText.getAsInteger(16, Entry));
      EXPECT_EQ(Pe->EntryPoint.Value, Entry);
    }
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

// audio-nodegraph-cl-o1.dll: .text raw slack [0xa37f0, 0xa3800) is 16
// zero bytes past VirtualSize — unmapped linker fill, not Unknown.
TEST(PeFileTest, ExecSlackZerosAreOverlay) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_AUDIO_CL_O1);
  ASSERT_TRUE(Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE(!!Pe);
  Program Prog;
  Prog.Img = Pe->Img;
  FileMap Map = FileMap::build(*Pe, Prog);
  const FileSpan *S = Map.spanAt(0xa37f0);
  ASSERT_TRUE(S);
  EXPECT_EQ(S->Offset, 0xa37f0u);
  EXPECT_EQ(S->End, 0xa3800u);
  EXPECT_EQ(S->Kind, FileByteKind::Overlay);
  EXPECT_FALSE(S->SpanVa.has_value());
}

// audio-nodegraph-clang-o3.dll: .text raw slack [0xa3e66, 0xa4000) is
// 410 0xcc bytes past VirtualSize — compiler fill, not Unknown.
TEST(PeFileTest, ExecSlackInt3FillIsPadding) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_AUDIO_CLANG_O3);
  ASSERT_TRUE(Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE(!!Pe);
  Program Prog;
  Prog.Img = Pe->Img;
  FileMap Map = FileMap::build(*Pe, Prog);
  const FileSpan *S = Map.spanAt(0xa3e66);
  ASSERT_TRUE(S);
  EXPECT_EQ(S->Kind, FileByteKind::Padding);
  EXPECT_FALSE(S->SpanVa.has_value());
}

// c-o2.exe: the CRT __GSHandlerCheckCommon switch table at file offset
// 6160 is a 20-byte `dd` RVA table followed by a 36-byte case bitmap
// and 8 bytes of 0xcc — it sits inside a gap the padding runs carve
// out, and must classify as Data, not Unknown.
TEST(PeFileTest, BitmapTailJumpTableIsData) {
  auto Pe = openCorpus();
  ASSERT_TRUE(Pe.has_value());
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);
  Program Prog = analyze(*Pe, *Hooks);
  FileMap Map = FileMap::build(*Pe, Prog);
  const FileSpan *S = Map.spanAt(6160);
  ASSERT_TRUE(S);
  EXPECT_EQ(S->Offset, 6160u);
  EXPECT_EQ(S->End, 6224u);
  EXPECT_EQ(S->Kind, FileByteKind::Data);
}

// c-gcc-o2.exe: MinGW CRT record at file offset 12832 — 40 bytes of
// {~0, fn, 0, ~0, 0} holding one code pointer — is data, not Unknown.
TEST(PeFileTest, MingwCrtRecordIsData) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_C_GCC_O2);
  ASSERT_TRUE(Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE(!!Pe);
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);
  Program Prog = analyze(*Pe, *Hooks);
  FileMap Map = FileMap::build(*Pe, Prog);
  const FileSpan *S = Map.spanAt(12832);
  ASSERT_TRUE(S);
  EXPECT_EQ(S->Kind, FileByteKind::Data);
}

// go-unoptimized divisiveis.exe: the 128-byte Go build-id blob at
// .text start (file offset 0x600) is loader-recorded InlineData.
TEST(PeFileTest, GoBuildIdSpanIsData) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_GO_UNOPTIMIZED);
  ASSERT_TRUE(Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE(!!Pe);
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);
  Program Prog = analyze(*Pe, *Hooks);
  FileMap Map = FileMap::build(*Pe, Prog);
  const FileSpan *S = Map.spanAt(0x600);
  ASSERT_TRUE(S);
  EXPECT_EQ(S->Kind, FileByteKind::Data);
}

// json-cl-o2.exe: file offset 0x6c7c4 is a mid-table slice — a `dd`
// switch table whose run starts mid-gap, entries into .text with a
// bitmap in between. It must classify as Data.
TEST(PeFileTest, MidTableSliceIsData) {
  auto Buf = MemoryBuffer::getFile(REVENANT_TEST_JSON_CL_O2);
  ASSERT_TRUE(Buf);
  StringRef Data = (*Buf)->getBuffer();
  auto Pe = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  ASSERT_TRUE(!!Pe);
  auto Hooks = createX86TargetHooks();
  ASSERT_TRUE(Hooks);
  Program Prog = analyze(*Pe, *Hooks);
  FileMap Map = FileMap::build(*Pe, Prog);
  const FileSpan *S = Map.spanAt(0x6c7c4);
  ASSERT_TRUE(S);
  EXPECT_EQ(S->Kind, FileByteKind::Data);
}

} // end anonymous namespace
