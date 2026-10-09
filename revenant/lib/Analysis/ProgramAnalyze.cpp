#include "AnalysisImpl.h"
#include "Orphan.h"

#include "revenant/Analysis/Noreturn.h"
#include "revenant/Analysis/Padding.h"
#include "revenant/Analysis/Resolver.h"
#include "revenant/Analysis/Strings.h"
#include "revenant/Analysis/Xrefs.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace revenant {
namespace {

using Range = std::pair<Va, Va>;
using PdataMap = std::map<Va, Range>;
using PdataGroups = std::map<Va, std::vector<Range>>;

std::optional<Va> pdataPrimary(const PdataMap &Pdata, Va Address) {
  auto It = Pdata.upper_bound(Address);
  if (It == Pdata.begin())
    return std::nullopt;
  --It;
  return Address < It->second.second ? std::optional<Va>(It->second.first)
                                     : std::nullopt;
}

std::vector<Range> mergeRanges(std::vector<Range> Ranges) {
  llvm::sort(Ranges);
  std::vector<Range> Out;
  Out.reserve(Ranges.size());
  for (const Range &R : Ranges) {
    if (!Out.empty() && R.first <= Out.back().second)
      Out.back().second = std::max(Out.back().second, R.second);
    else
      Out.push_back(R);
  }
  return Out;
}

void addClaim(std::map<Va, Va> &Claims, Va Start, Va End) {
  auto It = Claims.lower_bound(Start);
  if (It != Claims.begin()) {
    auto Prev = std::prev(It);
    if (Prev->second >= Start) {
      Start = Prev->first;
      End = std::max(End, Prev->second);
      It = Claims.erase(Prev);
    }
  }
  while (It != Claims.end() && It->first <= End) {
    End = std::max(End, It->second);
    It = Claims.erase(It);
  }
  Claims.emplace(Start, End);
}

bool claimsOverlap(const std::map<Va, Va> &Claims, const Range &R) {
  auto It = Claims.lower_bound(R.first);
  if (It != Claims.begin()) {
    auto Prev = std::prev(It);
    if (Prev->second > R.first)
      return true;
  }
  return It != Claims.end() && It->first < R.second;
}

std::vector<Range> claimVector(const std::map<Va, Va> &Claims) {
  std::vector<Range> Out;
  Out.reserve(Claims.size());
  for (const auto &R : Claims)
    Out.push_back(R);
  return Out;
}

std::string defaultName(Va Start) {
  return ("sub_" + llvm::utohexstr(Start.Value));
}

void setFunctionName(Function &F, const std::map<Va, std::string> &Names) {
  auto It = Names.find(F.Start);
  F.Name = It == Names.end() ? defaultName(F.Start) : It->second;
}

bool validStart(const Image &Img, const PdataMap &Pdata, Va Start) {
  if (!Img.isExecutable(Start) || !Img.isBacked(Start) ||
      Img.inInlineData(Start))
    return false;
  if (auto Primary = pdataPrimary(Pdata, Start); Primary && *Primary != Start)
    return false;
  return true;
}

void appendFunctionClaims(std::map<Va, Va> &Claims, const Function &F) {
  for (const Range &R : F.Chunks)
    addClaim(Claims, R.first, R.second);
}

} // namespace

