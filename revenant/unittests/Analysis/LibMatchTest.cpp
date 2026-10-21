//==- unittests/Analysis/LibMatchTest.cpp - libdb loader edge cases ------==//
//
// The raw-scanner loader must accept and reject exactly what the old
// llvm::json-based loader did. Each test feeds a minimal database through a
// temporary file (load() is the only public entry) and covers one divergence
// that the scanner had against the json path.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/LibMatch.h"

#include "gtest/gtest.h"

#include <string>

#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace revenant;

namespace {

/// Writes `Text` to a fresh temp file and loads it as a database.
Expected<LibraryDatabase> loadText(StringRef Text) {
  SmallString<64> Path;
  int FD = -1;
  if (std::error_code EC =
          sys::fs::createTemporaryFile("libmatch", "json", FD, Path))
    return createStringError(EC, "cannot create libdb temp file");
  {
    raw_fd_ostream OS(FD, true);
    OS << Text;
  }
  auto DB = LibraryDatabase::load(Path);
  sys::fs::remove(Path);
  return DB;
}

/// Error text, or empty when the database loaded.
std::string loadErr(StringRef Text) {
  auto DB = loadText(Text);
  if (DB)
    return {};
  return toString(DB.takeError());
}

/// ASSERT_TRUE with the load error as the failure message.
#define ASSERT_LOADED(DB) \
  do { \
    if (!DB) { \
      std::string LoadErr_ = toString(DB.takeError()); \
      FAIL() << LoadErr_; \
    } \
  } while (0)

/// The smallest entry the loader accepts; `Bytes` must have `Size`/2 hex
/// digits.
std::string minimalEntry() {
  return R"({"name":"f","mangled_name":"?f@@YAXXZ","archive":"a.lib",)"
         R"("member":"f.obj","size":1,"bytes":"90","masked_relocations":[],)"
         R"("callees":["g"]})";
}

std::string dbOf(StringRef Entries) {
  return std::string("{\"version\":1,\"functions\":[") + Entries.str() + "]}";
}

/// `minimalEntry` with every `XX` byte in the name replaced by `JsonName`,
/// which is substituted verbatim (escapes allowed).
std::string entryWithName(StringRef JsonName) {
  std::string E = minimalEntry();
  size_t Pos = E.find("\"f\"");
  E.replace(Pos, 3, JsonName);
  return E;
}

std::string entryWithSize(StringRef SizeJson, StringRef BytesHex) {
  std::string E = minimalEntry();
  size_t Pos = E.find("\"size\":1");
  E.replace(Pos, 8, (Twine("\"size\":") + SizeJson).str());
  Pos = E.find("\"90\"");
  E.replace(Pos, 4, (Twine("\"") + BytesHex + "\"").str());
  return E;
}

std::string entryWithMask(StringRef MaskJson) {
  std::string E = minimalEntry();
  size_t Pos = E.find("[]");
  E.replace(Pos, 2, MaskJson);
  return E;
}

std::string entryWithCallees(StringRef CalleesJson) {
  std::string E = minimalEntry();
  size_t Pos = E.find("[\"g\"]");
  E.replace(Pos, 5, CalleesJson);
  return E;
}

std::string entryWithExtra(StringRef ExtraMember) {
  std::string E = minimalEntry();
  E.insert(E.size() - 1, (Twine(",") + ExtraMember).str());
  return E;
}

/// Full prototype for `minimalEntry`, with `Field:Value` injected verbatim.
std::string entryWithProto(StringRef ProtoJson) {
  return entryWithExtra(ProtoJson);
}

std::string validProto() {
  return R"("prototype":{"return_type":"void","return_bits":0,)"
         R"("return_float":false,"return_void":true,"varargs":false,)"
         R"("params":[{"name":"x","type":"int","bits":32,"float":false}]})";
}

} // namespace

// The scanner consumed one character too many after every \uXXXX escape.
TEST(LibMatchTest, UnicodeEscapeKeepsFollowingCharacters) {
  auto DB = loadText(dbOf(entryWithName("\"a\\u0001b\"")));
  ASSERT_LOADED(DB);
  ASSERT_EQ(DB->Functions.size(), 1u);
  EXPECT_EQ(DB->Functions[0].Name, std::string("a\x01" "b", 3));
}

