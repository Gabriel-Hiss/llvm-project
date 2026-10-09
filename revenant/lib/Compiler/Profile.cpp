//==- revenant/lib/Compiler/Profile.cpp - Compiler detection ----*- C++ -*-==//

#include "revenant/Compiler/Profile.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

using namespace revenant;

namespace {

bool hasText(const std::vector<std::string> &Names, llvm::StringRef Needle) {
  return std::any_of(Names.begin(), Names.end(), [&](const std::string &S) {
    return llvm::StringRef(S).contains_insensitive(Needle);
  });
}

bool hasImport(const PeFile &PE, llvm::StringRef Needle) {
  for (const Import &I : PE.Imports)
    if ((I.Name && llvm::StringRef(*I.Name).contains_insensitive(Needle)) ||
        llvm::StringRef(I.Dll).contains_insensitive(Needle))
      return true;
  return false;
}

bool hasSection(const PeFile &PE, llvm::StringRef Name) {
  return std::any_of(PE.Img->Sections.begin(), PE.Img->Sections.end(),
                     [&](const Section &S) {
                       // COFF truncates and uppercases long names, so
                       // compare case-insensitively.
                       return llvm::StringRef(S.Name).equals_insensitive(Name);
                     });
}

bool hasSectionPrefix(const PeFile &PE, llvm::StringRef Prefix) {
  return std::any_of(PE.Img->Sections.begin(), PE.Img->Sections.end(),
                     [&](const Section &S) {
                       return llvm::StringRef(S.Name)
                           .starts_with_insensitive(Prefix);
                     });
}

bool hasImageText(const Image &Img, llvm::StringRef Needle) {
  for (const Section &S : Img.Sections)
    if (!S.Data.empty() &&
        llvm::StringRef(reinterpret_cast<const char *>(S.Data.data()),
                        S.Data.size()).contains_insensitive(Needle))
      return true;
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
static std::optional<uint32_t> fh4Length(uint8_t First) {
  static constexpr uint8_t Lengths[16] = {1, 2, 1, 3, 1, 2, 1, 4,
                                          1, 2, 1, 3, 1, 2, 1, 5};
  return Lengths[First & 0x0f];
}

static bool hasCppExceptionTables(const PeFile &PE) {
  if (!PE.Img || PE.Pdata.empty())
    return false;
  const Image &Img = *PE.Img;
  for (const RuntimeFunction &R : PE.Pdata) {
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
      auto N = fh4Length(Img.dataAt(P, 1).empty() ? 0 : Img.dataAt(P, 1)[0]);
      if (!N)
        continue;
      P = P.wrappingAdd(*N);
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

static bool isMsvcRuntimeType(llvm::StringRef Name) {
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

static bool hasMsvcRtti(const PeFile &PE) {
  if (!PE.Img)
    return false;
  const Image &Img = *PE.Img;
  for (const Section &Names : Img.Sections) {
    if (Names.Data.size() < 17)
      continue;
    llvm::StringRef Bytes(reinterpret_cast<const char *>(Names.Data.data()),
                          Names.Data.size());
    size_t Pos = 0;
    while ((Pos = Bytes.find(".?A", Pos)) != llvm::StringRef::npos) {
      Va NameVa = Names.SecVa.wrappingAdd(Pos);
      auto Name = Img.readCstr(NameVa, 256);
      if (!Name || !(llvm::StringRef(*Name).starts_with(".?AV") ||
                     llvm::StringRef(*Name).starts_with(".?AU")) ||
          isMsvcRuntimeType(*Name)) {
        ++Pos;
        continue;
      }
      if (NameVa.Value < Img.Base.Value + 16) {
        ++Pos;
        continue;
      }
      auto TypeRva = (NameVa - 16).offsetFrom(Img.Base);
      if (!TypeRva || *TypeRva > UINT32_MAX) {
        ++Pos;
        continue;
      }
      for (const Section &Data : Img.Sections) {
        for (size_t I = 0; I + 24 <= Data.Data.size(); ++I) {
          const uint8_t *P = Data.Data.data() + I;
          if (llvm::support::endian::read32le(P) != 1 ||
              llvm::support::endian::read32le(P + 12) != *TypeRva)
            continue;
          Va ColVa = Data.SecVa.wrappingAdd(I);
          auto ColRva = ColVa.offsetFrom(Img.Base);
          uint32_t ClassRva = llvm::support::endian::read32le(P + 16);
          if (!ColRva || *ColRva > UINT32_MAX ||
              llvm::support::endian::read32le(P + 20) != *ColRva ||
              !Img.sectionOf(Img.Base.wrappingAdd(ClassRva)))
            continue;
          for (const Section &Vtables : Img.Sections)
            for (size_t V = 0; V + 8 <= Vtables.Data.size(); ++V)
              if (llvm::support::endian::read64le(Vtables.Data.data() + V) ==
                  ColVa.Value)
                return true;
        }
      }
      ++Pos;
    }
  }
  return false;
}


bool hasSymbolPrefix(const std::vector<std::string> &Names,
                     llvm::StringRef Prefix) {
  return std::any_of(Names.begin(), Names.end(), [&](const std::string &S) {
    return llvm::StringRef(S).starts_with(Prefix);
  });
}

CompilerInfo detect(const PeFile &PE, const std::vector<std::string> &Symbols) {
  CompilerInfo I;
  bool HasMsvc = false, HasLto = false, HasPgo = false, HasRust = false;
  // Rich-header product ids, validated against the parity corpus:
  // 0x93 is rustc; 0x101-0x105/0xfd/0x100/0x102 are MSVC toolchains.
  // At the link build (highest build number) 0x104/0x105 mean a plain
  // link, 0x108/0x109 mean /GL+LTCG, 0x10d means LTCG+PGO, and 0xff is
  // the rust-cargo self-link marker. 0xfd at a tool build is a stock
  // cl/c2 object marker, not evidence of anything on its own.
  for (const RichEntry &E : PE.RichHeader) {
    switch (E.ProdId) {
    case 0x108: case 0x109: HasMsvc = true; HasLto = true; break;
    case 0x10b: case 0x0fe: HasMsvc = true; HasPgo = true; break;
    case 0x10d: HasMsvc = true; HasLto = true; HasPgo = true; break;
    case 0x93: HasRust = true; break;
    case 0xff: HasRust = true; break;
    default:
      if ((E.ProdId >= 0x101 && E.ProdId <= 0x105) || E.ProdId == 0xfd ||
          E.ProdId == 0x100 || E.ProdId == 0x102)
        HasMsvc = true;
    }
  }
  if (HasMsvc) I.Evidence.emplace_back("MSVC Rich-header product ids");
  if (HasRust) I.Evidence.emplace_back("rustc Rich-header product id 0x93");

  const bool IsGo = PE.GoBuildId.has_value() || hasSection(PE, ".gosymtab") ||
                    hasSection(PE, ".gopclntab") || hasText(Symbols, "runtime.") ||
                    hasImageText(*PE.Img, "gopclntab") ||
                    hasImageText(*PE.Img, "gosymtab") ||
                    hasImageText(*PE.Img, "go1.") ||
                    hasImageText(*PE.Img, "Go build ID");
  const bool IsRust = HasRust || hasSymbolPrefix(Symbols, "_R") ||
                     hasText(Symbols, "_$LT$") ||
                     hasImageText(*PE.Img, "/rustc/") ||
                     hasImageText(*PE.Img, "rustc version");
  const bool IsGcc = hasImport(PE, "mingw") || hasImport(PE, "libgcc") ||
                     hasImport(PE, "__gxx_personality_seh0") ||
                     hasImport(PE, "libstdc++") ||
                     hasSection(PE, ".eh_frame");
  // clang-cl leaves no Rich header: folded builtins (memcpy_/memset_) and
  // the CRT guard folding tables (_RDATA, _guard_c/_guard_d) are its
  // stable fingerprint on this corpus. Section-name comparison ignores
  // case: COFF truncates and uppercases long names.
  const bool IsClang = hasImageText(*PE.Img, "clang version") ||
                       hasText(Symbols, "clang version") ||
                       hasSection(PE, ".llvm") || hasSection(PE, ".clang") ||
                       (hasSectionPrefix(PE, "memcpy_") &&
                        hasSectionPrefix(PE, "memset_")) ||
                       (hasSectionPrefix(PE, "_RDATA") &&
                        hasSectionPrefix(PE, "_guard_"));
  // C++ exception tables live in .xdata whether or not any import, export
  // or string names them: FuncInfo magic reachable from an EHANDLER
  // unwind record proves user C++ (throw/ctor/dtor) code.
  const bool HasCppEH = hasCppExceptionTables(PE);
  const bool HasBinaryRtti = hasMsvcRtti(PE) ||
                             hasImageText(*PE.Img, "typeinfo for ") ||
                             hasImageText(*PE.Img, "typeinfo name for ");
  const bool HasCppSymbol = hasText(Symbols, "?AV") ||
                            hasText(Symbols, "?AU") ||
                            hasSymbolPrefix(Symbols, "_Z");
  const bool HasCppImport =
      hasImport(PE, "__CxxFrameHandler") || hasImport(PE, "__gxx_personality") ||
      hasImport(PE, "??2") || hasImport(PE, "??3") ||
      hasImport(PE, "msvcp") || hasImport(PE, "libstdc++");
  const bool IsCpp = HasCppEH || HasBinaryRtti || HasCppSymbol || HasCppImport;

  if (IsGo) {
    I.Producer = CompilerProducer::Go; I.Language = SourceLanguage::Go;
    I.Evidence.emplace_back("Go build-id, runtime symbol, or Go section");
  } else if (IsRust) {
    I.Producer = CompilerProducer::Rustc; I.Language = SourceLanguage::Rust;
    I.Evidence.emplace_back("rustc symbol or Rich-header evidence");
  } else if (IsGcc) {
    I.Producer = CompilerProducer::Gcc;
    I.Language = IsCpp ? SourceLanguage::Cpp : SourceLanguage::C;
    I.Evidence.emplace_back("MinGW/GCC runtime import or section");
  } else if (IsClang) {
    I.Producer = CompilerProducer::Clang;
    I.Language = IsCpp ? SourceLanguage::Cpp : SourceLanguage::C;
    I.Evidence.emplace_back("LLVM/Clang symbol or section");
  } else if (HasMsvc || hasImport(PE, "__CxxFrameHandler") ||
             hasImport(PE, "VCRUNTIME") || hasImport(PE, "MSVCRT") ||
             hasSection(PE, ".fptable")) {
    I.Producer = CompilerProducer::Msvc;
    I.Language = IsCpp ? SourceLanguage::Cpp : SourceLanguage::C;
    I.Evidence.emplace_back("MSVC Rich-header or runtime evidence");
  }
  if (IsCpp)
    I.Evidence.emplace_back(
        llvm::formatv("C++ evidence: EH={0}, RTTI={1}, symbol={2}, import={3}",
                      HasCppEH, HasBinaryRtti, HasCppSymbol, HasCppImport)
            .str());

  CompilerBuildTraits &T = I.Traits;
  if (HasLto) { T.Lto = true; I.Evidence.emplace_back("Rich-header LTCG product id"); }
  if (HasPgo) { T.Pgo = true; I.Evidence.emplace_back("Rich-header PGO product id"); }
  // LLVM PGO (clang -fprofile-use) keeps .lprfc/.lprfn sections.
  if (!HasPgo && (hasSectionPrefix(PE, ".lprfc") || hasSectionPrefix(PE, ".lprfn"))) {
    T.Pgo = true;
    I.Evidence.emplace_back("LLVM PGO counter sections (.lprfc/.lprfn)");
  }
  if (hasText(Symbols, "__security_cookie") ||
      hasImport(PE, "__security_check_cookie") || hasImport(PE, "__stack_chk_fail") ||
      hasImport(PE, "__stack_chk_guard") ||
      hasImageText(*PE.Img, "__security_check_cookie")) {
    T.StackProtection = true;
    I.Evidence.emplace_back("stack-protection runtime marker");
  }
  if (hasSection(PE, ".gfids") || hasSection(PE, ".giats") ||
      hasImport(PE, "guard_check_icall") || hasImport(PE, "guard_dispatch_icall") ||
      hasImageText(*PE.Img, ".gfids") || hasImageText(*PE.Img, ".giats")) {
    T.ControlFlowGuard = true;
    I.Evidence.emplace_back("CFG guard table or dispatch import");
  }
  T.HotColdSplit = std::any_of(PE.Pdata.begin(), PE.Pdata.end(),
                               [](const RuntimeFunction &F) { return F.ChainedTo.has_value(); });
  if (T.HotColdSplit)
    I.Evidence.emplace_back("chained unwind entries indicate split code");
  if (HasLto || HasPgo) {
    T.Optimization = OptimizationLevel::O2;
    I.Evidence.emplace_back("LTCG/PGO implies an optimized build (estimated O2)");
  }
  if (I.Evidence.empty()) I.Evidence.emplace_back("no positive producer evidence");
  return I;
}

class ProducerProfile final : public CompilerProfile {
  CompilerProducer Producer;
  CompilerBuildTraits Build;
public:
  ProducerProfile(CompilerProducer P, CompilerBuildTraits T) : Producer(P), Build(T) {}
  CompilerProducer producer() const override { return Producer; }
  CompilerBuildTraits traits() const override { return Build; }
  ColdChunkConvention coldChunkConvention() const override {
    switch (Producer) {
    case CompilerProducer::Msvc: return Build.HotColdSplit ? ColdChunkConvention::ChainedUnwind : ColdChunkConvention::None;
    case CompilerProducer::Gcc: return ColdChunkConvention::ColdSuffix;
    case CompilerProducer::Clang: return ColdChunkConvention::MfsSplit;
    default: return ColdChunkConvention::None;
    }
  }
  EhModel ehModel() const override {
    switch (Producer) {
    case CompilerProducer::Msvc: return EhModel::MsvcFuncInfo;
    case CompilerProducer::Clang: return EhModel::Seh;
    case CompilerProducer::Gcc: return EhModel::Lsda;
    case CompilerProducer::Rustc: return EhModel::RustCleanup;
    default: return EhModel::Unknown;
    }
  }
};

} // namespace

bool CompilerProfile::isColdChunkName(llvm::StringRef Name) const {
  return coldChunkConvention() == ColdChunkConvention::ColdSuffix &&
         Name.ends_with_insensitive(".cold");
}

CompilerInfo revenant::detectCompiler(const PeFile &PE,
                                      const std::vector<std::string> &Symbols) {
  return detect(PE, Symbols);
}

std::unique_ptr<CompilerProfile>
revenant::profileFor(const PeFile &PE, const std::vector<std::string> &Symbols) {
  CompilerInfo Info = detect(PE, Symbols);
  if (Info.Producer == CompilerProducer::Unknown)
    return std::make_unique<GenericLlvmProfile>(Info.Traits);
  return std::make_unique<ProducerProfile>(Info.Producer, Info.Traits);
}

const char *revenant::producerName(CompilerProducer P) {
  switch (P) {
  case CompilerProducer::Unknown: return "unknown";
  case CompilerProducer::Msvc: return "msvc";
  case CompilerProducer::Clang: return "clang";
  case CompilerProducer::Gcc: return "gcc";
  case CompilerProducer::Rustc: return "rustc";
  case CompilerProducer::Go: return "go";
  }
  return "unknown";
}

const char *revenant::languageName(SourceLanguage L) {
  switch (L) {
  case SourceLanguage::C: return "c";
  case SourceLanguage::Cpp: return "cpp";
  case SourceLanguage::Rust: return "rust";
  case SourceLanguage::Go: return "go";
  case SourceLanguage::Unknown: return "unknown";
  }
  return "unknown";
}
