#include "revenant/Analysis/EmbeddedMeta.h"

#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <utility>
using namespace revenant;

namespace {

void put64(std::vector<uint8_t> &Bytes, size_t Offset, uint64_t Value) {
  llvm::support::endian::write64le(Bytes.data() + Offset, Value);
}

void put32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

TEST(EmbeddedMetaTest, AssociatesRustLocationAndPanicMessageWithFunction) {
  constexpr uint64_t Base = 0x140000000;
  constexpr uint64_t Function = Base + 0x1000;
  constexpr uint64_t File = Base + 0x2100;
  constexpr uint64_t Message = Base + 0x2200;
  Image Img;
  Img.Base = Va(Base);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(Base + 0x1000);
  Text.Vsize = 0x100;
  Text.Data.resize(0x100);
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));

  Section RData;
  RData.Name = ".rdata";
  RData.SecVa = Va(Base + 0x2000);
  RData.Vsize = 0x400;
  RData.Data.resize(0x400);
  RData.Flags = SectionFlags::r();
  const std::string Source = "C:\\work\\src\\main.rs";
  const std::string Panic = "panicked at 'failure', src/main.rs:17:3";
  put64(RData.Data, 0, File);
  put64(RData.Data, 8, Source.size());
  put32(RData.Data, 16, 17);
  put32(RData.Data, 20, 3);
  std::copy(Source.begin(), Source.end(), RData.Data.begin() + 0x100);
  std::copy(Panic.begin(), Panic.end(), RData.Data.begin() + 0x200);
  Img.Sections.push_back(std::move(RData));

  PeFile PE;
  PE.Img = std::make_shared<Image>(std::move(Img));
  PE.CoffSymbols.push_back({Va(Function), "retained_symbol", true});

  Program P;
  P.Img = PE.Img;
  FuncRecord F;
  F.Start = Va(Function);
  F.End = Va(Function + 0x20);
  F.Chunks.push_back({F.Start, F.End});
  P.Functions.emplace(F.Start, std::move(F));
  P.Strings = std::make_shared<std::map<Va, std::string>>();
  P.Strings->emplace(Va(Message), Panic);
  P.Xr.Base = Va(Base);
  P.Xr.Targets = {0x2000, 0x2200};
  P.Xr.Offsets = {0, 1, 2};
  P.Xr.Froms = {0x1004, 0x1008};
  P.Xr.Kinds = {XrefKind::Read, XrefKind::Read};
  P.Names = std::make_shared<std::map<Va, std::string>>();


  applyEmbeddedMetadata(P, PE);

  const EmbeddedFunctionMetadata &Meta = P.Embedded.at(Va(Function));
  EXPECT_EQ(Meta.SourceFile, Source);
  EXPECT_EQ(Meta.Line, 17u);
  EXPECT_EQ(Meta.Column, 3u);
  EXPECT_EQ(P.Functions.at(Va(Function)).Name, "retained_symbol");
  ASSERT_EQ(Meta.PanicMessages.size(), 1u);
  EXPECT_EQ(Meta.PanicMessages.front(), Panic);
}

} // namespace