// A \u escape at the end of a string used to swallow the closing quote and
// poison the rest of the document.
TEST(LibMatchTest, UnicodeEscapeAtStringEndLoads) {
  auto DB = loadText(dbOf(entryWithName("\"a\\u0001\"")));
  ASSERT_LOADED(DB);
  EXPECT_EQ(DB->Functions[0].Name, std::string("a\x01", 2));
}

// Surrogate pairs still combine.
TEST(LibMatchTest, SurrogatePairKeepsFollowingCharacters) {
  auto DB = loadText(dbOf(entryWithName("\"\\ud83d\\ude00x\"")));
  ASSERT_LOADED(DB);
  EXPECT_EQ(DB->Functions[0].Name, std::string("\xf0\x9f\x98\x80x", 5));
}

// llvm::json maps unpaired surrogates to U+FFFD instead of failing.
TEST(LibMatchTest, LoneLeadingSurrogateBecomesReplacementChar) {
  auto DB = loadText(dbOf(entryWithName("\"\\ud800x\"")));
  ASSERT_LOADED(DB);
  EXPECT_EQ(DB->Functions[0].Name, std::string("\xef\xbf\xbdx", 4));
}

TEST(LibMatchTest, LoneTrailingSurrogateBecomesReplacementChar) {
  auto DB = loadText(dbOf(entryWithName("\"\\udc00\"")));
  ASSERT_LOADED(DB);
  EXPECT_EQ(DB->Functions[0].Name, std::string("\xef\xbf\xbd", 3));
}

// A leading surrogate followed by a non-pair \u yields U+FFFD plus the
// second escape's own code point.
TEST(LibMatchTest, LeadSurrogateThenNonPairBecomesReplacementPlusChar) {
  auto DB = loadText(dbOf(entryWithName("\"\\ud800\\u0041\"")));
  ASSERT_LOADED(DB);
  EXPECT_EQ(DB->Functions[0].Name, std::string("\xef\xbf\xbd" "A", 4));
}

// llvm::json rejects a comma before the closing brace; the scanner kept
// looping and accepted it.
TEST(LibMatchTest, EntryTrailingCommaRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithExtra("\"size\":1,"))), "");
}

TEST(LibMatchTest, PrototypeTrailingCommaRejected) {
  std::string Proto = validProto();
  Proto.insert(Proto.size() - 1, ",");
  EXPECT_NE(loadErr(dbOf(entryWithProto(Proto))), "");
}

TEST(LibMatchTest, RootTrailingCommaRejected) {
  std::string Text = "{\"version\":1,\"functions\":[" + minimalEntry() + "],}";
  EXPECT_NE(loadErr(Text), "");
}

// Text after the root object was silently ignored.
TEST(LibMatchTest, TextAfterRootRejected) {
  EXPECT_NE(loadErr(dbOf(minimalEntry()) + " blah"), "");
  EXPECT_EQ(loadErr(dbOf(minimalEntry()) + "\t \n"), "")
      << "trailing whitespace must still load";
}

// Unknown keys must accept any JSON value, not just integers.
TEST(LibMatchTest, UnknownKeyWithNonIntegerValuesSkips) {
  EXPECT_EQ(loadErr(dbOf(entryWithExtra("\"extra\":1.5"))), "") << "float";
  EXPECT_EQ(loadErr(dbOf(entryWithExtra("\"extra\":1e3"))), "")
      << "exponent";
  EXPECT_EQ(loadErr(dbOf(entryWithExtra("\"extra\":{\"a\":[1.5,true]}"))), "")
      << "nested container";
  EXPECT_EQ(loadErr(dbOf(entryWithExtra("\"extra\":null"))), "") << "null";
}

// Out-of-range integers must not wrap around to a small number.
TEST(LibMatchTest, OverflowingSizeRejected) {
  // 2^64 wrapped to 0 in the scanner.
  EXPECT_NE(loadErr(dbOf(entryWithSize("18446744073709551616", "90"))), "");
  // 2^63 is a uint64 literal: getAsInteger fails, old code rejected as a
  // missing field.
  EXPECT_NE(loadErr(dbOf(entryWithSize("9223372036854775808", "90"))), "");
}

