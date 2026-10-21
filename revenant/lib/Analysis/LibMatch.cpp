//==- lib/Analysis/LibMatch.cpp - Static-library function matching --------==//
//
// Loads the deterministic libdb format, matches whole discovered functions
// after masking source-object relocations, and applies names/prototypes.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/LibMatch.h"

#include "revenant/Analysis/Signatures.h"
#include "revenant/Core/Image.h"
#include "X86BaseInfo.h"
#include "llvm/Support/FileSystem.h"
#include "revenant/Core/Program.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;

namespace revenant {
namespace {

static Error invalidDB(const Twine &Why) {
  return createStringError(inconvertibleErrorCode(), "invalid libdb: %s",
                           Why.str().c_str());
}


static Expected<std::vector<uint8_t>> parseHexBytes(StringRef Hex) {
  if (Hex.size() % 2)
    return invalidDB("byte string has odd-length hex");
  std::vector<uint8_t> Bytes;
  Bytes.reserve(Hex.size() / 2);
  for (size_t I = 0; I < Hex.size(); I += 2) {
    unsigned V = 0;
    if (Hex.slice(I, I + 2).getAsInteger(16, V))
      return invalidDB("byte string contains non-hex characters");
    Bytes.push_back(static_cast<uint8_t>(V));
  }
  return Bytes;
}
// ---- Fast libdb JSON parser -----------------------------------------------
//
// The database is emitted by LibraryDatabase::save with llvm::json: a single
// `functions` array of objects with the keys "name", "mangled_name",
// "archive", "member", "size", "bytes", "masked_relocations", "callees" and
// an optional "prototype". Building the generic json value tree costs about
// 2 s for the 85 MB database; this parser walks the same schema straight into
// LibraryFunction records while preserving the grammar, conversion and error
// behavior of llvm/lib/Support/JSON.cpp: whitespace-terminated root, no
// trailing commas, strto* numbers, U+FFFD for unpaired surrogates, UTF-8
// validation for the whole document, and last-key-wins objects where a
// wrong-typed value counts as absent.
class JParser {
public:
  JParser(StringRef Text)
      : Start(Text.data()), P(Text.data()), E(Text.data() + Text.size()) {}

  void skipWs() {
    while (P < E && (*P == ' ' || *P == '\t' || *P == '\n' || *P == '\r'))
      ++P;
  }
  bool consume(char C) {
    skipWs();
    if (P < E && *P == C) {
      ++P;
      return true;
    }
    return false;
  }
  bool peek(char C) {
    skipWs();
    return P < E && *P == C;
  }
  const char *pos() const { return P; }
  const char *end() const { return E; }
  /// The recorded parse error, wrapped with position. Callers must only use
  /// it after a parse step failed; it falls back to the first recorded error.
  Error error() const {
    const char *At = ErrPos ? ErrPos : P;
    return invalidDB(Twine("malformed JSON at byte ") +
                     Twine(static_cast<uint64_t>(At - Start)) + ": " +
                     (Err.empty() ? StringRef("parse error") : Err));
  }

  /// Records the first parse error at byte `Off` (current position when
  /// negative). Returns an Error carrying it, for `return J.failAt(...)`.
  Error failAt(StringRef Msg, int64_t Off = -1) {
    if (!Failed) {
      Failed = true;
      Err = Msg;
      ErrPos = Off < 0 ? P : Start + Off;
    }
    return error();
  }

  /// Object key: a quoted string after optional whitespace.
  bool parseKey(std::string &Out) {
    skipWs();
    if (P >= E || *P != '"')
      return fail("expected object key");
    ++P;
    return parseStringBody(Out);
  }
  /// Quoted string after optional whitespace.
  bool parseString(std::string &Out) {
    skipWs();
    if (P >= E || *P != '"')
      return fail("expected string");
    ++P;
    return parseStringBody(Out);
  }

  /// Skips one complete JSON value, grammar-checked like llvm::json.
  bool skipValue() {
    skipWs();
    if (P >= E)
      return fail("unexpected end of input");
    switch (*P) {
    case '{': {
      ++P;
      if (consume('}'))
        return true;
      for (;;) {
        std::string K;
        if (!parseKey(K))
          return false;
        if (!consume(':'))
          return fail("expected ':'");
        if (!skipValue())
          return false;
        if (consume('}'))
          return true;
        if (!consume(','))
          return fail("expected ',' or '}'");
      }
    }
    case '[': {
      ++P;
      if (consume(']'))
        return true;
      for (;;) {
        if (!skipValue())
          return false;
        if (consume(']'))
          return true;
        if (!consume(','))
          return fail("expected ',' or ']'");
      }
    }
    case '"': {
      std::string S;
      return parseString(S);
    }
    case 't':
      return consumeWord("true", 4);
    case 'f':
      return consumeWord("false", 5);
    case 'n':
      return consumeWord("null", 4);
    default:
      JsonNumber N;
      switch (parseNumber(N)) {
      case NumOk:
        return true;
      case NumBad:
        return false;
      default:
        return fail("expected a JSON value");
      }
    }
  }

  /// llvm::json number classification: strtoll, then strtoull for
  /// non-negative literals, then strtod; the whole run must convert.
  struct JsonNumber {
    enum Kind { Int, UInt, Double } K;
    int64_t I;
    uint64_t U;
    double D;
  };
  /// llvm::json::Value::getAsInteger: exactly representable int64_t.
  static bool jsonAsInteger(const JsonNumber &N, int64_t &Out) {
    if (N.K == JsonNumber::Int) {
      Out = N.I;
      return true;
    }
    if (N.K == JsonNumber::UInt) {
      if (N.U <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        Out = static_cast<int64_t>(N.U);
        return true;
      }
      return false;
    }
    double I;
    if (std::modf(N.D, &I) == 0.0 && I >= -9223372036854775808.0 &&
        I < 9223372036854775808.0) {
      Out = static_cast<int64_t>(I);
      return true;
    }
    return false;
  }

