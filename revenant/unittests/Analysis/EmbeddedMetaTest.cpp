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

// A panic call references the `Location` static itself. A constant that merely
// sits next to a record must not hand that record to the referencing function:
// with a neighbourhood scan, the string at 0x2000 gave its neighbour record at
// 0x2018 to the wrong function and the record's own reference kept no location.
TEST(EmbeddedMetaTest, LocationComesFromTheReferencedRecordOnly) {
  constexpr uint64_t Base = 0x140000000;
  constexpr uint64_t StringUser = Base + 0x1000;
  constexpr uint64_t RecordUser = Base + 0x1100;
  constexpr uint64_t File = Base + 0x2200;
  Image Img;
  Img.Base = Va(Base);
  Section Text;
  Text.Name = ".text";
  Text.SecVa = Va(Base + 0x1000);
  Text.Vsize = 0x200;
  Text.Data.resize(0x200);
  Text.Flags = SectionFlags::rx();
  Img.Sections.push_back(std::move(Text));

  Section RData;
  RData.Name = ".rdata";
  RData.SecVa = Va(Base + 0x2000);
  RData.Vsize = 0x400;
  RData.Data.resize(0x400);
  RData.Flags = SectionFlags::r();
  const std::string Source = "C:\\work\\src\\main.rs";
  const std::string Panic = "boom";
  std::copy(Panic.begin(), Panic.end(), RData.Data.begin());
  // The record sits exactly 24 bytes after the message, the offset a
  // neighbourhood scan would reach from the message reference.
  put64(RData.Data, 0x18, File);
  put64(RData.Data, 0x20, Source.size());
  put32(RData.Data, 0x28, 17);
  put32(RData.Data, 0x2C, 3);
  std::copy(Source.begin(), Source.end(), RData.Data.begin() + 0x200);
  Img.Sections.push_back(std::move(RData));

  PeFile PE;
  PE.Img = std::make_shared<Image>(std::move(Img));

  Program P;
  P.Img = PE.Img;
  FuncRecord StringFn;
  StringFn.Start = Va(StringUser);
  StringFn.End = Va(StringUser + 0x20);
  StringFn.Chunks.push_back({StringFn.Start, StringFn.End});
  FuncRecord RecordFn;
  RecordFn.Start = Va(RecordUser);
  RecordFn.End = Va(RecordUser + 0x20);
  RecordFn.Chunks.push_back({RecordFn.Start, RecordFn.End});
  P.Functions.emplace(StringFn.Start, std::move(StringFn));
  P.Functions.emplace(RecordFn.Start, std::move(RecordFn));
  P.Strings = std::make_shared<std::map<Va, std::string>>();
  P.Strings->emplace(Va(Base + 0x2000), Panic);
  // Target 0x2000 is the message (referenced by the first function), target
  // 0x2018 is the record itself (referenced by the second function).
  P.Xr.Base = Va(Base);
  P.Xr.Targets = {0x2000, 0x2018};
  P.Xr.Offsets = {0, 1, 2};
  P.Xr.Froms = {0x1004, 0x1104};
  P.Xr.Kinds = {XrefKind::Read, XrefKind::Read};
  P.Names = std::make_shared<std::map<Va, std::string>>();

  applyEmbeddedMetadata(P, PE);

  auto It = P.Embedded.find(Va(StringUser));
  if (It != P.Embedded.end())
    EXPECT_TRUE(It->second.SourceFile.empty());
  const EmbeddedFunctionMetadata &Meta = P.Embedded.at(Va(RecordUser));
  EXPECT_EQ(Meta.SourceFile, Source);
  EXPECT_EQ(Meta.Line, 17u);
  EXPECT_EQ(Meta.Column, 3u);
}

} // namespace
