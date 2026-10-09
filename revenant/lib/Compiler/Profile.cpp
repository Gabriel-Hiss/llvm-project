//==- revenant/lib/Compiler/Profile.cpp - Compiler detection ----*- C++ -*-==//

#include "revenant/Compiler/Profile.h"

#include "llvm/ADT/StringRef.h"

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
                     [&](const Section &S) { return S.Name == Name; });
}

CompilerInfo detect(const PeFile &PE, const std::vector<std::string> &Symbols) {
  CompilerInfo I;
  bool HasMsvc = false, HasLto = false, HasPgo = false, HasRust = false;
  // Rich-header product ids: 0x108/0x109 are LTCG, 0x10b/0x0fe are
  // MSVC PGO, and 0x93 is rustc. Other MSVC product ids identify cl.
  for (const RichEntry &E : PE.RichHeader) {
    switch (E.ProdId) {
    case 0x108: case 0x109: HasMsvc = true; HasLto = true; break;
    case 0x10b: case 0x0fe: HasMsvc = true; HasPgo = true; break;
    case 0x93: HasRust = true; break;
    default: if (E.ProdId >= 0x100 && E.ProdId <= 0x10f) HasMsvc = true;
    }
  }
  if (HasMsvc) I.Evidence.emplace_back("MSVC Rich-header product ids");
  if (HasRust) I.Evidence.emplace_back("rustc Rich-header product id 0x93");

  const bool IsGo = PE.GoBuildId.has_value() || hasSection(PE, ".gosymtab") ||
                    hasSection(PE, ".gopclntab") || hasText(Symbols, "runtime.");
  const bool IsRust = HasRust || hasText(Symbols, "_R") || hasText(Symbols, "_$LT$");
  const bool IsGcc = hasImport(PE, "mingw") || hasImport(PE, "libgcc") ||
                     hasImport(PE, "__gxx_personality_seh0") ||
                     hasSection(PE, ".eh_frame");
  const bool IsClang = hasText(Symbols, "clang version") ||
                       hasSection(PE, ".llvm") || hasSection(PE, ".clang");
  const bool IsCpp = hasText(Symbols, "?AV") || hasText(Symbols, "?AU") ||
                     hasText(Symbols, "_Z") || hasText(Symbols, "__Z");

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
  } else if (HasMsvc || hasImport(PE, "__CxxFrameHandler")) {
    I.Producer = CompilerProducer::Msvc;
    I.Language = IsCpp ? SourceLanguage::Cpp : SourceLanguage::C;
    I.Evidence.emplace_back("MSVC Rich-header or runtime evidence");
  }

  CompilerBuildTraits &T = I.Traits;
  if (HasLto) { T.Lto = true; I.Evidence.emplace_back("Rich-header LTCG product id"); }
  if (HasPgo) { T.Pgo = true; I.Evidence.emplace_back("Rich-header PGO product id"); }
  if (hasText(Symbols, "__security_cookie") ||
      hasImport(PE, "__security_check_cookie") || hasImport(PE, "__stack_chk_fail") ||
      hasImport(PE, "__stack_chk_guard")) {
    T.StackProtection = true;
    I.Evidence.emplace_back("stack-protection runtime marker");
  }
  if (hasSection(PE, ".gfids") || hasSection(PE, ".giats") ||
      hasImport(PE, "guard_check_icall") || hasImport(PE, "guard_dispatch_icall")) {
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