  enum IntResult { IntOk, IntNotNumber, IntNotInteger, IntBad };
  /// Parses one number token. IntNotNumber leaves the cursor before the
  /// token; IntNotInteger consumed a valid non-integer number; IntBad
  /// consumed a malformed token and recorded an error.
  IntResult parseInteger(int64_t &Out) {
    JsonNumber N;
    switch (parseNumber(N)) {
    case NumNotNumber:
      return IntNotNumber;
    case NumBad:
      return IntBad;
    case NumOk:
      break;
    }
    if (!jsonAsInteger(N, Out))
      return IntNotInteger;
    return IntOk;
  }

  /// Old `getUInt`: a number that converts to a non-negative int64_t, else
  /// absent (the value is still consumed). Errors only on malformed JSON.
  Expected<std::optional<uint64_t>> parseUIntField() {
    skipWs();
    const char *At = P;
    int64_t I = 0;
    switch (parseInteger(I)) {
    case IntBad:
      return error();
    case IntNotNumber:
      P = At;
      if (!skipValue())
        return error();
      return std::optional<uint64_t>();
    case IntNotInteger:
      return std::optional<uint64_t>();
    case IntOk:
      break;
    }
    if (I < 0)
      return std::optional<uint64_t>();
    return std::optional<uint64_t>(static_cast<uint64_t>(I));
  }

  /// Old `getString`: a string or absent (non-string value consumed).
  Expected<std::optional<std::string>> parseStrField() {
    skipWs();
    if (P < E && *P == '"') {
      std::string S;
      ++P;
      if (!parseStringBody(S))
        return error();
      return std::optional<std::string>(std::move(S));
    }
    if (!skipValue())
      return error();
    return std::optional<std::string>();
  }

  /// Old `getBoolean`: a boolean or absent (non-boolean value consumed).
  Expected<std::optional<bool>> parseBoolField() {
    skipWs();
    const char *At = P;
    if (P < E && *P == 't') {
      if (!consumeWord("true", 4))
        return error();
      return std::optional<bool>(true);
    }
    if (P < E && *P == 'f') {
      if (!consumeWord("false", 5))
        return error();
      return std::optional<bool>(false);
    }
    P = At;
    if (!skipValue())
      return error();
    return std::optional<bool>();
  }

private:
  enum NumResult { NumOk, NumNotNumber, NumBad };

  static bool isNumChar(char C) {
    // Same set llvm::json uses, which intentionally accepts more than the
    // strict JSON grammar (leading '+', "1.", "1e", ...) so strto* decides.
    return (C >= '0' && C <= '9') || C == 'e' || C == 'E' || C == '+' ||
           C == '-' || C == '.';
  }

  /// Port of llvm::json's Parser::parseNumber.
  NumResult parseNumber(JsonNumber &Out) {
    skipWs();
    const char *Tok = P;
    while (P < E && isNumChar(*P))
      ++P;
    if (P == Tok)
      return NumNotNumber;
    SmallString<32> S;
    S.assign(Tok, P);
    char *End = nullptr;
    errno = 0;
    int64_t I = std::strtoll(S.c_str(), &End, 10);
    if (End == S.end() && errno != ERANGE) {
      Out.K = JsonNumber::Int;
      Out.I = I;
      return NumOk;
    }
    if (*Tok != '-') {
      errno = 0;
      uint64_t UI = std::strtoull(S.c_str(), &End, 10);
      if (End == S.end() && errno != ERANGE) {
        Out.K = JsonNumber::UInt;
        Out.U = UI;
        return NumOk;
      }
    }
    Out.D = std::strtod(S.c_str(), &End);
    if (End != S.end()) {
      fail("invalid JSON number");
      return NumBad;
    }
    Out.K = JsonNumber::Double;
    return NumOk;
  }

  /// String contents after the opening quote. Port of llvm::json's
  /// Parser::parseString.
  bool parseStringBody(std::string &Out) {
    Out.clear();
    for (;;) {
      if (P >= E)
        return fail("unterminated string");
      unsigned char C = static_cast<unsigned char>(*P);
      if (C == '"') {
        ++P;
        return true;
      }
      if (C == '\\') {
        ++P;
        if (P >= E)
          return fail("invalid escape sequence");
        switch (*P) {
        case '"': Out.push_back('"'); ++P; continue;
        case '\\': Out.push_back('\\'); ++P; continue;
        case '/': Out.push_back('/'); ++P; continue;
        case 'b': Out.push_back('\b'); ++P; continue;
        case 'f': Out.push_back('\f'); ++P; continue;
        case 'n': Out.push_back('\n'); ++P; continue;
        case 'r': Out.push_back('\r'); ++P; continue;
        case 't': Out.push_back('\t'); ++P; continue;
        case 'u':
          ++P;
          if (!parseUnicode(Out))
            return false;
          continue;
        default:
          return fail("invalid escape sequence");
        }
      }
      if (C < 0x20)
        return fail("control character in string");
      Out.push_back(static_cast<char>(C));
      ++P;
    }
  }

