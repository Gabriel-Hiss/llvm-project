//==-- revenant/Core/Va.h - Virtual addresses ----------------------*- C++ -*-==//
//
// Mirrors crates/core/src/lib.rs `Va`: an absolute virtual address in the
// loaded image (image base included).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CORE_VA_H
#define REVENANT_CORE_VA_H

#include <cstdint>
#include <optional>

#include "llvm/Support/Format.h"
#include "llvm/Support/FormatProviders.h"
#include "llvm/Support/raw_ostream.h"

namespace revenant {

/// A virtual address in the loaded image (absolute, includes image base).
struct Va {
  uint64_t Value = 0;

  constexpr Va() = default;
  explicit constexpr Va(uint64_t V) : Value(V) {}

  constexpr bool operator==(Va O) const { return Value == O.Value; }
  constexpr bool operator!=(Va O) const { return Value != O.Value; }
  constexpr bool operator<(Va O) const { return Value < O.Value; }
  constexpr bool operator<=(Va O) const { return Value <= O.Value; }
  constexpr bool operator>(Va O) const { return Value > O.Value; }
  constexpr bool operator>=(Va O) const { return Value >= O.Value; }

  /// Wrapping `+` (Rust `wrapping_add`): VAs may sit near u64::MAX.
  constexpr Va wrappingAdd(uint64_t N) const { return Va(Value + N); }
  /// `self - base` when `self >= base`.
  std::optional<uint64_t> offsetFrom(Va Base) const {
    if (Value >= Base.Value)
      return Value - Base.Value;
    return std::nullopt;
  }
};

inline Va operator+(Va A, uint64_t N) { return Va(A.Value + N); }
inline Va operator-(Va A, uint64_t N) { return Va(A.Value - N); }
inline uint64_t operator-(Va A, Va B) { return A.Value - B.Value; }

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, Va V) {
  OS << llvm::format_hex(V.Value, 0);
  return OS;
}

/// A half-open address range `[start, end)` (Rust `VaRange = Range<Va>`).
struct VaRange {
  Va Start;
  Va End;

  bool contains(Va V) const { return V >= Start && V < End; }
  uint64_t size() const { return End - Start; }
};

/// A single RVA (`va - image.base`), kept distinct from `Va`.
using Rva = uint32_t;

} // end namespace revenant

template <> struct llvm::format_provider<revenant::Va> {
  static void format(const revenant::Va &V, llvm::raw_ostream &OS,
                     llvm::StringRef /*Style*/) {
    llvm::format_provider<uint64_t>::format(V.Value, OS, "x");
  }
};

#endif // REVENANT_CORE_VA_H
