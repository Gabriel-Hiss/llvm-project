//==- revenant/lib/Analysis/Strings.cpp - Referenced strings -------------==//
//
// See revenant/Analysis/Strings.h. Ports crates/analysis/src/strings.rs 1:1:
//
// - ASCII: NUL-terminated bytes, legal UTF-8 whose characters are printable
//   or tab, newline and carriage return, >= 4. rustc concatenates string
//   fragments, so one constant may mix plain ASCII with multi-byte text.
// - UTF-16: LE units up to the first NUL, strictly decoded (lone
//   surrogates reject, like Rust's `String::from_utf16`), >= 4 chars, all
//   non-control or tab.
// - Read caps: 4096 bytes ASCII, 8192 bytes UTF-16.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Strings.h"

#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ConvertUTF.h"

namespace revenant {
namespace {

constexpr size_t AsciiCap = 4096;
constexpr size_t WideCap = 8192;
constexpr size_t MinLen = 4;

// A referenced run may hold UTF-8 text: rustc concatenates string fragments
// into one constant, so a merged panic message mixes plain ASCII with
// multi-byte characters. The run is legal UTF-8 and every character is
// printable, with tab, newline and carriage return the control characters
// kept. Reads that fail either test fall through to the UTF-16 reader.
bool isTextualUTF8(llvm::StringRef Run) {
  const llvm::UTF8 *Cursor = reinterpret_cast<const llvm::UTF8 *>(Run.data());
  const llvm::UTF8 *RunEnd = Cursor + Run.size();
  while (Cursor < RunEnd) {
    llvm::UTF32 Cp = 0;
    if (llvm::convertUTF8Sequence(&Cursor, RunEnd, &Cp,
                                  llvm::strictConversion) !=
        llvm::conversionOK)
      return false;
    if (Cp == '\t' || Cp == '\n' || Cp == '\r')
      continue;
    if (Cp < 0x20 || (Cp >= 0x7f && Cp <= 0x9f))
      return false;
  }
  return true;
}

} // anonymous namespace

std::optional<std::string> stringAt(const Image &Img, Va Addr) {
  const Section *Sec = Img.sectionOf(Addr);
  if (!Sec || Sec->Flags.Execute || !Sec->Flags.Read)
    return std::nullopt;

  // ASCII, including UTF-8 text. A merged constant mixes plain ASCII with
  // multi-byte characters, so the run is decoded as UTF-8 rather than checked
  // byte by byte; reads that are not textual fall through to the UTF-16
  // reader below.
  if (auto Bytes = Img.readAtMost(Addr, AsciiCap)) {
    size_t End = 0;
    while (End < Bytes->size() && (*Bytes)[End] != 0)
      ++End;
    if (End >= MinLen &&
        isTextualUTF8(llvm::StringRef((const char *)Bytes->data(), End)))
      return std::string((const char *)Bytes->data(), End);
  }

  // UTF-16.
  if (auto Bytes = Img.readAtMost(Addr, WideCap)) {
    std::vector<llvm::UTF16> Units;
    for (size_t I = 0; I + 1 < Bytes->size(); I += 2) {
      uint16_t U =
          (uint16_t)(*Bytes)[I] | (uint16_t)((uint16_t)(*Bytes)[I + 1] << 8);
      if (U == 0)
        break;
      Units.push_back(U);
    }
    std::string Text;
    llvm::SmallVector<llvm::UTF16, 32> Chars;
    if (!Units.empty() &&
        llvm::convertUTF16ToUTF8String(llvm::ArrayRef<llvm::UTF16>(Units),
                                       Text) &&
        llvm::convertUTF8ToUTF16String(llvm::StringRef(Text), Chars)) {
      // The UTF-16 round trip restores the units 1:1 unless the text held
      // a surrogate pair (2 units, 1 char): count chars as units minus
      // trail surrogates.
      size_t CharsSeen = 0;
      bool Clean = true;
      for (size_t K = 0; K < Chars.size(); ++K) {
        uint16_t U = Chars[K];
        if (U >= 0xdc00 && U <= 0xdfff)
          continue; // Trail surrogate: halves one char, not a new one.
        if ((U < 0x20 || (U >= 0x7f && U <= 0x9f)) && U != '\t') {
          Clean = false;
          break;
        }
        ++CharsSeen;
      }
      if (Clean && CharsSeen >= MinLen)
        return Text;
    }
  }
  return std::nullopt;
}

std::map<Va, std::string> collectStrings(const Image &Img, const Xrefs &Xr) {
  std::map<Va, std::string> Out;
  for (Va Target : Xr.targetVas())
    if (auto S = stringAt(Img, Target))
      Out.emplace(Target, std::move(*S));
  return Out;
}

} // end namespace revenant