  bool parseHex16(uint16_t &Out) {
    uint32_t V = 0;
    for (int I = 0; I < 4; ++I) {
      if (P >= E)
        return fail("bad \\u escape sequence");
      char C = *P;
      V <<= 4;
      if (C >= '0' && C <= '9')
        V |= static_cast<uint32_t>(C - '0');
      else if (C >= 'a' && C <= 'f')
        V |= static_cast<uint32_t>(C - 'a' + 10);
      else if (C >= 'A' && C <= 'F')
        V |= static_cast<uint32_t>(C - 'A' + 10);
      else
        return fail("bad \\u escape sequence");
      ++P;
    }
    Out = static_cast<uint16_t>(V);
    return true;
  }

  /// Port of llvm::json's Parser::parseUnicode: unpaired surrogates are
  /// replaced by U+FFFD rather than rejected ("\u" was already consumed).
  bool parseUnicode(std::string &Out) {
    auto Invalid = [&Out] { Out.append("\xef\xbf\xbd", 3); };
    uint16_t First = 0;
    if (!parseHex16(First))
      return false;
    for (;;) {
      if (First < 0xD800 || First >= 0xE000) {
        appendUtf8(Out, First);
        return true;
      }
      if (First >= 0xDC00) {
        Invalid();
        return true;
      }
      if (P + 2 > E || P[0] != '\\' || P[1] != 'u') {
        Invalid();
        return true;
      }
      P += 2;
      uint16_t Second = 0;
      if (!parseHex16(Second))
        return false;
      if (Second < 0xDC00 || Second >= 0xE000) {
        Invalid();
        First = Second;
        continue;
      }
      appendUtf8(Out, 0x10000 | ((First - 0xD800) << 10) | (Second - 0xDC00));
      return true;
    }
  }

  static void appendUtf8(std::string &Out, uint32_t Cp) {
    if (Cp < 0x80) {
      Out.push_back(static_cast<char>(Cp));
    } else if (Cp < 0x800) {
      Out.push_back(static_cast<char>(0xC0 | (Cp >> 6)));
      Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F)));
    } else if (Cp < 0x10000) {
      Out.push_back(static_cast<char>(0xE0 | (Cp >> 12)));
      Out.push_back(static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)));
      Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F)));
    } else {
      Out.push_back(static_cast<char>(0xF0 | (Cp >> 18)));
      Out.push_back(static_cast<char>(0x80 | ((Cp >> 12) & 0x3F)));
      Out.push_back(static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)));
      Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F)));
    }
  }

  bool consumeWord(const char *W, size_t Len) {
    if (static_cast<size_t>(E - P) >= Len && memcmp(P, W, Len) == 0) {
      P += Len;
      return true;
    }
    return fail("bad literal");
  }
  bool fail(StringRef Msg) {
    if (!Failed) {
      Failed = true;
      Err = Msg;
      ErrPos = P;
    }
    return false;
  }

  const char *Start;
  const char *P;
  const char *E;
  bool Failed = false;
  const char *ErrPos = nullptr;
  StringRef Err;
};

// Parse a prototype object (schema of `prototypeFromJSON`). Mirroring the old
// json accessors, a present-but-wrong-typed field counts as missing and the
// last duplicate key wins.
static Expected<LibraryPrototype> parsePrototypeFast(JParser &J) {
  LibraryPrototype P;
  std::optional<std::string> ReturnType;
  std::optional<uint64_t> ReturnBits;
  std::optional<bool> ReturnFloat, ReturnVoid, VarArgs;
  std::optional<bool> HasParams;
  if (!J.consume('{'))
    return J.failAt("expected '{'");
  if (!J.consume('}')) {
    for (;;) {
      std::string Key;
      if (!J.parseKey(Key))
        return J.error();
      if (!J.consume(':'))
        return J.failAt("expected ':'");
      if (Key == "return_type") {
        auto S = J.parseStrField();
        if (!S)
          return S.takeError();
        ReturnType = std::move(*S);
        if (ReturnType)
          P.ReturnType = *ReturnType;
      } else if (Key == "return_bits") {
        auto V = J.parseUIntField();
        if (!V)
          return V.takeError();
        ReturnBits = *V;
        if (ReturnBits)
          P.ReturnBits = static_cast<uint32_t>(*ReturnBits);
      } else if (Key == "return_float") {
        auto B = J.parseBoolField();
        if (!B)
          return B.takeError();
        ReturnFloat = *B;
        if (ReturnFloat)
          P.ReturnFloat = *ReturnFloat;
      } else if (Key == "return_void") {
        auto B = J.parseBoolField();
        if (!B)
          return B.takeError();
        ReturnVoid = *B;
        if (ReturnVoid)
          P.ReturnVoid = *ReturnVoid;
      } else if (Key == "varargs") {
        auto B = J.parseBoolField();
        if (!B)
          return B.takeError();
        VarArgs = *B;
        if (VarArgs)
          P.VarArgs = *VarArgs;
      } else if (Key == "params") {
        P.Params.clear();
        HasParams = false;
        if (!J.consume('[')) {
          if (!J.skipValue())
            return J.error();
        } else {
          HasParams = true;
          if (!J.consume(']')) {
            for (;;) {
              LibraryParam Param;
              std::optional<std::string> ParamName, ParamType;
              std::optional<uint64_t> ParamBits;
              std::optional<bool> ParamFloat;
              // The old loader required each parameter element to be an
              // object.
              if (!J.consume('{'))
                return invalidDB("prototype parameter is not an object");
              if (!J.consume('}')) {
                for (;;) {
                  std::string PKey;
                  if (!J.parseKey(PKey))
                    return J.error();
                  if (!J.consume(':'))
                    return J.failAt("expected ':'");
                  if (PKey == "name") {
                    auto S = J.parseStrField();
                    if (!S)
                      return S.takeError();
                    ParamName = std::move(*S);
                    if (ParamName)
                      Param.Name = *ParamName;
                  } else if (PKey == "type") {
                    auto S = J.parseStrField();
                    if (!S)
                      return S.takeError();
                    ParamType = std::move(*S);
                    if (ParamType)
                      Param.Type = *ParamType;
                  } else if (PKey == "bits") {
                    auto V = J.parseUIntField();
                    if (!V)
                      return V.takeError();
                    ParamBits = *V;
                    if (ParamBits)
                      Param.Bits = static_cast<uint32_t>(*ParamBits);
                  } else if (PKey == "float") {
                    auto B = J.parseBoolField();
                    if (!B)
                      return B.takeError();
                    ParamFloat = *B;
                    if (ParamFloat)
                      Param.Float = *ParamFloat;
                  } else if (!J.skipValue()) {
                    return J.error();
                  }
                  if (J.consume('}'))
                    break;
                  if (!J.consume(','))
                    return J.failAt("expected ',' or '}'");
                }
              }
              if (!ParamName || !ParamType || !ParamBits || !ParamFloat)
                return invalidDB(
                    "prototype parameter is missing required fields");
              P.Params.push_back(std::move(Param));
              if (J.consume(']'))
                break;
              if (!J.consume(','))
                return J.failAt("expected ',' or ']'");
            }
          }
        }
      } else if (!J.skipValue()) {
        return J.error();
      }
      if (J.consume('}'))
        break;
      if (!J.consume(','))
        return J.failAt("expected ',' or '}'");
    }
  }
  if (!ReturnType || !ReturnBits || !ReturnFloat || !ReturnVoid || !VarArgs ||
      HasParams != std::optional<bool>(true))
    return invalidDB("prototype is missing required fields");
  return P;
}

