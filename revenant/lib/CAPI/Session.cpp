//==- lib/CAPI/Session.cpp - session setup and lookups -------------------==//
//
// crates/engine Session::open ported: parse the PE, build the disassembler,
// run analysis, collect symbol names, detect the producer language.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Session.h"

#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include "revenant/Analysis/Analysis.h"
#include "revenant/Analysis/EmbeddedMeta.h"
#include "revenant/Analysis/Resolver.h"
#include "revenant/Analysis/LibMatch.h"
#include "revenant/Core/Instr.h"

#include <queue>
#include <tuple>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "llvm/ADT/StringExtras.h"

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

/// Microseconds since `Start`.
static uint64_t microsSince(std::chrono::steady_clock::time_point Start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - Start)
      .count();
}

Expected<std::unique_ptr<Session>> Session::open(StringRef Path) {
  auto Start = std::chrono::steady_clock::now();
  auto PeOrErr = PeFile::load(Path);
  if (!PeOrErr)
    return PeOrErr.takeError();
  uint64_t LoadMicros = microsSince(Start);
  auto S = create(std::move(*PeOrErr));
  if (!S)
    return S.takeError();
  (*S)->SetupStages.insert((*S)->SetupStages.begin(),
                           {"load", "ok", Path.str(), "", LoadMicros});
  return S;
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
  SymNames.erase(std::unique(SymNames.begin(), SymNames.end()), SymNames.end());

  auto DetectStart = std::chrono::steady_clock::now();
  Compiler = detectProducer(*Pe, SymNames, *Pe->Img);
  SetupStages.push_back(
      {"detect", "ok",
       formatv("{0} via {1}: {2}", langName(Compiler.Language),
               compilerName(Compiler.Producer),
               Compiler.Evidence.empty() ? StringRef("no evidence")
                                         : StringRef(Compiler.Evidence[0]))
           .str(),
       "", microsSince(DetectStart)});
  return Error::success();
}

Error Session::analyze() {
  auto Start = std::chrono::steady_clock::now();
  Prog = revenant::analyze(*Pe, *createX86TargetHooks());
  applyEmbeddedMetadata(Prog, *Pe);
  if (!Prog.Resolver)
    Prog.Resolver = std::make_shared<SessionNames>(Prog);
  OriginalFunctionNames.clear();
  for (const auto &[Start, Rec] : Prog.Functions)
    OriginalFunctionNames.emplace(Start, Rec.Name);
  SetupStages.push_back(
      {"analysis", "ok",
       formatv("RevenantAnalysis: {0} functions", Prog.Functions.size()).str(),
       "", microsSince(Start)});
  return Error::success();
}