Program analyze(const PeFile &Pe, const TargetHooks &Hooks) {
  Program P;
  P.Img = Pe.Img;
  P.Names = std::make_shared<std::map<Va, std::string>>();
  P.Externs = std::make_shared<std::map<Va, Extern>>();
  P.Strings = std::make_shared<std::map<Va, std::string>>();
  if (!P.Img)
    return P;

  for (const Import &I : Pe.Imports) {
    std::string Dll = I.Dll;
    if (llvm::StringRef(Dll).ends_with(".dll"))
      Dll.resize(Dll.size() - 4);
    std::string Name = Dll + "!";
    if (I.Name)
      Name += *I.Name;
    else
      Name += "#" + std::to_string(I.Ordinal.value_or(0));
    (*P.Externs)[I.IatVa] = {I.IatVa, Name, isNoreturnImport(Name)};
    (*P.Names)[I.IatVa] = Name;
  }
  for (const Export &E : Pe.Exports)
    (*P.Names)[E.ExportVa] = E.Name;
  (*P.Names)[Pe.EntryPoint] = "entry";

  auto ExpectedDis = Disasm::create(*P.Img);
  if (!ExpectedDis) {
    llvm::consumeError(ExpectedDis.takeError());
    return P;
  }
  Disasm Dis = std::move(*ExpectedDis);

  PdataMap Pdata;
  PdataGroups Groups;
  for (const RuntimeFunction &R : Pe.Pdata) {
    const Va Primary = R.ChainedTo.value_or(R.Begin);
    Pdata[R.Begin] = {Primary, R.End};
    Groups[Primary].emplace_back(R.Begin, R.End);
  }
  for (auto &[Primary, Ranges] : Groups) {
    (void)Primary;
    llvm::sort(Ranges);
  }

  std::set<Va> Seeds = analysis_detail::collectStrongStarts(Pe, *P.Img);
  std::set<Va> Starts;
  std::set<Va> Weak = analysis_detail::collectInitialWeakCandidates(Pe);
  std::set<Va> SharedRipRefs;
  std::vector<Va> Worklist(Seeds.begin(), Seeds.end());
  auto IsNoreturn = [&](Va Address) {
    auto It = P.Externs->find(Address);
    if (It != P.Externs->end() && It->second.Noreturn)
      return true;
    auto Name = P.Names->find(Address);
    return Name != P.Names->end() && nameIsNoreturn(Name->second);
  };
  auto enqueueStrong = [&](Va Candidate, std::set<Va> &TargetStarts,
                           std::vector<Va> &Pending) {
    if (auto Head = fillerHead(Dis, Candidate))
      Candidate = *Head;
    if (TargetStarts.count(Candidate) || !validStart(*P.Img, Pdata, Candidate))
      return;
    TargetStarts.insert(Candidate);
    Pending.push_back(Candidate);
  };

  // Strong discovery collects direct call targets before weak pointers are
  // accepted. The sorted worklist makes output independent of thread order.
  while (!Worklist.empty()) {
    std::vector<Va> Calls;
    for (Va Start : Worklist) {
      if (!P.Img->isExecutable(Start))
        continue;
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Start, Starts, Pdata, Groups, SharedRipRefs,
          [&](Va Target) {
            auto It = P.Externs->find(Target);
            return It != P.Externs->end() && It->second.Noreturn;
          },
          true);
      Calls.insert(Calls.end(), Result.F.CallTargets.begin(),
                   Result.F.CallTargets.end());
      Weak.insert(Result.LeaTargets.begin(), Result.LeaTargets.end());
      Weak.insert(Result.ForeignTargets.begin(), Result.ForeignTargets.end());
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
    }
    Worklist.clear();
    for (Va Seed : Seeds)
      enqueueStrong(Seed, Starts, Worklist);
    for (Va Call : Calls)
      enqueueStrong(Call, Starts, Worklist);
  }

  std::map<Va, Function> Built;
  for (unsigned Iteration = 0; Iteration < 8; ++Iteration) {
    Built.clear();
    std::vector<Va> Calls;
    for (Va Start : Starts) {
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Start, Starts, Pdata, Groups, SharedRipRefs,
          IsNoreturn);
      setFunctionName(Result.F, *P.Names);
      Calls.insert(Calls.end(), Result.F.CallTargets.begin(),
                   Result.F.CallTargets.end());
      Weak.insert(Result.LeaTargets.begin(), Result.LeaTargets.end());
      Weak.insert(Result.ForeignTargets.begin(), Result.ForeignTargets.end());
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
      Built.insert_or_assign(Start, std::move(Result.F));
    }
    std::vector<Va> NewStarts;
    for (Va Call : Calls)
      enqueueStrong(Call, Starts, NewStarts);
    if (NewStarts.empty() || Iteration == 7)
      break;
  }

  std::set<Va> Pending = std::move(Weak);
  std::set<Va> AcceptedWeak;
  std::set<Va> Rejected;
  std::map<Va, Va> Claims;
  for (const auto &[Start, F] : Built) {
    (void)Start;
    appendFunctionClaims(Claims, F);
  }
  std::vector<Range> DataClaims;
  for (const auto &[Start, F] : Built) {
    (void)Start;
    DataClaims.insert(DataClaims.end(), F.DataRanges.begin(), F.DataRanges.end());
  }
  DataClaims = mergeRanges(std::move(DataClaims));
  auto processWeak = [&] {
  while (!Pending.empty()) {
    std::set<Va> Batch;
    for (unsigned I = 0; I < 4 && !Pending.empty(); ++I) {
      Va Candidate = *Pending.begin();
      Pending.erase(Pending.begin());
      if (auto Head = fillerHead(Dis, Candidate)) {
        if (*Head < Candidate) {
          Range Span{*Head, Candidate};
          if (claimsOverlap(Claims, Span) ||
              std::any_of(DataClaims.begin(), DataClaims.end(),
                          [&](const Range &R) {
                            return R.first < Span.second && R.second > Span.first;
                          }))
            continue;
          Candidate = *Head;
        }
      }
      if (Built.count(Candidate) || Rejected.count(Candidate) ||
          analysis_detail::rejectWeakCandidate(*P.Img, Dis, Hooks, Pdata,
                                                claimVector(Claims), Candidate)) {
        Rejected.insert(Candidate);
        continue;
      }
      if (std::any_of(DataClaims.begin(), DataClaims.end(),
                      [&](const Range &R) {
                        return Candidate >= R.first && Candidate < R.second;
                      }))
        continue;
      Batch.insert(Candidate);
    }
    const std::map<Va, Va> ClaimsBefore = Claims;
    std::vector<Va> Overlapping;
    for (Va Candidate : Batch) {
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Candidate, Starts, Pdata, Groups, SharedRipRefs,
          IsNoreturn, false, &Claims);
      if (Result.F.Instrs.empty() ||
          analysis_detail::rejectWeakFunction(*P.Img, Hooks, Result.F)) {
        Rejected.insert(Candidate);
        continue;
      }
      if (std::any_of(Result.F.Chunks.begin(), Result.F.Chunks.end(),
                      [&](const Range &R) { return claimsOverlap(Claims, R); })) {
        Overlapping.push_back(Candidate);
        continue;
      }
      setFunctionName(Result.F, *P.Names);
      Pending.insert(Result.LeaTargets.begin(), Result.LeaTargets.end());
      Pending.insert(Result.ForeignTargets.begin(), Result.ForeignTargets.end());
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
      appendFunctionClaims(Claims, Result.F);
      DataClaims.insert(DataClaims.end(), Result.F.DataRanges.begin(),
                        Result.F.DataRanges.end());
      DataClaims = mergeRanges(std::move(DataClaims));
      Built.insert_or_assign(Candidate, std::move(Result.F));
      AcceptedWeak.insert(Candidate);
    }
    if (Claims != ClaimsBefore)
      Pending.insert(Overlapping.begin(), Overlapping.end());
    else
      Rejected.insert(Overlapping.begin(), Overlapping.end());
  }
  };
  processWeak();
  for (unsigned Promotion = 0; Promotion < 8; ++Promotion) {
    std::vector<Va> NewStarts;
    for (const auto &[Start, F] : Built) {
      (void)Start;
      for (Va Call : F.CallTargets)
        enqueueStrong(Call, Starts, NewStarts);
    }
    if (NewStarts.empty())
      break;

    std::map<Va, Function> WeakFunctions;
    for (Va Start : AcceptedWeak) {
      auto Node = Built.extract(Start);
      if (!Node.empty())
        WeakFunctions.insert(std::move(Node));
    }
    Built.clear();
    Claims.clear();
    std::vector<Va> Calls;
    for (Va Start : Starts) {
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Start, Starts, Pdata, Groups, SharedRipRefs,
          IsNoreturn);
      setFunctionName(Result.F, *P.Names);
      Calls.insert(Calls.end(), Result.F.CallTargets.begin(),
                   Result.F.CallTargets.end());
      Pending.insert(Result.LeaTargets.begin(), Result.LeaTargets.end());
      Pending.insert(Result.ForeignTargets.begin(), Result.ForeignTargets.end());
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
      appendFunctionClaims(Claims, Result.F);
      Built.insert_or_assign(Start, std::move(Result.F));
    }
    std::vector<Va> NewlyDiscovered;
    for (Va Call : Calls)
      enqueueStrong(Call, Starts, NewlyDiscovered);

    for (auto &[Start, F] : WeakFunctions) {
      const bool Claimed = Starts.count(Start) ||
          std::any_of(F.Chunks.begin(), F.Chunks.end(),
                      [&](const Range &R) { return claimsOverlap(Claims, R); });
      if (Claimed) {
        AcceptedWeak.erase(Start);
        continue;
      }
      appendFunctionClaims(Claims, F);
      Built.insert_or_assign(Start, std::move(F));
    }
    if (NewlyDiscovered.empty())
      break;
  }

  processWeak();
  DataClaims.clear();
  for (const auto &[Start, F] : Built) {
    (void)Start;
    DataClaims.insert(DataClaims.end(), F.DataRanges.begin(), F.DataRanges.end());
  }
  DataClaims = mergeRanges(std::move(DataClaims));
  std::vector<Function> Functions;
  Functions.reserve(Built.size());
  for (auto &[Start, F] : Built) {
    (void)Start;
    Functions.push_back(std::move(F));
  }
  llvm::sort(Functions, [](const Function &A, const Function &B) {
    return A.Start < B.Start;
  });
  std::set<Va> StrongStarts = Starts;
  std::map<Va, Va> OrphanClaims = Claims;
  for (unsigned Pass = 0; Pass < 2; ++Pass) {
    const std::vector<Range> ProbeRanges =
        analysis_detail::orphanProbeRanges(Pe, Functions);
    for (const Range &Probe : ProbeRanges) {
      Va Cursor = Probe.first;
      while (Cursor < Probe.second) {
        const uint64_t Pad = padLen(Dis, *P.Img, Cursor);
        if (Pad) {
          Cursor = Cursor.wrappingAdd(Pad);
          continue;
        }
        if (auto Head = fillerHead(Dis, Cursor); Head && *Head < Cursor) {
          Cursor = Cursor.wrappingAdd(1);
          continue;
        }
        if (auto InlineEnd = P.Img->inlineDataEnd(Cursor)) {
          Cursor = std::min(*InlineEnd, Probe.second);
          continue;
        }
        if (analysis_detail::rejectWeakCandidate(
                *P.Img, Dis, Hooks, Pdata, claimVector(OrphanClaims), Cursor)) {
          Cursor = Cursor.wrappingAdd(1);
          continue;
        }
        if (std::any_of(DataClaims.begin(), DataClaims.end(),
                        [&](const Range &R) {
                          return Cursor >= R.first && Cursor < R.second;
                        })) {
          Cursor = Cursor.wrappingAdd(1);
          continue;
        }
        if (!P.Img->isBacked(Cursor)) {
          Cursor = Cursor.wrappingAdd(1);
          continue;
        }
        auto Containing = OrphanClaims.upper_bound(Cursor);
        if (Containing != OrphanClaims.begin()) {
          --Containing;
          if (Cursor < Containing->second) {
            Cursor = std::min(Containing->second, Probe.second);
            continue;
          }
        }
        auto NextClaim = OrphanClaims.upper_bound(Cursor);
        const Va BoundEnd = NextClaim == OrphanClaims.end()
                                ? Probe.second
                                : std::min(NextClaim->first, Probe.second);
        const std::vector<Range> Bound{{Cursor, BoundEnd}};
        analysis_detail::BuildResult Result = analysis_detail::buildFunction(
            *P.Img, Dis, Hooks, Cursor, Starts, Pdata, Groups, SharedRipRefs,
            IsNoreturn, false, &OrphanClaims, Bound);
        SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
        if (analysis_detail::orphanFunctionOk(*P.Img, Result.F, BoundEnd,
                                              Hooks) &&
            !analysis_detail::rejectWeakFunction(*P.Img, Hooks, Result.F)) {
          setFunctionName(Result.F, *P.Names);
          Result.F.Orphan = true;
          Starts.insert(Cursor);
          for (const Range &R : Result.F.Chunks)
            addClaim(OrphanClaims, R.first, R.second);
          for (const Range &R : Result.F.DataRanges)
            addClaim(OrphanClaims, R.first, R.second);
          DataClaims.insert(DataClaims.end(), Result.F.DataRanges.begin(),
                            Result.F.DataRanges.end());
          DataClaims = mergeRanges(std::move(DataClaims));
          Va End = Result.F.Chunks.empty() ? Cursor
                                           : Result.F.Chunks.back().second;
          Functions.push_back(std::move(Result.F));
          Cursor = std::max(End, Cursor.wrappingAdd(1));
        } else {
          Cursor = Va((Cursor.Value + 16) & ~uint64_t(15));
        }
      }
    }
    llvm::sort(Functions, [](const Function &A, const Function &B) {
      return A.Start < B.Start;
    });
  }
  for (unsigned Iteration = 0; Iteration < 8; ++Iteration) {
    std::vector<Va> NewStarts;
    for (const Function &F : Functions)
      for (Va Call : F.CallTargets)
        enqueueStrong(Call, StrongStarts, NewStarts);
    if (NewStarts.empty())
      break;
    Starts.insert(NewStarts.begin(), NewStarts.end());

    std::vector<Function> OtherFunctions;
    for (Function &F : Functions)
      if (!StrongStarts.count(F.Start))
        OtherFunctions.push_back(std::move(F));
    std::vector<Function> Rebuilt;
    Rebuilt.reserve(StrongStarts.size());
    std::map<Va, Va> StrongClaims;
    for (Va Start : StrongStarts) {
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Start, Starts, Pdata, Groups, SharedRipRefs,
          IsNoreturn);
      setFunctionName(Result.F, *P.Names);
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
      for (const Range &R : Result.F.Chunks)
        addClaim(StrongClaims, R.first, R.second);
      Rebuilt.push_back(std::move(Result.F));
    }
    std::vector<Function> Survivors;
    for (Function &F : OtherFunctions) {
      const bool AllCovered =
          !F.Chunks.empty() &&
          std::all_of(F.Chunks.begin(), F.Chunks.end(), [&](const Range &R) {
            auto It = StrongClaims.upper_bound(R.first);
            if (It == StrongClaims.begin())
              return false;
            --It;
            return It->first <= R.first && It->second >= R.second;
          });
      if (!AllCovered)
        Survivors.push_back(std::move(F));
    }
    Rebuilt.insert(Rebuilt.end(),
                   std::make_move_iterator(Survivors.begin()),
                   std::make_move_iterator(Survivors.end()));
    Functions = std::move(Rebuilt);
    llvm::sort(Functions, [](const Function &A, const Function &B) {
      return A.Start < B.Start;
    });
  }

  std::set<Va> NoreturnStarts =
      propagateNoreturn(Functions, *P.Externs, *P.Names, Hooks);
  for (unsigned Pass = 0; Pass < 4; ++Pass) {
    auto IsNoreturn2 = [&](Va Target) {
      return IsNoreturn(Target) || NoreturnStarts.count(Target) != 0;
    };
    std::set<Va> Dirty;
    for (const Function &F : Functions) {
      for (const Instr &I : F.Instrs) {
        auto Target = callSiteTarget(I);
        if (!Target || !IsNoreturn2(*Target))
          continue;
        const BasicBlock *B = F.blockContaining(I.Addr);
        if (!B || B->EndKind == BlockEnd::Noreturn ||
            B->InstrsEnd == B->InstrsBegin)
          continue;
        if (I.Addr != F.Instrs[B->InstrsEnd - 1].Addr)
          Dirty.insert(F.Start);
      }
    }
    if (Dirty.empty())
      break;
    for (Va Start : Dirty) {
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Start, Starts, Pdata, Groups, SharedRipRefs,
          IsNoreturn2);
      setFunctionName(Result.F, *P.Names);
      auto It = std::find_if(Functions.begin(), Functions.end(),
                             [&](const Function &F) {
                               return F.Start == Start;
                             });
      if (It != Functions.end())
        *It = std::move(Result.F);
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
    }
    NoreturnStarts =
        propagateNoreturn(Functions, *P.Externs, *P.Names, Hooks);
  }
  llvm::sort(Functions, [](const Function &A, const Function &B) {
    return A.Start < B.Start;
  });
  for (Function &F : Functions) {
    setFunctionName(F, *P.Names);
    P.Names->insert_or_assign(F.Start, F.Name);
  }
  P.Xr = collectXrefs(*P.Img, Functions, Dis);
  *P.Strings = collectStrings(*P.Img, P.Xr);

  std::vector<Range> AllChunks;
  std::vector<Range> AllData;
  for (const Function &F : Functions) {
    AllChunks.insert(AllChunks.end(), F.Chunks.begin(), F.Chunks.end());
    AllData.insert(AllData.end(), F.DataRanges.begin(), F.DataRanges.end());
  }
  llvm::sort(AllData);
  AllData.erase(std::unique(AllData.begin(), AllData.end()), AllData.end());
  P.Padding = decodePadding(Pe, AllChunks, AllData);
  for (Function &F : Functions)
    insertFunction(P, std::move(F));
  P.Resolver = makeResolver(P.Names, P.Externs, P.Strings);
  return P;
}

} // namespace revenant
