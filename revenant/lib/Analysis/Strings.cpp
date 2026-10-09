//==- revenant/lib/Analysis/Strings.cpp - Referenced strings -------------==//
//
// See revenant/Analysis/Strings.h. Ports crates/analysis/src/strings.rs 1:1:
//
// - ASCII: NUL-terminated bytes, all printable (0x20..0x7e) or tab, >= 4.
//   The printable range is plain ASCII, so the Rust side's UTF-8 validity
//   check holds by construction.
// - UTF-16: LE units up to the first NUL, strictly decoded (lone
//   surrogates reject, like Rust's `String::from_utf16`), >= 4 chars, all
//   non-control or tab.
// - Read caps: 4096 bytes ASCII, 8192 bytes UTF-16.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Strings.h"

#include <cstdint>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ConvertUTF.h"

namespace revenant {
namespace {

constexpr size_t AsciiCap = 4096;
constexpr size_t WideCap = 8192;
constexpr size_t MinLen = 4;

bool isAsciiPrintable(uint8_t B) {
  return (B >= 0x20 && B < 0x7f) || B == '\t';
}

}


std::optional<std::string> stringAt(const Image &Img, Va Addr) {
  const Section *Sec = Img.sectionOf(Addr);
  if (!Sec || Sec->Flags.Execute || !Sec->Flags.Read)
    return std::nullopt;

  // ASCII.
  if (auto Bytes = Img.readAtMost(Addr, AsciiCap)) {
    size_t End = 0;
    while (End < Bytes->size() && (*Bytes)[End] != 0)
      ++End;
    bool Printable = true;
    for (size_t I = 0; I < End; ++I)
      if (!isAsciiPrintable((*Bytes)[I])) {
        Printable = false;
        break;
      }
    if (End >= MinLen && Printable)
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