void Session::setMetaMode(MetaMode Mode) {
  if (Mode == MetaMode::Embedded) {
    applyEmbeddedMetadata(Prog, *Pe);
    return;
  }
  Prog.Embedded.clear();
  Prog.EmbeddedTypes.clear();
  if (Mode != MetaMode::None)
    return;

  // Imports are needed to keep external calls linked. Other names are
  // optional metadata and must not leak into none-mode output.
  std::map<Va, std::string> AvailableNames;
  if (Prog.Externs)
    for (const auto &[Slot, Ext] : *Prog.Externs)
      AvailableNames.emplace(Slot, Ext.Name);
  for (auto &[Start, Rec] : Prog.Functions) {
    Rec.Name = "sub_" + utohexstr(Start.Value);
    AvailableNames.insert_or_assign(Start, Rec.Name);
  }
  if (Prog.Names)
    *Prog.Names = std::move(AvailableNames);
  Prog.Resolver = makeResolver(Prog.Names, Prog.Externs, nullptr);
  SymNames.clear();
  if (Prog.Externs)
    for (const auto &[Slot, Ext] : *Prog.Externs) {
      (void)Slot;
      SymNames.push_back(Ext.Name);
    }
  llvm::sort(SymNames);
  SymNames.erase(std::unique(SymNames.begin(), SymNames.end()), SymNames.end());
  Pe->CoffSymbols.clear();
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

Expected<FunctionClassification>
Session::classifyFunctions(Lang Out, StringRef LibraryDBPath) const {
  FunctionClassification Result;
  if (!LibraryDBPath.empty()) {
    auto DBOrErr = LibraryDatabase::load(LibraryDBPath);
    if (!DBOrErr)
      return DBOrErr.takeError();
    Result.HasLibraryDB = true;
    for (const LibraryMatch &Match : matchLibraryFunctions(Prog, *DBOrErr)) {
      Result.LibraryStarts.insert(Match.Start);
      if (Match.FunctionIndex < DBOrErr->Functions.size())
        Result.LibraryNames[Match.Start] =
            DBOrErr->Functions[Match.FunctionIndex].Name;
    }
  }

  auto functionName = [&](Va Start, const FuncRecord &Rec) -> StringRef {
    auto It = OriginalFunctionNames.find(Start);
    return It == OriginalFunctionNames.end() ? StringRef(Rec.Name)
                                              : StringRef(It->second);
  };
  if (Out != Lang::Rust) {
    for (const auto &[Start, Rec] : Prog.Functions) {
      StringRef Name = functionName(Start, Rec);
      if (Name == "main" || Name == "wmain" || Name == "WinMain" ||
          Name == "wWinMain") {
        Result.UserEntry = Start;
        break;
      }
    }
  }
  if (!Result.UserEntry && Out != Lang::Rust) {
    for (const auto &[At, Name] : Result.LibraryNames) {
      if (Name != "invoke_main" &&
          Name.find("__tmainCRTStartup") == std::string::npos)
        continue;
      auto Invoke = materialize(At);
      if (!Invoke) {
        consumeError(Invoke.takeError());
        continue;
      }
      for (const Instr &I : Invoke->Instrs)
        if (I.Flow == FlowKind::Branch && I.HasTarget &&
            Prog.Functions.count(I.Target) &&
            !Result.LibraryStarts.count(I.Target)) {
          Result.UserEntry = I.Target;
          // Traced through the narrow handoff: the entry is hosted `main`.
          Result.UserEntryIsNarrowMain = (Name == "invoke_main");
          break;
        }
      if (Result.UserEntry)
        break;
    }
  }
  // MinGW's CRT passes argc, argv, and envp to main from stored startup
  // values. This register setup is stronger evidence than the largest
  // reachable tail target, which can be an unrelated runtime helper.
  auto isMoveFrom = [&](const Instr &I, StringRef Dest, StringRef Source,
                        bool MemorySource) {
    std::string Text = Dis->format(I);
    StringRef Assembly = StringRef(Text).trim();
    size_t Separator = Assembly.find_first_of(" \t");
    if (Separator == StringRef::npos)
      return false;
    StringRef Opcode = Assembly.take_front(Separator).trim();
    StringRef Operands = Assembly.drop_front(Separator).trim();
    if (Opcode != "mov")
      return false;
    auto [ActualDest, ActualSource] = Operands.split(',');
    if (ActualDest.trim() != Dest)
      return false;
    ActualSource = ActualSource.trim();
    return MemorySource ? ActualSource.starts_with(Source)
                        : ActualSource == Source;
  };
  auto isMingwMainCall = [&](const std::vector<Instr> &Instrs,
                             size_t CallIndex) {
    if (CallIndex < 5)
      return false;
    return isMoveFrom(Instrs[CallIndex - 5], "rcx", "qword ptr [", true) &&
           isMoveFrom(Instrs[CallIndex - 4], "rdx", "qword ptr [", true) &&
           isMoveFrom(Instrs[CallIndex - 3], "eax", "dword ptr [", true) &&
           isMoveFrom(Instrs[CallIndex - 2], "r8", "rcx", false) &&
           isMoveFrom(Instrs[CallIndex - 1], "ecx", "eax", false);
  };

  // GCC's __main hook walks an indexed constructor table through an indirect
  // call. A one-time guard wrapper around that dispatcher is a runtime hook
  // too, not user code.
  auto isCtorTableDispatcher = [&](const Function &F) {
    bool HasIndirectCall = false;
    bool HasIndexedLoad = false;
    bool HasBackwardLoop = false;
    for (const Instr &I : F.Instrs) {
      HasIndirectCall |= I.Flow == FlowKind::Call && !I.HasTarget;
      if (I.Flow == FlowKind::CondBranch && I.HasTarget &&
          I.Target.Value <= I.Addr.Value)
        HasBackwardLoop = true;
      std::string Text = Dis->format(I);
      StringRef Assembly(Text);
      HasIndexedLoad |= Assembly.contains("qword ptr [") &&
                        Assembly.contains("*");
    }
    return HasIndirectCall && HasIndexedLoad && HasBackwardLoop;
  };
  auto callsCtorTableDispatcher = [&](Va Start) {
    std::queue<std::pair<Va, unsigned>> Work;
    std::set<Va> Seen;
    Work.push({Start, 0});
    while (!Work.empty()) {
      auto [At, Depth] = Work.front();
      Work.pop();
      if (!Seen.insert(At).second)
        continue;
      auto FOrErr = materialize(At);
      if (!FOrErr) {
        consumeError(FOrErr.takeError());
        continue;
      }
      if (isCtorTableDispatcher(*FOrErr))
        return true;
      if (Depth == 1)
        continue;
      for (const Instr &I : FOrErr->Instrs)
        if (I.Flow == FlowKind::Call && I.HasTarget &&
            Prog.Functions.count(I.Target))
          Work.push({I.Target, Depth + 1});
    }
    return false;
  };

  std::set<Va> MinGWRuntimeHooks;
  auto recordMinGWRuntimeHook = [&] {
    auto MainOrErr = materialize(*Result.UserEntry);
    if (!MainOrErr) {
      consumeError(MainOrErr.takeError());
      return;
    }
    for (const Instr &I : MainOrErr->Instrs) {
      if (I.Flow != FlowKind::Call || !I.HasTarget ||
          !Prog.Functions.count(I.Target))
        continue;
      if (callsCtorTableDispatcher(I.Target)) {
        MinGWRuntimeHooks.insert(I.Target);
        break;
      }
    }
  };

  if (!Result.UserEntry) {
    auto PeEntry = Prog.Functions.upper_bound(Pe->EntryPoint);
    if (PeEntry != Prog.Functions.begin()) {
      --PeEntry;
      std::queue<std::pair<Va, unsigned>> Pending;
      std::set<Va> Seen;
      std::optional<std::pair<size_t, Va>> PointerEntry;
      std::optional<std::tuple<unsigned, size_t, Va>> TailEntry;
      Pending.push({PeEntry->first, 0});
      while (!Pending.empty()) {
        auto [At, Depth] = Pending.front();
        Pending.pop();
        if (!Seen.insert(At).second || Depth > 6)
          continue;
        auto FunctionOrErr = materialize(At);
        if (!FunctionOrErr) {
          consumeError(FunctionOrErr.takeError());
          continue;
        }
        std::optional<Va> PendingCodePointer;

        for (size_t InstrIndex = 0;
             InstrIndex < FunctionOrErr->Instrs.size(); ++InstrIndex) {
          const Instr &I = FunctionOrErr->Instrs[InstrIndex];
          if (Out != Lang::Rust && I.Flow == FlowKind::Call && I.HasTarget &&
              Prog.Functions.count(I.Target) &&
              !Result.LibraryStarts.count(I.Target) &&
              isMingwMainCall(FunctionOrErr->Instrs, InstrIndex)) {
            Result.UserEntry = I.Target;
            break;
          }
          if (I.HasMemTarget && Prog.Functions.count(I.MemTarget))
            PendingCodePointer = I.MemTarget;
          if (!I.HasTarget || !Prog.Functions.count(I.Target))
            continue;
          if (Out == Lang::Rust && I.Flow == FlowKind::Call &&
              PendingCodePointer &&
              !Result.LibraryStarts.count(*PendingCodePointer)) {
            size_t Size =
                Prog.Functions.find(*PendingCodePointer)->second.InstrRvas.size();
            if (!PointerEntry || Size > PointerEntry->first)
              PointerEntry = {Size, *PendingCodePointer};
            PendingCodePointer.reset();
          }
          if (I.Flow == FlowKind::Branch && I.Target != At &&
              !Result.LibraryStarts.count(I.Target)) {
            auto Candidate = Prog.Functions.find(I.Target);
            if (Candidate != Prog.Functions.end() &&
                Candidate->second.InstrRvas.size() > 2 &&
                (!TailEntry || Depth > std::get<0>(*TailEntry) ||
                 (Depth == std::get<0>(*TailEntry) &&
                  Candidate->second.InstrRvas.size() >
                      std::get<1>(*TailEntry))))
              TailEntry =
                  {Depth, Candidate->second.InstrRvas.size(), I.Target};
            if (Depth < 6)
              Pending.push({I.Target, Depth + 1});
          }
          if (I.Flow == FlowKind::Call)
            Pending.push({I.Target, Depth + 1});
        }
      }
      if (Out == Lang::Rust && PointerEntry)
        Result.UserEntry = PointerEntry->second;
      else if (!Result.UserEntry && TailEntry)
        Result.UserEntry = std::get<2>(*TailEntry);
    }
  }
  if (Out != Lang::Rust && Result.UserEntry)
    recordMinGWRuntimeHook();


  auto Entry = Prog.Functions.upper_bound(Pe->EntryPoint);
  if (Entry != Prog.Functions.begin()) {
    --Entry;
    std::queue<Va> Pending;
    Pending.push(Entry->first);
    while (!Pending.empty()) {
      Va At = Pending.front();
      Pending.pop();
      if ((Result.UserEntry && At == *Result.UserEntry) ||
          !Result.Startup.insert(At).second)
        continue;
      auto FOrErr = materialize(At);
      if (!FOrErr) {
        consumeError(FOrErr.takeError());
        continue;
      }
      for (const Instr &I : FOrErr->Instrs)
        if (I.HasTarget && Prog.Functions.count(I.Target) &&
            (I.Flow == FlowKind::Call || I.Flow == FlowKind::Branch) &&
            (!Result.UserEntry || I.Target != *Result.UserEntry))
          Pending.push(I.Target);
    }
  }

  if (!Result.UserEntry)
    return Result;

  std::queue<Va> UserPending;
  UserPending.push(*Result.UserEntry);
  while (!UserPending.empty()) {
    Va At = UserPending.front();
    UserPending.pop();
    if (MinGWRuntimeHooks.count(At) ||
        !Result.UserReachable.insert(At).second)
      continue;
    auto FOrErr = materialize(At);
    if (!FOrErr) {
      consumeError(FOrErr.takeError());
      continue;
    }
    for (const Instr &I : FOrErr->Instrs) {
      // Calls into libdb-matched functions stay library: user code calling
      // the CRT must not pull the whole static CRT into the user set.
      // Branch targets are still followed: GCC .text.unlikely cold parts
      // are reached by jumps from the hot body, and the matcher can match
      // them (same code as a library function), but they are user code.
      if (I.HasTarget && Prog.Functions.count(I.Target)) {
        if (I.Flow == FlowKind::Call && Result.LibraryStarts.count(I.Target))
          continue;
        UserPending.push(I.Target);
      }
      if (I.HasMemTarget && Prog.Functions.count(I.MemTarget) &&
          !Result.LibraryStarts.count(I.MemTarget))
        UserPending.push(I.MemTarget);
      for (unsigned OpIdx = 0; OpIdx < I.Inst.getNumOperands(); ++OpIdx) {
        const MCOperand &Operand = I.Inst.getOperand(OpIdx);
        if (!Operand.isImm() || Operand.getImm() < 0)
          continue;
        Va Target(static_cast<uint64_t>(Operand.getImm()));
        if (Prog.Functions.count(Target) &&
            !Result.LibraryStarts.count(Target))
          UserPending.push(Target);
      }
    }
  }
  return Result;
}

Expected<Function> Session::materialize(Va Start) const {
  auto It = Prog.Functions.find(Start);
  if (It == Prog.Functions.end())
    return createStringError(
        formatv("{0} is not a function start", format_hex(Start.Value, 0))
            .str()
            .c_str());
  return revenant::materialize(Prog, It->second);
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

std::vector<std::pair<uint32_t, Va>> Session::firstVaPerLine(
    const std::vector<std::pair<uint32_t, std::vector<Va>>> &In) {
  std::vector<std::pair<uint32_t, Va>> Out;
  Out.reserve(In.size());
  for (auto &[Line, Vas] : In)
    if (!Vas.empty())
      Out.push_back({Line, *std::min_element(Vas.begin(), Vas.end())});
  return Out;
}

} // namespace revenant
