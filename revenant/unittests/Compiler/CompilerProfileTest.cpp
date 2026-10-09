#include "revenant/Compiler/Profile.h"

#include "gtest/gtest.h"

using namespace revenant;

namespace {
PeFile emptyPe() {
  PeFile PE;
  PE.Img = std::make_shared<Image>();
  return PE;
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

TEST(CompilerProfileTest, RustRichHeaderSelectsRustProfile) {
  PeFile PE = emptyPe();
  PE.RichHeader.push_back({0x93, 0, 1});
  std::unique_ptr<CompilerProfile> Profile = profileFor(PE);
  EXPECT_EQ(Profile->producer(), CompilerProducer::Rustc);
  EXPECT_EQ(Profile->ehModel(), EhModel::RustCleanup);
}

TEST(CompilerProfileTest, GccColdSuffixIsRecognized) {
  PeFile PE = emptyPe();
  PE.Img->Sections.push_back({".eh_frame", {}, 1, {}, SectionFlags::r()});
  std::unique_ptr<CompilerProfile> Profile = profileFor(PE);
  EXPECT_EQ(Profile->producer(), CompilerProducer::Gcc);
  EXPECT_TRUE(Profile->isColdChunkName("parse.cold"));
}
