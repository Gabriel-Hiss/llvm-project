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

#include "gtest/gtest.h"

#include <cstdlib>
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
    if (!L.trim().starts_with("counts:"))
      continue;
    SmallVector<StringRef, 8> Parts;
    L.split(Parts, ',');
    for (StringRef P : Parts) {
      SmallVector<StringRef, 4> Words;
      P.split(Words, ' ');
      if (Words.size() < 2)
        continue;
      long long N;
      if (!Words[0].getAsInteger(10, N))
        Out[Words[1].str()] = N;
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

} // end anonymous namespace
