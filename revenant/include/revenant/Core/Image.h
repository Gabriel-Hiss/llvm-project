//==- revenant/Core/Image.h - Loaded image model -----------------*- C++ -*-==//
//
// Mirrors crates/core/src/lib.rs `Section`, `SectionFlags` and `Image`:
// image base + sections, VA->byte access with zero-fill, inline-data ranges.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CORE_IMAGE_H
#define REVENANT_CORE_IMAGE_H

#include "revenant/Core/Va.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

namespace revenant {

/// Access flags for an image section.
struct SectionFlags {
  bool Read = false;
  bool Write = false;
  bool Execute = false;

  static constexpr SectionFlags r() { return {true, false, false}; }
  static constexpr SectionFlags rw() { return {true, true, false}; }
  static constexpr SectionFlags rx() { return {true, false, true}; }
  static constexpr SectionFlags rwx() { return {true, true, true}; }
};

/// One mapped section.
struct Section {
  std::string Name;
  /// Absolute start VA (image base + RVA).
  Va SecVa;
  /// Virtual size (mapped size; may exceed `Data.size()`).
  uint64_t Vsize = 0;
  /// Raw section bytes (may be shorter than `Vsize`; rest is zero-fill).
  std::vector<uint8_t> Data;
  SectionFlags Flags;

  Va end() const { return SecVa.wrappingAdd(Vsize); }
  VaRange range() const { return {SecVa, end()}; }
  bool contains(Va V) const { return V >= SecVa && V < end(); }

  /// Bytes at `va..va+len`, including implicit zero-fill beyond `Data`.
  std::optional<std::vector<uint8_t>> read(Va Addr, size_t Len) const {
    if (Addr < SecVa)
      return std::nullopt;
    uint64_t Off = Addr - SecVa;
    if (Off + Len > Vsize || Off + Len < Off)
      return std::nullopt;
    std::vector<uint8_t> Buf;
    Buf.reserve(Len);
    if (Off < Data.size()) {
      size_t Avail = std::min<size_t>(Data.size() - (size_t)Off, Len);
      Buf.insert(Buf.end(), Data.begin() + Off, Data.begin() + Off + Avail);
    }
    Buf.resize(Len, 0);
    return Buf;
  }
};

/// A loaded executable image: image base + sections.
struct Image {
  Va Base;
  std::vector<Section> Sections;
  /// Sorted, disjoint ranges inside executable sections that hold data,
  /// not code: relocation slots, linker-emitted records like the Go
  /// build-id blob. Decoders must not start or enter these.
  std::vector<VaRange> InlineData;

  /// Read `len` bytes at `va` from whichever section contains the range.
  /// Zero-fill beyond a section's raw data is honoured.
  std::optional<std::vector<uint8_t>> read(Va Addr, size_t Len) const {
    const Section *S = sectionOf(Addr);
    if (!S)
      return std::nullopt;
    return S->read(Addr, Len);
  }

  /// Read bytes at `va`, shrinking `len` to what the section holds.
  std::optional<std::vector<uint8_t>> readAtMost(Va Addr, size_t Len) const {
    const Section *S = sectionOf(Addr);
    if (!S)
      return std::nullopt;
    uint64_t Off = Addr - S->SecVa;
    if (Off >= S->Data.size())
      return std::vector<uint8_t>();
    size_t Avail = std::min<size_t>(S->Data.size() - (size_t)Off, Len);
    return std::vector<uint8_t>(S->Data.begin() + Off,
                                S->Data.begin() + Off + Avail);
  }

  /// Borrowed form of `readAtMost`: up to `len` raw bytes at `va`.
  /// Returns an empty slice (not nullopt) when `va` maps but has no
  /// backing bytes left.
  llvm::ArrayRef<uint8_t> dataAt(Va Addr, size_t Len) const {
    const Section *S = sectionOf(Addr);
    if (!S)
      return {};
    uint64_t Off = Addr - S->SecVa;
    if (Off >= S->Data.size())
      return {};
    size_t Avail = std::min<size_t>(S->Data.size() - (size_t)Off, Len);
    return llvm::ArrayRef<uint8_t>(S->Data.data() + Off, Avail);
  }