// Parse one `functions` array entry into DB. Semantic validation mirrors the
// original json loader; parse failures surface the JParser error.
static Error parseFunctionEntry(JParser &J, LibraryDatabase &DB) {
  std::optional<std::string> Name, MangledName, Archive, Member, BytesHex;
  std::optional<uint64_t> Size;
  std::vector<int64_t> Mask;
  std::vector<std::string> Callees;
  std::optional<LibraryPrototype> Prototype;
  bool HasMask = false, HasCallees = false;
  if (!J.consume('{'))
    return J.failAt("expected '{'");
  if (!J.consume('}')) {
    for (;;) {
      std::string Key;
      if (!J.parseKey(Key))
        return J.error();
      if (!J.consume(':'))
        return J.failAt("expected ':'");
      if (Key == "name") {
        auto S = J.parseStrField();
        if (!S)
          return S.takeError();
        Name = std::move(*S);
      } else if (Key == "mangled_name") {
        auto S = J.parseStrField();
        if (!S)
          return S.takeError();
        MangledName = std::move(*S);
      } else if (Key == "archive") {
        auto S = J.parseStrField();
        if (!S)
          return S.takeError();
        Archive = std::move(*S);
      } else if (Key == "member") {
        auto S = J.parseStrField();
        if (!S)
          return S.takeError();
        Member = std::move(*S);
      } else if (Key == "size") {
        auto V = J.parseUIntField();
        if (!V)
          return V.takeError();
        Size = *V;
      } else if (Key == "bytes") {
        auto S = J.parseStrField();
        if (!S)
          return S.takeError();
        BytesHex = std::move(*S);
      } else if (Key == "masked_relocations") {
        Mask.clear();
        HasMask = false;
        if (!J.consume('[')) {
          if (!J.skipValue())
            return J.error();
        } else {
          HasMask = true;
          if (!J.consume(']')) {
            for (;;) {
              // Every element must convert via getAsInteger like the old
              // loader: non-integer values are rejected by the bounds check
              // below, malformed JSON by the parser.
              int64_t V = 0;
              switch (J.parseInteger(V)) {
              case JParser::IntBad:
                return J.error();
              case JParser::IntNotNumber:
                if (!J.skipValue())
                  return J.error();
                return invalidDB(
                    "relocation mask offset is outside the function");
              case JParser::IntNotInteger:
                return invalidDB(
                    "relocation mask offset is outside the function");
              case JParser::IntOk:
                break;
              }
              Mask.push_back(V);
              if (J.consume(']'))
                break;
              if (!J.consume(','))
                return J.failAt("expected ',' or ']'");
            }
          }
        }
      } else if (Key == "callees") {
        Callees.clear();
        HasCallees = false;
        if (!J.consume('[')) {
          if (!J.skipValue())
            return J.error();
        } else {
          HasCallees = true;
          if (!J.consume(']')) {
            for (;;) {
              J.skipWs();
              if (J.pos() >= J.end() || *J.pos() != '"') {
                if (!J.skipValue())
                  return J.error();
                return invalidDB("callee name is not a string");
              }
              std::string Callee;
              if (!J.parseString(Callee))
                return J.error();
              Callees.push_back(std::move(Callee));
              if (J.consume(']'))
                break;
              if (!J.consume(','))
                return J.failAt("expected ',' or ']'");
            }
          }
        }
      } else if (Key == "prototype") {
        // Old code used getObject("prototype"): a non-object value is
        // silently ignored, an object must parse as a full prototype.
        Prototype.reset();
        if (J.peek('{')) {
          auto Parsed = parsePrototypeFast(J);
          if (!Parsed)
            return Parsed.takeError();
          Prototype = std::move(*Parsed);
        } else if (!J.skipValue()) {
          return J.error();
        }
      } else if (!J.skipValue()) {
        return J.error();
      }
      if (J.consume('}'))
        break;
      if (!J.consume(','))
        return J.failAt("expected ',' or '}'");
    }
  }
  if (!Name || !MangledName || !Archive || !Member || !Size || !BytesHex ||
      !HasMask || !HasCallees)
    return invalidDB("function entry is missing required fields");
  auto Bytes = parseHexBytes(*BytesHex);
  if (!Bytes)
    return Bytes.takeError();
  if (*Size != Bytes->size())
    return invalidDB("function size does not match byte string");
  LibraryFunction F;
  F.Name = std::move(*Name);
  F.MangledName = std::move(*MangledName);
  F.Archive = std::move(*Archive);
  F.Member = std::move(*Member);
  F.Size = *Size;
  F.Bytes = std::move(*Bytes);
  F.MaskedRelocations.reserve(Mask.size());
  for (int64_t Off : Mask) {
    if (Off < 0 || static_cast<uint64_t>(Off) >= F.Size)
      return invalidDB("relocation mask offset is outside the function");
    F.MaskedRelocations.push_back(static_cast<uint32_t>(Off));
  }
  F.Callees = std::move(Callees);
  F.Prototype = std::move(Prototype);
  DB.Functions.push_back(std::move(F));
  return Error::success();
}

