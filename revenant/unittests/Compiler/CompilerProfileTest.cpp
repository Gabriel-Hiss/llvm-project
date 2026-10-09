#include "revenant/Compiler/Profile.h"

#include "gtest/gtest.h"

using namespace revenant;

namespace {
PeFile emptyPe() {
  PeFile PE;
  PE.Img = std::make_shared<Image>();
  return PE;
}

void addSection(PeFile &PE, const char *Name) {
  PE.Img->Sections.push_back({Name, {}, 1, {}, SectionFlags::r()});
}
} // namespace

TEST(CompilerProfileTest, UnknownUsesGenericLlvmProfile) {
  PeFile PE = emptyPe();
  CompilerInfo Info = detectCompiler(PE);
  EXPECT_EQ(Info.Producer, CompilerProducer::Unknown);
  EXPECT_EQ(Info.Language, SourceLanguage::Unknown);
  std::unique_ptr<CompilerProfile> Profile = profileFor(PE);
  EXPECT_EQ(Profile->producer(), CompilerProducer::Unknown);
  EXPECT_EQ(Profile->traits().Optimization, OptimizationLevel::Unknown);
}

TEST(CompilerProfileTest, RichHeaderDetectsLtcgAndPgo) {
  PeFile PE = emptyPe();
  PE.RichHeader.push_back({0x108, 0, 1});
  PE.RichHeader.push_back({0x10b, 0, 1});
  CompilerInfo Info = detectCompiler(PE);
  EXPECT_EQ(Info.Producer, CompilerProducer::Msvc);
  EXPECT_EQ(Info.Traits.Lto, std::optional<bool>(true));
  EXPECT_EQ(Info.Traits.Pgo, std::optional<bool>(true));
}

// Corpus-validated ids: 0x109 is the /GL link marker, 0x10d the LTCG+PGO
// link marker.
TEST(CompilerProfileTest, CorpusLinkIdsDetectLtcgAndPgo) {
  PeFile Lt = emptyPe();
  Lt.RichHeader.push_back({0x101, 0, 1});
  Lt.RichHeader.push_back({0x109, 0, 1});
  CompilerInfo LtInfo = detectCompiler(Lt);
  EXPECT_EQ(LtInfo.Producer, CompilerProducer::Msvc);
  EXPECT_EQ(LtInfo.Traits.Lto, std::optional<bool>(true));
  EXPECT_EQ(LtInfo.Traits.Pgo, std::optional<bool>());

  PeFile Pg = emptyPe();
  Pg.RichHeader.push_back({0x101, 0, 1});
  Pg.RichHeader.push_back({0x10d, 0, 1});
  CompilerInfo PgInfo = detectCompiler(Pg);
  EXPECT_EQ(PgInfo.Traits.Lto, std::optional<bool>(true));
  EXPECT_EQ(PgInfo.Traits.Pgo, std::optional<bool>(true));
  EXPECT_EQ(PgInfo.Traits.Optimization, OptimizationLevel::O2);
}

TEST(CompilerProfileTest, RustRichHeaderSelectsRustProfile) {
  PeFile PE = emptyPe();
  PE.RichHeader.push_back({0x93, 0, 1});
  std::unique_ptr<CompilerProfile> Profile = profileFor(PE);
  EXPECT_EQ(Profile->producer(), CompilerProducer::Rustc);
  EXPECT_EQ(Profile->ehModel(), EhModel::RustCleanup);
}

TEST(CompilerProfileTest, GccColdSuffixIsRecognized) {
  PeFile PE = emptyPe();
  addSection(PE, ".eh_frame");
  std::unique_ptr<CompilerProfile> Profile = profileFor(PE);
  EXPECT_EQ(Profile->producer(), CompilerProducer::Gcc);
  EXPECT_TRUE(Profile->isColdChunkName("parse.cold"));
}

TEST(CompilerProfileTest, ClangFoldedBuiltinsSelectClang) {
  PeFile PE = emptyPe();
  addSection(PE, "memcpy_");
  addSection(PE, "memset_");
  CompilerInfo Info = detectCompiler(PE);
  EXPECT_EQ(Info.Producer, CompilerProducer::Clang);
}

TEST(CompilerProfileTest, MsvcFptableSelectsMsvc) {
  PeFile PE = emptyPe();
  PE.RichHeader.push_back({0x104, 0, 1});
  addSection(PE, ".fptable");
  CompilerInfo Info = detectCompiler(PE);
  EXPECT_EQ(Info.Producer, CompilerProducer::Msvc);
}

TEST(CompilerProfileTest, GuardTablesDetectCfg) {
  PeFile PE = emptyPe();
  PE.Img->Sections.push_back(
      {".rdata", {}, 1, {'x', '.', 'g', 'f', 'i', 'd', 's'}, SectionFlags::r()});
  CompilerInfo Info = detectCompiler(PE);
  EXPECT_EQ(Info.Traits.ControlFlowGuard, std::optional<bool>(true));
}

TEST(CompilerProfileTest, LlvmPgoSectionsDetectPgo) {
  PeFile PE = emptyPe();
  addSection(PE, ".lprfn");
  CompilerInfo Info = detectCompiler(PE);
  EXPECT_EQ(Info.Traits.Pgo, std::optional<bool>(true));
}