  std::optional<uint32_t> readU32(Va Addr) const {
    auto B = dataAt(Addr, 4);
    if (B.size() < 4) {
      auto Owned = read(Addr, 4);
      if (!Owned)
        return std::nullopt;
      return llvm::support::endian::read32le(Owned->data());
    }
    return llvm::support::endian::read32le(B.data());
  }

  std::optional<uint64_t> readU64(Va Addr) const {
    auto B = dataAt(Addr, 8);
    if (B.size() < 8) {
      auto Owned = read(Addr, 8);
      if (!Owned)
        return std::nullopt;
      return llvm::support::endian::read64le(Owned->data());
    }
    return llvm::support::endian::read64le(B.data());
  }

  const Section *sectionOf(Va Addr) const {
    for (const Section &S : Sections)
      if (S.contains(Addr))
        return &S;
    return nullptr;
  }

  /// Section by name (PE name, e.g. `.text`).
  const Section *sectionNamed(llvm::StringRef Name) const {
    for (const Section &S : Sections)
      if (S.Name == Name)
        return &S;
    return nullptr;
  }

  /// True if `va` lands in an executable section.
  bool isExecutable(Va Addr) const {
    const Section *S = sectionOf(Addr);
    return S && S->Flags.Execute;
  }

  /// True if `va` is inside a writable section.
  bool isWritable(Va Addr) const {
    const Section *S = sectionOf(Addr);
    return S && S->Flags.Write;
  }

  /// True if `va` maps to a real file byte — inside a section's mapped
  /// extent, not its zero-filled virtual tail.
  bool isBacked(Va Addr) const {
    const Section *S = sectionOf(Addr);
    return S && (Addr - S->SecVa) < S->Data.size();
  }

  /// True if `va` falls inside a recorded inline-data range.
  bool inInlineData(Va Addr) const {
    for (const VaRange &R : InlineData)
      if (R.contains(Addr))
        return true;
    return false;
  }

  /// When `va` sits inside an inline-data range, the range's end;
  /// else nullopt.
  std::optional<Va> inlineDataEnd(Va Addr) const {
    for (const VaRange &R : InlineData)
      if (R.contains(Addr))
        return R.End;
    return std::nullopt;
  }

  /// VA of the image base (i.e. where `__ImageBase` resolves).
  Va imageBase() const { return Base; }

  /// RVA -> VA.
  Va vaOfRva(Rva R) const { return Base.wrappingAdd(R); }

  /// VA -> RVA when inside the image.
  std::optional<Rva> rvaOf(Va Addr) const {
    if (auto Off = Addr.offsetFrom(Base))
      if (*Off <= UINT32_MAX)
        return (Rva)*Off;
    return std::nullopt;
  }

  /// NUL-terminated ASCII string at `va`, capped at `max` bytes.
  std::optional<std::string> readCstr(Va Addr, size_t Max) const {
    auto Bytes = readAtMost(Addr, Max);
    if (!Bytes)
      return std::nullopt;
    size_t End = 0;
    while (End < Bytes->size() && (*Bytes)[End] != 0)
      ++End;
    return std::string((const char *)Bytes->data(), End);
  }

  /// NUL-terminated UTF-16 string at `va`, capped at `max` bytes.
  std::optional<std::string> readWstr(Va Addr, size_t Max) const {
    auto Raw = readAtMost(Addr, Max);
    if (!Raw)
      return std::nullopt;
    std::string Out;
    for (size_t I = 0; I + 1 < Raw->size(); I += 2) {
      uint16_t U = llvm::support::endian::read16le(Raw->data() + I);
      if (U == 0)
        break;
      if (U < 0x80)
        Out.push_back((char)U);
      else {
        char Utf8[4] = {0};
        // Minimal UTF-16 -> UTF-8 for non-surrogate units.
        if (U < 0x800) {
          Utf8[0] = (char)(0xC0 | (U >> 6));
          Utf8[1] = (char)(0x80 | (U & 0x3F));
          Utf8[2] = 0;
        } else if (U < 0xD800 || U >= 0xE000) {
          Utf8[0] = (char)(0xE0 | (U >> 12));
          Utf8[1] = (char)(0x80 | ((U >> 6) & 0x3F));
          Utf8[2] = (char)(0x80 | (U & 0x3F));
          Utf8[3] = 0;
        }
        Out += Utf8;
      }
    }
    return Out;
  }
};

} // end namespace revenant

#endif // REVENANT_CORE_IMAGE_H
