//==- lib/CAPI/Session.cpp - session setup and lookups -------------------==//
//
// crates/engine Session::open ported: parse the PE, build the disassembler,
// run analysis (real library when present, .pdata fallback otherwise),
// collect symbol names, detect the producer language.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Session.h"

#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#if __has_include("revenant/Analysis/Analysis.h")
#include "revenant/Analysis/Analysis.h"
#define RV_HAVE_ANALYSIS 1
#endif
#include <algorithm>
#include <map>
#include <set>

#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;

namespace revenant {

namespace {
/// Names -> disasm operand resolution: externs by IAT slot, then the
/// program name table, then referenced strings.
class SessionNames : public NameProvider {
public:
  SessionNames(const Program &P) : P(P) {}
  std::optional<std::string> nameFor(Va Addr, RefKind Kind) const override {
    if (P.Externs) {
      if (auto It = P.Externs->find(Addr); It != P.Externs->end())
        return It->second.Name;
    }
    if (P.Names) {
      if (auto It = P.Names->find(Addr); It != P.Names->end())
        return It->second;
    }
    // Inside a function: name+off.
    auto Fit = P.Functions.upper_bound(Addr);
    if (Fit != P.Functions.begin()) {
      --Fit;
      if (Addr >= Fit->second.Start && Addr < Fit->second.End) {
        uint64_t Off = Addr.Value - Fit->second.Start.Value;
        if (Off)
          return formatv("{0}+0x{1:x}", Fit->second.Name, Off).str();
        return Fit->second.Name;
      }
    }
    if (P.Strings && (Kind == RefKind::Data || Kind == RefKind::AddrOf)) {
      if (auto It = P.Strings->find(Addr); It != P.Strings->end())
        return formatv("\"{0}\"", It->second).str();
    }
    return std::nullopt;
  }

private:
  const Program &P;
};
} // namespace

Session::Session() = default;
Session::~Session() = default;
Session::Session(Session &&) = default;
Session &Session::operator=(Session &&) = default;

Expected<std::unique_ptr<Session>> Session::open(StringRef Path) {
  auto PeOrErr = PeFile::load(Path);
  if (!PeOrErr)
    return PeOrErr.takeError();
  return create(std::move(*PeOrErr));
}

Expected<std::unique_ptr<Session>> Session::create(PeFile Pe) {
  auto S = std::unique_ptr<Session>(new Session());
  S->Pe = std::make_unique<PeFile>(std::move(Pe));
  if (auto E = S->setup())
    return std::move(E);
  return S;
}

Error Session::setup() {
  auto DOrErr = Disasm::create(*Pe->Img);
  if (!DOrErr)
    return DOrErr.takeError();
  Dis = std::make_unique<Disasm>(std::move(*DOrErr));

  if (auto E = analyze())
    return E;

  // Symbol names for detection: imports, exports, function names.
  for (const Import &I : Pe->Imports) {
    if (I.Name)
      SymNames.push_back(*I.Name);
    if (auto It = Prog.Externs->find(I.IatVa); It != Prog.Externs->end())
      SymNames.push_back(It->second.Name);
  }
  for (const Export &E : Pe->Exports)
    SymNames.push_back(E.Name);
  for (const auto &[Start, Rec] : Prog.Functions)
    SymNames.push_back(Rec.Name);
  llvm::sort(SymNames);
  SymNames.erase(std::unique(SymNames.begin(), SymNames.end()),
                 SymNames.end());

  Compiler = detectProducer(*Pe, SymNames, *Pe->Img);
  SetupStages.push_back(
      {"detect", "ok",
       formatv("{0} via {1}: {2}", langName(Compiler.Language),
               compilerName(Compiler.Producer),
               Compiler.Evidence.empty() ? StringRef("no evidence")
                                         : StringRef(Compiler.Evidence[0]))
           .str(),
       ""});
  return Error::success();
}

Error Session::analyze() {
#if defined(RV_HAVE_ANALYSIS)
  // Real analysis (Swe2Discovery): .pdata + discovery + jump tables + the
  // x86 TargetHooks shapes.
  Prog = revenant::analyze(*Pe, *createX86TargetHooks());
  if (!Prog.Resolver)
    Prog.Resolver = std::make_shared<SessionNames>(Prog);
  RealAnalysis = true;
  SetupStages.push_back(
      {"analysis", "ok",
       formatv("RevenantAnalysis: {0} functions", Prog.Functions.size())
           .str(),
       ""});
  return Error::success();
#else
  auto ProgOrErr = fallbackAnalyze(*Pe, *Dis);
  if (!ProgOrErr)
    return ProgOrErr.takeError();
  Prog = std::move(*ProgOrErr);
  Prog.Resolver = std::make_shared<SessionNames>(Prog);
  SetupStages.push_back(
      {"analysis", "stub",
       formatv("pdata-seeded fallback CFG: {0} functions",
               Prog.Functions.size())
           .str(),
       ""});
  return Error::success();
#endif
}

std::string Session::nameFor(Va Addr) const {
  if (Prog.Externs)
    if (auto It = Prog.Externs->find(Addr); It != Prog.Externs->end())
      return It->second.Name;
  if (Prog.Names)
    if (auto It = Prog.Names->find(Addr); It != Prog.Names->end())
      return It->second;
  if (Prog.Functions.count(Addr))
    return Prog.Functions.at(Addr).Name;
  return formatv("sub_{0:x-8}", Addr.Value).str();
}

std::optional<Va> Session::functionByName(StringRef Name) const {
  for (const auto &[Start, Rec] : Prog.Functions)
    if (Rec.Name == Name)
      return Start;
  if (Prog.Names)
    for (const auto &[Addr, N] : *Prog.Names)
      if (N == Name && Prog.Functions.count(Addr))
        return Addr;
  // `sub_<hex>` names resolve even for records not yet named.
  if (Name.starts_with("sub_"))
    if (uint64_t V; !Name.drop_front(4).getAsInteger(16, V))
      if (Prog.Functions.count(Va(V)))
        return Va(V);
  return std::nullopt;
}

Expected<Function> Session::materialize(Va Start) const {
  auto It = Prog.Functions.find(Start);
  if (It == Prog.Functions.end())
    return createStringError(
        formatv("{0} is not a function start", format_hex(Start.Value, 0))
            .str()
            .c_str());
  return materializeFunction(*Pe, *Dis, It->second);
}

Expected<std::string> Session::disasmFunction(Va Start) const {
  auto FOrErr = materialize(Start);
  if (!FOrErr)
    return FOrErr.takeError();
  const Function &F = *FOrErr;
  std::string Out;
  raw_string_ostream OS(Out);
  for (const Instr &I : F.Instrs) {
    ArrayRef<uint8_t> Bytes = Pe->Img->dataAt(I.Addr, I.Size);
    std::string Hex;
    for (uint8_t B : Bytes)
      Hex += formatv("{0:x-2}", B).str() + " ";
    OS << format_hex(I.Addr.Value, 0) << ": " << left_justify(Hex, 32)
       << Dis->format(I, Prog.Resolver) << "\n";
  }
  return Out;
}

std::vector<std::pair<uint32_t, Va>>
Session::firstVaPerLine(
    const std::vector<std::pair<uint32_t, std::vector<Va>>> &In) {
  std::vector<std::pair<uint32_t, Va>> Out;
  Out.reserve(In.size());
  for (auto &[Line, Vas] : In)
    if (!Vas.empty())
      Out.push_back({Line, *std::min_element(Vas.begin(), Vas.end())});
  return Out;
}

} // namespace revenant
