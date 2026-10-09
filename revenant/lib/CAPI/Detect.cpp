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

/// MSVC x64 RTTI: a type descriptor's ".?AV"/".?AU" name must be named
/// by a complete-object locator (signature 1, matching type and self RVAs),
/// and that locator must be referenced by a vftable's prefix slot.
static bool isMsvcRuntimeType(StringRef Name) {
  return Name == ".?AVtype_info@@" || Name.starts_with(".?AVbad_") ||
         Name.starts_with(".?AVexception@std@@") ||
         Name.starts_with(".?AVlogic_error@std@@") ||
         Name.starts_with(".?AVruntime_error@std@@") ||
         Name.starts_with(".?AVlength_error@std@@") ||
         Name.starts_with(".?AVout_of_range@std@@") ||
         Name.starts_with(".?AVinvalid_argument@std@@") ||
         Name.starts_with(".?AVoverflow_error@std@@") ||
         Name.starts_with(".?AVregex_error@std@@") ||
         Name.starts_with(".?AVDNameNode@@") ||
         Name.starts_with(".?AVDNameStatusNode@@") ||
         Name.starts_with(".?AVcharNode@@") ||
         Name.starts_with(".?AVpDNameNode@@") ||
         Name.starts_with(".?AVpairNode@@") ||
         Name.starts_with(".?AVpcharNode@@");
}

static bool hasMsvcRtti(const Image &Img) {
  for (const Section &Names : Img.Sections) {
    if (Names.Data.size() < 17)
      continue;
    StringRef Bytes(reinterpret_cast<const char *>(Names.Data.data()),
                    Names.Data.size());
    size_t Pos = 0;
    while ((Pos = Bytes.find(".?A", Pos)) != StringRef::npos) {
      Va NameVa = Names.SecVa.wrappingAdd(Pos);
      auto Name = Img.readCstr(NameVa, 256);
      if (!Name || !(StringRef(*Name).starts_with(".?AV") ||
                     StringRef(*Name).starts_with(".?AU")) ||
          isMsvcRuntimeType(*Name)) {
        ++Pos;
        continue;
      }
      if (NameVa.Value < Img.Base.Value + 16) {
        ++Pos;
        continue;
      }
      Va TypeDesc = NameVa - 16;
      auto TypeRva = TypeDesc.offsetFrom(Img.Base);
      if (!TypeRva || *TypeRva > UINT32_MAX) {
        ++Pos;
        continue;
      }
      for (const Section &Data : Img.Sections) {
        if (Data.Data.size() < 24)
          continue;
        for (size_t I = 0; I + 24 <= Data.Data.size(); ++I) {
          const uint8_t *P = Data.Data.data() + I;
          if (support::endian::read32le(P) != 1 ||
              support::endian::read32le(P + 12) != *TypeRva)
            continue;
          Va ColVa = Data.SecVa.wrappingAdd(I);
          auto ColRva = ColVa.offsetFrom(Img.Base);
          uint32_t ClassRva = support::endian::read32le(P + 16);
          if (!ColRva || *ColRva > UINT32_MAX ||
              support::endian::read32le(P + 20) != *ColRva ||
              !Img.sectionOf(Img.Base.wrappingAdd(ClassRva)))
            continue;
          for (const Section &Vtables : Img.Sections) {
            if (Vtables.Data.size() < 8)
              continue;
            for (size_t V = 0; V + 8 <= Vtables.Data.size(); ++V)
              if (support::endian::read64le(Vtables.Data.data() + V) ==
                  ColVa.Value)
                return true;
          }
        }
      }
      ++Pos;
    }
  }
  return false;
}

/// User C++ exception tables surviving the link: for each .pdata unwind
/// record with UNW_FLAG_EHANDLER, the language-handler data is either a
/// FuncInfo3 record (magic 0x19930520-0x19930524) or an RVA to a
/// `$cppxdata` table (FuncInfo4 flag-byte form). CRT tables of both
/// shapes exist in every MSVC binary, but only user C++ (throw/catch,
/// ctor/dtor cleanups with try regions) carries a TRY map: FuncInfo3
/// with tryCount != 0, or FuncInfo4 with a TryDisp. SEH scope tables
/// (a small count dword) never qualify.
static uint8_t fh4Length(uint8_t First) {
  static constexpr uint8_t Lengths[16] = {1, 2, 1, 3, 1, 2, 1, 4,
                                          1, 2, 1, 3, 1, 2, 1, 5};
  return Lengths[First & 0x0f];
}