static Error loadLibraryDatabaseFromText(StringRef Text, LibraryDatabase &DB) {
  JParser J(Text);
  std::optional<uint64_t> Version;
  bool HaveFunctions = false;
  // llvm::json validates the document encoding before parsing.
  size_t Utf8Error = 0;
  if (!json::isUTF8(Text, &Utf8Error))
    return J.failAt("invalid UTF-8 sequence", static_cast<int64_t>(Utf8Error));
  J.skipWs();
  if (J.pos() >= J.end())
    return J.failAt("unexpected end of input");
  if (!J.peek('{'))
    return invalidDB("root is not an object");
  J.consume('{');
  if (!J.consume('}')) {
    for (;;) {
      std::string Key;
      if (!J.parseKey(Key))
        return J.error();
      if (!J.consume(':'))
        return J.failAt("expected ':'");
      if (Key == "version") {
        auto V = J.parseUIntField();
        if (!V)
          return V.takeError();
        Version = *V;
      } else if (Key == "functions") {
        // Last duplicate key wins, like the old Object map lookup.
        DB.Functions.clear();
        HaveFunctions = false;
        if (!J.consume('[')) {
          if (!J.skipValue())
            return J.error();
        } else {
          HaveFunctions = true;
          if (!J.consume(']')) {
            for (;;) {
              J.skipWs();
              if (J.pos() >= J.end() || *J.pos() != '{') {
                if (!J.skipValue())
                  return J.error();
                return invalidDB("function entry is not an object");
              }
              if (Error E = parseFunctionEntry(J, DB))
                return E;
              if (J.consume(']'))
                break;
              if (!J.consume(','))
                return J.failAt("expected ',' or ']'");
            }
          }
        }
      } else if (!J.skipValue()) {
        return J.error();
      }
      if (J.consume('}'))
        break;
      if (!J.consume(','))
        return J.failAt("expected ',' or '}'");
    }
  }
  J.skipWs();
  if (J.pos() != J.end())
    return J.failAt("text after end of document");
  if (Version != std::optional<uint64_t>(1) || !HaveFunctions)
    return invalidDB("unsupported version or missing functions array");
  return Error::success();
}


static std::string hexBytes(ArrayRef<uint8_t> Bytes) {
  static const char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.resize(Bytes.size() * 2);
  for (size_t I = 0; I < Bytes.size(); ++I) {
    Result[2 * I] = Digits[Bytes[I] >> 4];
    Result[2 * I + 1] = Digits[Bytes[I] & 0x0f];
  }
  return Result;
}

static json::Value prototypeToJSON(const LibraryPrototype &P) {
  json::Array Params;
  for (const LibraryParam &Param : P.Params) {
    json::Object Obj;
    Obj["name"] = Param.Name;
    Obj["type"] = Param.Type;
    Obj["bits"] = static_cast<int64_t>(Param.Bits);
    Obj["float"] = Param.Float;
    Params.push_back(std::move(Obj));
  }
  json::Object Obj;
  Obj["return_type"] = P.ReturnType;
  Obj["return_bits"] = static_cast<int64_t>(P.ReturnBits);
  Obj["return_float"] = P.ReturnFloat;
  Obj["return_void"] = P.ReturnVoid;
  Obj["varargs"] = P.VarArgs;
  Obj["params"] = std::move(Params);
  return Obj;
}


static uint32_t registerForArg(unsigned Index, bool IsFloat) {
  static const uint32_t IntRegs[] = {X86::RCX, X86::RDX, X86::R8, X86::R9};
  static const uint32_t FloatRegs[] = {X86::XMM0, X86::XMM1, X86::XMM2,
                                       X86::XMM3};
  return IsFloat ? FloatRegs[Index] : IntRegs[Index];
}

/// Callee-name comparison key: imports come in as `dll!name`, archive
/// records store demangled `name` — compare on alphanumeric runs so
/// decoration spellings still score.
static std::string nameKey(StringRef Name) {
  size_t Bang = Name.rfind('!');
  if (Bang != StringRef::npos)
    Name = Name.drop_front(Bang + 1);
  std::string Key;
  for (char C : Name)
    if (isAlnum(C) || C == '_')
      Key.push_back(C);
  return Key;
}

