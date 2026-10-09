//==- lib/CAPI/Detect.cpp - producer language detection -------------------==//
//
// Port of crates/detect minus the PDB compiland pass: rustc evidence first
// (symbols and embedded strings), then C++ evidence (mangled names, RTTI,
// CRT imports), MinGW imports for the compiler field, and a C default.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Detect.h"

#include "revenant/Object/PeFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
using namespace llvm;

namespace revenant {

StringRef langName(Lang L) {
  switch (L) {
  case Lang::C:
    return "c";
  case Lang::Cpp:
    return "cpp";
  case Lang::Rust:
    return "rust";
  }
  llvm_unreachable("bad Lang");
}

StringRef compilerName(Compiler C) {
  switch (C) {
  case Compiler::Msvc:
    return "msvc";
  case Compiler::ClangCl:
    return "clang-cl";
  case Compiler::Rustc:
    return "rustc";
  case Compiler::MinGwGcc:
    return "mingw-gcc";
  case Compiler::Unknown:
    return "unknown";
  }
  llvm_unreachable("bad Compiler");
}

/// MSVC-mangled `?name@namespace@@...` spells `::` as `@@` after the first
/// '@'. crates/detect demangles and checks for "::" in the result; reading
/// the mangled form directly is equivalent for this check.
static bool isMsvcCppName(StringRef S) {
  if (!S.starts_with("?"))
    return false;
  size_t First = S.find('@');
  if (First == StringRef::npos)
    return false;
  return S.find("@@", First + 1) != StringRef::npos;
}

/// Rust symbols without a demangler: v0 scheme `_R...`, legacy `_ZN...E`
/// names carry a `17h<hex>` crate-hash element (and end in `E`). GCC-style
/// `_ZN` C++ names have no `17h` element.
static bool isRustSymbolName(StringRef S) {
  if (S.starts_with("_R"))
    return true;
  return S.starts_with("_ZN") && S.contains("17h") && S.ends_with("E");
}

/// Rust crate/detect string scan: ASCII runs of >= 6 chars in non-executable
/// sections are fed to `hit`.
static void scanStrings(const Image &Img, function_ref<void(StringRef)> Hit) {
  for (const Section &S : Img.Sections) {
    if (!S.Flags.Read || S.Flags.Execute)
      continue;
    SmallString<256> Cur;
    auto Flush = [&] {
      if (Cur.size() >= 6)
        Hit(Cur);
      Cur.clear();
    };
    for (uint8_t B : S.Data) {
      if (B >= 0x20 && B < 0x7f)
        Cur.push_back((char)B);
      else
        Flush();
    }
    Flush();
  }
}

CompilerInfo detectProducer(const PeFile &Pe,
                            ArrayRef<std::string> SymbolNames,
                            const Image &Img) {
  CompilerInfo Info;
  std::optional<Lang> CompLang;
  std::optional<Compiler> CompCompiler;

  // ---- rustc evidence (strings + symbols). crates/detect pass 2; the PDB
  // pass above it has no LLVM-side equivalent yet.
  std::vector<std::string> RustcHits;
  for (StringRef S : SymbolNames) {
    if (isRustSymbolName(S))
      RustcHits.push_back(("Rust-mangled symbol " + S).str());
  }
  scanStrings(Img, [&](StringRef Text) {
    if (size_t Pos = Text.find("/rustc/"); Pos != StringRef::npos) {
      // `/rustc/<40-hex-commit>/` is the embedded rust-src prefix.
      if (llvm::all_of(Text.substr(Pos + 7).take_front(40),
                       [](char C) { return isHexDigit(C); }))
        RustcHits.push_back("rustc path \"" + Text.str() + "\"");
    } else if ((Text.ends_with(".rs") || Text.contains(".rs:")) &&
               (Text.contains(":\\") || Text.starts_with("/")) &&
               Text.find_first_of(" \t\r\n") == StringRef::npos) {
      RustcHits.push_back("rust source path \"" + Text.str() + "\"");
    } else if (Text.contains("rustc version") &&
               llvm::any_of(Text, [](char C) { return isDigit(C); })) {
      std::string T = Text.trim().str();
      if (llvm::find(RustcHits, T) == RustcHits.end())
        RustcHits.push_back(std::move(T));
    }
  });
  llvm::sort(RustcHits);
  RustcHits.erase(std::unique(RustcHits.begin(), RustcHits.end()),
                  RustcHits.end());
  if (!RustcHits.empty()) {
    CompLang = Lang::Rust;
    CompCompiler = Compiler::Rustc;
    Info.Evidence.push_back(formatv("{0} rustc evidence hits",
                                    RustcHits.size())
                                .str());
    for (size_t I = 0; I < std::min<size_t>(4, RustcHits.size()); ++I)
      Info.Evidence.push_back(RustcHits[I]);
    // `rustc version 1.xx.y` strings carry the toolchain version.
    for (StringRef H : RustcHits) {
      size_t Pos = H.find("rustc version ");
      if (Pos == StringRef::npos)
        continue;
      StringRef Rest = H.drop_front(Pos + strlen("rustc version "));
      size_t End = Rest.find_if_not(
          [](char C) { return isAlnum(C) || C == '.'; });
      Info.Version = Rest.take_front(End).str();
      break;
    }
  }

  // ---- C++ evidence (crates/detect pass 3). Only when rustc did not win.
  if (!CompLang) {
    std::vector<std::string> Cpp;
    for (StringRef S : SymbolNames) {
      if (isMsvcCppName(S))
        Cpp.push_back(("C++ mangled " + S).str());
      else if (S.starts_with(".?AV") || S.starts_with(".?AU"))
        Cpp.push_back(("RTTI " + S).str());
    }
    for (const Import &Imp : Pe.Imports) {
      StringRef N = Imp.Name ? StringRef(*Imp.Name) : StringRef();
      std::string Dll = Imp.Dll;
      std::transform(Dll.begin(), Dll.end(), Dll.begin(),
                     [](unsigned char C) { return toLower(C); });
      if (N.starts_with("__CxxFrameHandler") ||
          StringRef(Dll).starts_with("msvcp") ||
          StringRef(Dll).starts_with("vcruntime"))
        Cpp.push_back("import " + Imp.Dll + "!" + N.str());
    }
    llvm::sort(Cpp);
    Cpp.erase(std::unique(Cpp.begin(), Cpp.end()), Cpp.end());
    if (!Cpp.empty()) {
      CompLang = Lang::Cpp;
      Info.Evidence.push_back(
          formatv("{0} C++ evidence hits", Cpp.size()).str());
      for (size_t I = 0; I < std::min<size_t>(4, Cpp.size()); ++I)
        Info.Evidence.push_back(Cpp[I]);
    }
  }

  // MinGW: mingw-specific imports or GCC-style init symbols.
  bool Mingw = llvm::any_of(Pe.Imports, [](const Import &I) {
    std::string Dll = I.Dll;
    std::transform(Dll.begin(), Dll.end(), Dll.begin(),
                   [](unsigned char C) { return toLower(C); });
    return Dll.find("mingw") != std::string::npos ||
           Dll.find("libgcc") != std::string::npos;
  });
  if (Mingw) {
    CompCompiler = Compiler::MinGwGcc;
    Info.Evidence.push_back("MinGW/libgcc imports");
    if (!CompLang)
      CompLang = Lang::C;
  }

  Info.Language = CompLang.value_or(Lang::C);
  Info.Producer = CompCompiler.value_or(Compiler::Unknown);
  if (Info.Evidence.empty())
    Info.Evidence.push_back("no positive evidence; defaulting to C");
  return Info;
}

} // namespace revenant