static bool hasCppExceptionTables(const PeFile &Pe) {
  if (!Pe.Img || Pe.Pdata.empty())
    return false;
  const Image &Img = *Pe.Img;
  for (const RuntimeFunction &R : Pe.Pdata) {
    if (R.ChainedTo)
      continue;
    auto Head = Img.readAtMost(R.Unwind, 4);
    if (!Head || Head->size() != 4)
      continue;
    uint8_t Flags = (*Head)[0] >> 3;
    if (Flags & 4) // CHAININFO: no language handler of its own.
      continue;
    if (!(Flags & 1)) // UNW_FLAG_EHANDLER only.
      continue;
    uint64_t Count = (*Head)[2];
    uint64_t DataOff = 4 + ((Count + 1u) & ~1u) * 2u;
    Va Data = R.Unwind.wrappingAdd(DataOff + 4);
    auto First = Img.readU32(Data);
    if (!First)
      continue;
    // Inline FuncInfo3: magic LED, try count at +12.
    if (*First >= 0x19930520 && *First <= 0x19930524) {
      auto TryCount = Img.readU32(Data.wrappingAdd(12));
      auto TryRva = Img.readU32(Data.wrappingAdd(16));
      if (TryCount && *TryCount < 4096 && TryRva && *TryRva)
        return true;
      continue;
    }
    // Otherwise Data holds an RVA to the `$cppxdata` table: FuncInfo4
    // when the target starts with a flag byte (high bit clear, one of
    // the low disp bits set). SEH scope counts are small dwords that
    // never resolve into a mapped table with that shape.
    if (*First < 0x10000)
      continue;
    Va Target = Img.vaOfRva(*First);
    // The target is either magic-led FuncInfo3 (clang output) or
    // flag-byte FuncInfo4 (cl output).
    if (auto Magic = Img.readU32(Target);
        Magic && *Magic >= 0x19930520 && *Magic <= 0x19930524) {
      auto TryCount = Img.readU32(Target.wrappingAdd(12));
      auto TryRva = Img.readU32(Target.wrappingAdd(16));
      if (TryCount && *TryCount < 4096 && TryRva && *TryRva)
        return true;
      continue;
    }
    auto FlagBytes = Img.dataAt(Target, 1);
    if (FlagBytes.empty())
      continue;
    uint8_t H = FlagBytes[0];
    if ((H & 0x80) || !(H & 0x78))
      continue;
    // FuncInfo4 header: optional compressed int, then unwind/try/IP
    // relative dwords. Any TryDisp proves a try/catch region.
    Va P = Target.wrappingAdd(1);
    if (H & 0x04) {
      auto FlagByte = Img.dataAt(P, 1);
      if (FlagByte.empty())
        continue;
      P = P.wrappingAdd(fh4Length(FlagByte[0]));
    }
    if (H & 0x08)
      P = P.wrappingAdd(4);
    if (H & 0x10) {
      auto TryDisp = Img.readU32(P);
      if (TryDisp && *TryDisp)
        return true;
    }
  }
  return false;
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
    scanStrings(Img, [&](StringRef Text) {
      if (Text.starts_with("typeinfo for ") ||
          Text.starts_with("typeinfo name for "))
        Cpp.push_back("GCC RTTI " + Text.str());
    });
    for (const Import &Imp : Pe.Imports) {
      StringRef N = Imp.Name ? StringRef(*Imp.Name) : StringRef();
      std::string Dll = Imp.Dll;
      std::transform(Dll.begin(), Dll.end(), Dll.begin(),
                     [](unsigned char C) { return toLower(C); });
      if (N.starts_with("__CxxFrameHandler") ||
          N.starts_with("__gxx_personality") || N.starts_with("??2") ||
          N.starts_with("??3") || StringRef(Dll).starts_with("msvcp") ||
          StringRef(Dll).starts_with("libstdc++"))
        Cpp.push_back("import " + Imp.Dll + "!" + N.str());
    }
    if (hasCppExceptionTables(Pe))
      Cpp.push_back("MSVC C++ exception tables ($cppxdata FuncInfo)");
    if (hasMsvcRtti(Img))
      Cpp.push_back("MSVC RTTI complete-object locator/vftable");
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