/// Name-collision key: the domain the lifter's `sanitizeName` produces, where
/// alphanumerics, `_` and `.` carry over and every other byte becomes `_XX`
/// hex. Uniqueness has to be decided on that spelling: the raw names can
/// collide only after sanitization, and the lifter refuses to define two
/// functions with the same IR name.
static std::string sanitizedKey(StringRef Name) {
  std::string Key;
  Key.reserve(Name.size() * 3);
  for (char C : Name) {
    if (isAlnum(C) || C == '_' || C == '.') {
      Key.push_back(C);
    } else {
      char Buf[4];
      snprintf(Buf, sizeof(Buf), "_%02x", (unsigned char)C);
      Key += Buf;
    }
  }
  return Key.empty() ? std::string("f") : Key;
}

static uint32_t parseTypeBits(StringRef Type, uint32_t Fallback) {
  Type = Type.trim();
  if (Type.contains("char") || Type.contains("bool"))
    return 8;
  if (Type.contains("short") || Type.contains("wchar_t"))
    return 16;
  if (Type.contains("float"))
    return 32;
  if (Type.contains("double"))
    return 64;
  if (Type.contains("long long") || Type.contains("__int64"))
    return 64;
  if (Type.contains("long") || Type.contains("int") ||
      Type.contains("enum"))
    return 32;
  if (Type.contains("*") || Type.contains("&"))
    return 64;
  return Fallback;
}

} // namespace

Expected<LibraryDatabase> LibraryDatabase::load(StringRef Path) {
  auto Buffer = MemoryBuffer::getFile(Path);
  if (!Buffer)
    return createFileError(Path, Buffer.getError());
  LibraryDatabase DB;
  if (Error E = loadLibraryDatabaseFromText((*Buffer)->getBuffer(), DB))
    return std::move(E);
  return DB;
}

namespace {
std::mutex DBCacheMutex;
StringMap<std::shared_ptr<const LibraryDatabase>> DBCache;
} // namespace

Expected<std::shared_ptr<const LibraryDatabase>>
loadLibraryDatabaseCached(StringRef Path) {
  std::lock_guard<std::mutex> Lock(DBCacheMutex);
  auto It = DBCache.find(Path);
  if (It != DBCache.end())
    return It->second;
  auto DB = LibraryDatabase::load(Path);
  if (!DB)
    return DB.takeError();
  auto Ptr = std::make_shared<const LibraryDatabase>(std::move(*DB));
  DBCache[Path] = Ptr;
  return Ptr;
}

Error LibraryDatabase::save(StringRef Path) const {
  json::Array Entries;
  for (const LibraryFunction &F : Functions) {
    json::Object Entry;
    Entry["name"] = F.Name;
    Entry["mangled_name"] = F.MangledName;
    Entry["archive"] = F.Archive;
    Entry["member"] = F.Member;
    Entry["size"] = static_cast<int64_t>(F.Size);
    Entry["bytes"] = hexBytes(F.Bytes);
    json::Array Mask;
    for (uint32_t Offset : F.MaskedRelocations)
      Mask.push_back(static_cast<int64_t>(Offset));
    Entry["masked_relocations"] = std::move(Mask);
    json::Array Callees;
    for (const std::string &Callee : F.Callees)
      Callees.push_back(Callee);
    Entry["callees"] = std::move(Callees);
    if (F.Prototype)
      Entry["prototype"] = prototypeToJSON(*F.Prototype);
    Entries.push_back(std::move(Entry));
  }
  json::Object Root;
  Root["version"] = 1;
  Root["functions"] = std::move(Entries);
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC)
    return createFileError(Path, EC);
  OS << formatv("{0:2}", json::Value(std::move(Root))) << '\n';
  return Error::success();
}

std::vector<LibraryMatch> matchLibraryFunctions(const Program &P,
                                                const LibraryDatabase &DB) {
  std::map<uint64_t, std::vector<uint32_t>> BySize;
  for (uint32_t I = 0; I < DB.Functions.size(); ++I)
    BySize[DB.Functions[I].Size].push_back(I);
  // A match needs every unmasked byte equal, so the unmasked bytes among the
  // first eight must be equal too: one masked 64-bit compare rejects almost
  // every candidate before the per-byte walk below.
  struct Prefix {
    uint64_t Bytes = 0;
    uint64_t Mask = 0;
  };
  std::vector<Prefix> Prefixes(DB.Functions.size());
  for (uint32_t I = 0; I < DB.Functions.size(); ++I) {
    const LibraryFunction &F = DB.Functions[I];
    for (unsigned B = 0; B < 8 && B < F.Bytes.size(); ++B)
      if (!std::binary_search(F.MaskedRelocations.begin(),
                              F.MaskedRelocations.end(), B)) {
        Prefixes[I].Bytes |= uint64_t(F.Bytes[B]) << (8 * B);
        Prefixes[I].Mask |= uint64_t(0xff) << (8 * B);
      }
  }
  std::vector<LibraryMatch> Matches;
  if (!P.Img)
    return Matches;
  for (const auto &[Start, Record] : P.Functions) {
    if (Record.Chunks.size() != 1 || Record.Chunks[0].first != Start)
      continue;
    uint64_t Size = Record.Chunks[0].second - Start;
    if (Size > SIZE_MAX)
      continue;
    auto Raw = P.Img->dataAt(Start, static_cast<size_t>(Size));
    if (Raw.size() != Size)
      continue;
    uint64_t RawPrefix = 0;
    for (unsigned B = 0; B < 8 && B < Size; ++B)
      RawPrefix |= uint64_t(Raw[B]) << (8 * B);
    const uint64_t InSize =
        Size >= 8 ? ~uint64_t(0) : (uint64_t(1) << (8 * Size)) - 1;
    for (auto It = BySize.lower_bound(Size);
         It != BySize.end() && It->first - Size <= 15; ++It) {
      for (uint32_t Index : It->second) {
        const Prefix &Pre = Prefixes[Index];
        if ((RawPrefix ^ Pre.Bytes) & Pre.Mask & InSize)
          continue;
        const LibraryFunction &F = DB.Functions[Index];
        if (F.Size != F.Bytes.size())
          continue;
        if (std::any_of(F.MaskedRelocations.begin(),
                        F.MaskedRelocations.end(),
                        [&](uint32_t Offset) { return Offset >= Size; }))
          continue;
        bool AlignedPadding = std::all_of(
            F.Bytes.begin() + Size, F.Bytes.end(),
            [](uint8_t Byte) { return Byte == 0x90 || Byte == 0xCC; });
        if (!AlignedPadding)
          continue;
        bool Equal = true;
        for (uint64_t I = 0; I < Size; ++I) {
          bool Masked = std::binary_search(F.MaskedRelocations.begin(),
                                           F.MaskedRelocations.end(),
                                           static_cast<uint32_t>(I));
          if (!Masked && Raw[I] != F.Bytes[I]) {
            Equal = false;
            break;
          }
        }
        if (Equal)
          Matches.push_back({Start, Index});
      }
    }
  }
  return Matches;
}

