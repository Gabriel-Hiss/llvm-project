//==- revenant/lib/Analysis/Resolver.cpp - Symbol resolver -------------==//
//
// See revenant/Analysis/Resolver.h. Ports crates/analysis/src/resolver.rs:
//
// - `name_at`: exact hit in names, then externs, then a short `aIDENT`
//   label from the referenced string (first 32 chars, uppercased
//   alphanumerics plus `_` and non-leading `.`).
// - `name_for`: exact hit first, then the nearest preceding name with a
//   small offset (`< 0x10000`) as `name+0x<off>`, then the string label for
//   AddrOf/Data refs (unreachable in practice — `name_at` already covers
//   strings — kept to match the reference).
// - `decorateLine` substitutes those names into the Intel line: the printed
//   branch/call target address becomes the name, `rip + 0x<disp>` becomes
//   the referenced name. It assumes the printer renders PC-relative targets
//   from the next IP (llvm-objdump convention), so the address token in the
//   line equals the absolute target VA.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Resolver.h"

#include <cctype>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"

namespace revenant {
namespace {

constexpr uint64_t MaxInteriorOffset = 0x10000;
constexpr size_t MaxLabelChars = 32;

// Width of the UTF-8 sequence starting at `Lead` (inputs are valid UTF-8,
// produced by strict decoders).
size_t utf8Width(uint8_t Lead) {
  if ((Lead & 0x80) == 0)
    return 1;
  if ((Lead & 0xe0) == 0xc0)
    return 2;
  if ((Lead & 0xf0) == 0xe0)
    return 3;
  return 4;
}

// First `Max` Unicode scalars of a valid UTF-8 string.
std::string takeChars(const std::string &S, size_t Max) {
  std::string Out;
  size_t I = 0, N = 0;
  while (I < S.size() && N < Max) {
    size_t W = utf8Width((uint8_t)S[I]);
    Out.append(S, I, W);
    I += W;
    ++N;
  }
  return Out;
}

// Rust `escape_ident`: keep ASCII alphanumerics (uppercased), `_`, and `.`
// past the first char; drop everything else. Byte-wise iteration is exact
// here: every kept char is ASCII, and dropped bytes are dropped chars.
std::string escapeIdent(llvm::StringRef S) {
  std::string Out;
  Out.reserve(S.size());
  for (size_t I = 0; I < S.size(); ++I) {
    char C = S[I];
    bool Keep = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
                (C >= '0' && C <= '9') || C == '_' || (I > 0 && C == '.');
    if (Keep)
      Out.push_back((char)std::toupper((unsigned char)C));
  }
  return Out;
}

std::string stringLabel(const std::string &S) {
  return "a" + escapeIdent(takeChars(S, MaxLabelChars));
}

// `0x<lowercase hex>` as the Intel printer renders an absolute target VA
// (C hex style, minimal digits).
std::string addrToken(Va Addr) {
  std::string S;
  llvm::raw_string_ostream(S) << llvm::format_hex(Addr.Value, 0);
  return S;
}

// Replace the first standalone occurrence of `Token` (not followed by a hex
// digit, so `0x1400` never matches inside `0x14000`) with `Name`.
bool substituteToken(std::string &Line, const std::string &Token,
                     const std::string &Name) {
  size_t At = 0;
  while ((At = Line.find(Token, At)) != std::string::npos) {
    size_t After = At + Token.size();
    if (After >= Line.size() || !std::isxdigit((unsigned char)Line[After])) {
      Line.replace(At, Token.size(), Name);
      return true;
    }
    At = After;
  }
  return false;
}

} // end anonymous namespace

Resolver::Resolver(std::shared_ptr<const std::map<Va, std::string>> Names,
                   std::shared_ptr<const std::map<Va, Extern>> Externs,
                   std::shared_ptr<const std::map<Va, std::string>> Strings)
    : Names(std::move(Names)), Externs(std::move(Externs)),
      Strings(std::move(Strings)) {}

std::optional<std::string> Resolver::nameAt(Va Addr) const {
  if (Names)
    if (auto It = Names->find(Addr); It != Names->end())
      return It->second;
  if (Externs)
    if (auto It = Externs->find(Addr); It != Externs->end())
      return It->second.Name;
  if (Strings)
    if (auto It = Strings->find(Addr); It != Strings->end())
      return stringLabel(It->second);
  return std::nullopt;
}

std::optional<std::string> Resolver::nameFor(Va Addr, RefKind Kind) const {
  if (auto N = nameAt(Addr))
    return N;
  if (Names) {
    auto It = Names->upper_bound(Addr);
    if (It != Names->begin()) {
      --It;
      uint64_t Off = Addr - It->first;
      if (Off > 0 && Off < MaxInteriorOffset) {
        std::string Out = It->second + "+0x" + llvm::utohexstr(Off, true);
        return Out;
      }
    }
  }
  if ((Kind == RefKind::AddrOf || Kind == RefKind::Data) && Strings)
    if (auto It = Strings->find(Addr); It != Strings->end())
      return stringLabel(It->second);
  return std::nullopt;
}
std::string Resolver::decorateLine(
    const Instr &I, llvm::StringRef Line,
    llvm::ArrayRef<std::pair<Va, RefKind>> Refs) const {
  std::string Out = Line.str();
  for (auto [Target, Kind] : Refs) {
    auto Name = nameFor(Target, Kind);
    if (!Name)
      continue;
    if (Kind == RefKind::Call || Kind == RefKind::Jump) {
      substituteToken(Out, addrToken(Target), *Name);
    } else {
      // RIP-relative displacement as printed inside `[rip + 0x<disp>]`: the
      // printer renders the absolute target, so work back to the displacement
      // from the next IP.
      uint64_t Next = I.nextVa().Value;
      if (Target.Value < Next) {
        uint64_t Disp = Next - Target.Value;
        substituteToken(Out, "rip - 0x" + llvm::utohexstr(Disp, true), *Name);
      } else {
        uint64_t Disp = Target.Value - Next;
        if (Disp == 0)
          continue; // `[rip]`: no displacement token to replace.
        substituteToken(Out, "rip + 0x" + llvm::utohexstr(Disp, true), *Name);
      }
    }
  }
  return Out;
}

SharedNames
makeResolver(std::shared_ptr<const std::map<Va, std::string>> Names,
             std::shared_ptr<const std::map<Va, Extern>> Externs,
             std::shared_ptr<const std::map<Va, std::string>> Strings) {
  return std::make_shared<Resolver>(std::move(Names), std::move(Externs),
                                    std::move(Strings));
}

} // end namespace revenant