TEST(LibMatchTest, OverflowingMaskOffsetRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithMask("[18446744073709551616]"))), "");
}

// INT64_MIN parsed through UB in the old accumulate-then-negate code; it is
// a valid literal and must hit the bounds check instead.
TEST(LibMatchTest, Int64MinMaskOffsetRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithMask("[-9223372036854775808]"))), "");
}

TEST(LibMatchTest, NegativeMaskOffsetRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithMask("[-1]"))), "");
}

// The old getUInt required a non-negative integer for prototype bit fields.
TEST(LibMatchTest, NegativeReturnBitsRejected) {
  std::string Proto = validProto();
  size_t Pos = Proto.find("\"return_bits\":0");
  Proto.replace(Pos, 15, "\"return_bits\":-1");
  EXPECT_NE(loadErr(dbOf(entryWithProto(Proto))), "");
}

TEST(LibMatchTest, NegativeParamBitsRejected) {
  std::string Proto = validProto();
  size_t Pos = Proto.find("\"bits\":32");
  Proto.replace(Pos, 9, "\"bits\":-1");
  EXPECT_NE(loadErr(dbOf(entryWithProto(Proto))), "");
}

// getObject("prototype") ignores non-object values.
TEST(LibMatchTest, NonObjectPrototypeIgnored) {
  for (StringRef V : {"null", "5", "\"x\"", "[]", "true"}) {
    auto DB = loadText(dbOf(entryWithProto(
        (Twine("\"prototype\":") + V).str())));
    ASSERT_LOADED(DB);
    EXPECT_FALSE(DB->Functions[0].Prototype.has_value()) << V;
  }
}

// The old parser validated the document encoding before parsing.
TEST(LibMatchTest, InvalidUtf8Rejected) {
  EXPECT_NE(loadErr(dbOf(entryWithName("\"\xff\""))), "");
}

// A string is only a string when it is one; wrong-typed fields count as
// missing, like the old getString.
TEST(LibMatchTest, NonStringRequiredFieldRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithName("5"))), "");
}

// Mask elements must be integers; other JSON values are the old bounds
// error, not a parse failure.
TEST(LibMatchTest, NonIntegerMaskElementRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithMask("[1.5]"))), "");
  EXPECT_NE(loadErr(dbOf(entryWithMask("[\"x\"]"))), "");
}

TEST(LibMatchTest, NonStringCalleeRejected) {
  EXPECT_NE(loadErr(dbOf(entryWithCallees("[5]"))), "");
}

// A full valid prototype round-trips.
TEST(LibMatchTest, ValidPrototypeLoads) {
  auto DB = loadText(dbOf(entryWithProto(validProto())));
  ASSERT_LOADED(DB);
  const LibraryFunction &F = DB->Functions[0];
  ASSERT_TRUE(F.Prototype.has_value());
  EXPECT_EQ(F.Prototype->ReturnType, "void");
  EXPECT_EQ(F.Prototype->ReturnBits, 0u);
  EXPECT_TRUE(F.Prototype->ReturnVoid);
  ASSERT_EQ(F.Prototype->Params.size(), 1u);
  EXPECT_EQ(F.Prototype->Params[0].Bits, 32u);
}

// F.Archive stayed populated (it was empty under the old json loader; the
// scanner filled it and the fix keeps that).
TEST(LibMatchTest, ArchiveFieldIsLoaded) {
  auto DB = loadText(dbOf(minimalEntry()));
  ASSERT_LOADED(DB);
  EXPECT_EQ(DB->Functions[0].Archive, "a.lib");
  EXPECT_EQ(DB->Functions[0].Member, "f.obj");
}

// Parse failures name the parser's message and position, not a generic
// "malformed JSON".
TEST(LibMatchTest, ErrorCarriesParserMessage) {
  std::string Msg = loadErr(dbOf(minimalEntry()) + " blah");
  EXPECT_NE(Msg.find("malformed JSON at byte"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("text after end of document"), std::string::npos) << Msg;
}