void extendLibraryMatchesByCalleeEvidence(const Program &P,
                                           const LibraryDatabase &DB,
                                           std::vector<LibraryMatch> &Matches) {
  std::map<std::string, std::vector<uint32_t>> ByName;
  for (uint32_t I = 0; I < DB.Functions.size(); ++I)
    ByName[DB.Functions[I].Name].push_back(I);
  std::set<Va> Matched;
  std::map<Va, std::string> MatchedName;
  for (const LibraryMatch &M : Matches) {
    Matched.insert(M.Start);
    if (M.FunctionIndex < DB.Functions.size())
      MatchedName[M.Start] = DB.Functions[M.FunctionIndex].Name;
  }
  auto calleeName = [&](Va Callee) -> std::string {
    auto Known = MatchedName.find(Callee);
    if (Known != MatchedName.end())
      return Known->second;
    if (P.Names)
      if (auto It = P.Names->find(Callee); It != P.Names->end())
        return It->second;
    auto Rec = P.Functions.find(Callee);
    if (Rec != P.Functions.end())
      return Rec->second.Name;
    return {};
  };
  bool Changed;
  do {
    Changed = false;
    for (const auto &[Start, Record] : P.Functions) {
      if (Matched.count(Start))
        continue;
      // Program name for the candidate: the name table carries import and
      // discovered names; fall back to the record.
      std::string Name;
      if (P.Names)
        if (auto It = P.Names->find(Start); It != P.Names->end())
          Name = It->second;
      if (Name.empty() || StringRef(Name).starts_with("sub_"))
        Name = Record.Name;
      bool Generic =
          Name.empty() || StringRef(Name).starts_with("sub_");
      const std::vector<uint32_t> *Candidates = nullptr;
      if (!Generic) {
        auto Found = ByName.find(Name);
        if (Found == ByName.end())
          continue;
        Candidates = &Found->second;
      }
      if (Record.CallTargets.empty())
        continue;
      // Compiler-inserted instrumentation calls (stack cookies, probes)
      // are transparent for library membership: they appear in user and
      // library bodies alike and must neither gate nor score a match.
      auto isInstrumentation = [&](Va Callee) {
        std::string N = calleeName(Callee);
        StringRef S(N);
        // Imports arrive as `dll!name`; compare the bare name.
        size_t Bang = S.rfind('!');
        if (Bang != StringRef::npos)
          S = S.drop_front(Bang + 1);
        // Unnamed callees resolve through the callee's own record below.
        if (S.empty() || S.starts_with("sub_")) {
          auto Rec = P.Functions.find(Callee);
          if (Rec == P.Functions.end() || Rec->second.Name.empty())
            return false;
          S = StringRef(Rec->second.Name);
        }
        return S == "__security_check_cookie" || S == "__report_gsfailure" ||
               S == "_alloca_probe" || S == "__alloca_probe" ||
               S == "_RTC_CheckStackVars" || S == "__RTC_CheckEsp";
      };
      std::set<std::string> Known;
      bool AllMatched = true;
      for (Va Callee : Record.CallTargets) {
        if (Callee != Start && isInstrumentation(Callee))
          continue;
        if (Matched.count(Callee) || (P.Externs && P.Externs->count(Callee))) {
          std::string CalleeName = calleeName(Callee);
          if (!CalleeName.empty())
            Known.insert(nameKey(CalleeName));
          continue;
        }
        // Self-recursion says nothing about library membership.
        if (Callee == Start)
          continue;
        AllMatched = false;
        break;
      }
      if (!AllMatched || Known.empty() || Generic)
        continue;
      // Named candidates only: the program name plus at least one
      // agreeing callee disambiguates same-named records. Generic
      // sub_-named candidates are left unmatched: callee sets alone
      // fire on user code that calls library helpers.
      bool Hit = false;
      uint32_t Best = 0;
      int BestScore = 0;
      bool Tie = false;
      for (uint32_t Index : *Candidates) {
        const LibraryFunction &F = DB.Functions[Index];
        if (F.Callees.empty())
          continue;
        int Score = 0;
        for (const std::string &Callee : F.Callees)
          Score += Known.count(nameKey(Callee));
        if (Score > BestScore) {
          BestScore = Score;
          Best = Index;
          Hit = true;
          Tie = false;
        } else if (Score == BestScore && Score > 0) {
          Tie = true;
        }
      }
      if (Tie || !Hit || BestScore == 0)
        continue;
      Matches.push_back({Start, Best});
      Matched.insert(Start);
      MatchedName[Start] = DB.Functions[Best].Name;
      Changed = true;
    }
  } while (Changed);
}

void applyLibraryMatches(Program &P, Signatures &Sigs,
                         ArrayRef<LibraryMatch> Matches,
                         const LibraryDatabase &DB) {
  if (!P.Names)
    P.Names = std::make_shared<std::map<Va, std::string>>();
  std::map<Va, std::vector<uint32_t>> ByStart;
  for (const LibraryMatch &M : Matches)
    if (M.FunctionIndex < DB.Functions.size())
      ByStart[M.Start].push_back(M.FunctionIndex);

  std::map<Va, uint32_t> Chosen;
  for (const auto &[Start, Candidates] : ByStart) {
    if (Candidates.size() == 1) {
      Chosen[Start] = Candidates.front();
      continue;
    }
    const std::string &Name = DB.Functions[Candidates.front()].Name;
    if (std::all_of(Candidates.begin(), Candidates.end(), [&](uint32_t I) {
          return DB.Functions[I].Name == Name;
        }))
      Chosen[Start] = Candidates.front();
  }

  bool Changed;
  do {
    Changed = false;
    for (const auto &[Start, Candidates] : ByStart) {
      if (Chosen.count(Start) || Candidates.size() < 2)
        continue;
      const FuncRecord *Record = nullptr;
      auto RecordIt = P.Functions.find(Start);
      if (RecordIt != P.Functions.end())
        Record = &RecordIt->second;
      if (!Record)
        continue;
      int BestScore = 0;
      std::optional<uint32_t> Best;
      bool Tie = false;
      for (uint32_t Candidate : Candidates) {
        const LibraryFunction &F = DB.Functions[Candidate];
        int Score = 0;
        std::set<std::string> Known;
        for (const std::string &Callee : F.Callees)
          Known.insert(nameKey(Callee));
        for (Va Callee : Record->CallTargets) {
          std::string Name;
          if (auto It = P.Names->find(Callee); It != P.Names->end())
            Name = It->second;
          else if (auto It = Chosen.find(Callee); It != Chosen.end())
            Name = DB.Functions[It->second].Name;
          if (!Name.empty() && Known.count(nameKey(Name)))
            ++Score;
        }
        if (Score > BestScore) {
          BestScore = Score;
          Best = Candidate;
          Tie = false;
        } else if (Score == BestScore && Score > 0) {
          Tie = true;
        }
      }
      if (Best && !Tie) {
        Chosen[Start] = *Best;
        Changed = true;
      }
    }
  } while (Changed);

  std::set<std::string> UsedNames;
  for (const auto &[Address, Name] : *P.Names)
    if (!Chosen.count(Address) || !StringRef(Name).starts_with("sub_"))
      UsedNames.insert(sanitizedKey(Name));

  for (const auto &[Start, Index] : Chosen) {
    const LibraryFunction &F = DB.Functions[Index];
    auto Existing = P.Names->find(Start);
    bool ReplaceName = Existing == P.Names->end() ||
                       StringRef(Existing->second).starts_with("sub_");
    if (ReplaceName) {
      std::string Name = F.Name;
      if (UsedNames.count(sanitizedKey(Name)))
        Name += "__" + utohexstr(Start.Value, true);
      UsedNames.insert(sanitizedKey(Name));
      (*P.Names)[Start] = Name;
      auto RecordIt = P.Functions.find(Start);
      if (RecordIt != P.Functions.end())
        RecordIt->second.Name = std::move(Name);
    } else {
      UsedNames.insert(sanitizedKey(Existing->second));
    }
    auto SigIt = Sigs.Funcs.find(Start);
    if (SigIt == Sigs.Funcs.end() || !F.Prototype)
      continue;
    Signature &Sig = SigIt->second;
    Sig.Params.clear();
    for (size_t I = 0; I < F.Prototype->Params.size(); ++I) {
      const LibraryParam &P_ = F.Prototype->Params[I];
      Param Param_;
      Param_.Bits = P_.Bits ? P_.Bits : parseTypeBits(P_.Type, 64);
      Param_.Float = P_.Float;
      Param_.Name = P_.Name;
      Param_.Type = P_.Type;
      if (I < 4)
        Param_.Loc = ParamLoc::reg(registerForArg(I, P_.Float));
      else
        Param_.Loc = ParamLoc::stack(0x28 + 8 * (I - 4));
      Sig.Params.push_back(std::move(Param_));
    }
    Sig.VarArgs = F.Prototype->VarArgs;
    Sig.HasRet = !F.Prototype->ReturnVoid;
    Sig.Ret.Float = F.Prototype->ReturnFloat;
    Sig.Ret.Bits = F.Prototype->ReturnBits;
    Sig.SigSource = Signature::Source::Library;
  }
  std::set<std::string> EmittedNames;
  for (const auto &[Address, Name] : *P.Names)
    if (!P.Functions.count(Address))
      EmittedNames.insert(sanitizedKey(Name));
  for (auto &[Start, Record] : P.Functions) {
    std::string &Name = (*P.Names)[Start];
    if (Name.empty())
      Name = Record.Name;
    std::string Key = sanitizedKey(Name);
    if (EmittedNames.count(Key)) {
      Name += "__" + utohexstr(Start.Value, true);
      Key = sanitizedKey(Name);
    }
    EmittedNames.insert(Key);
    Record.Name = Name;
  }
}

} // namespace revenant
