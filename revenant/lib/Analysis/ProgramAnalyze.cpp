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
#include <optional>
#include <tuple>
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

bool claimedAt(const std::map<Va, Va> &Claims, Va V) {
  auto It = Claims.upper_bound(V);
  if (It == Claims.begin())
    return false;
  --It;
  return V < It->second;
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
  P.GuardCFCheckPtr = Pe.GuardCFCheckPtr;
  P.GuardCFDispatchPtr = Pe.GuardCFDispatchPtr;
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
  for (const Export &E : Pe.Exports) {
    (*P.Names)[E.ExportVa] = E.Name;
    if (!E.Forwarder && P.Img && P.Img->isExecutable(E.ExportVa))
      P.Exports.insert(E.ExportVa);
  }
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
    std::vector<Va> RipBatch;
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
      RipBatch.insert(RipBatch.end(), Result.RipRefs.begin(),
                      Result.RipRefs.end());
    }
    SharedRipRefs.insert(RipBatch.begin(), RipBatch.end());
    Worklist.clear();
    for (Va Seed : Seeds)
      enqueueStrong(Seed, Starts, Worklist);
    for (Va Call : Calls)
      enqueueStrong(Call, Starts, Worklist);
  }

  // ---- full descent, weak candidates, orphan sweep ------------------------
  // Mirrors the `analyze_with` iteration in crates/analysis/src/lib.rs:
  // strong starts build first, weak candidates (relocation targets, lea and
  // foreign edge targets) are accepted only into unclaimed bytes, the orphan
  // sweep runs once, and call targets found along the way promote to strong
  // starts for the next round.
  std::set<Va> StrongStarts = Starts;
  std::map<Va, Function> Built;
  std::set<Va> Processed;
  bool OrphanDone = false;
  std::vector<Function> Functions;
  constexpr size_t BatchSize = 64;

  auto promotableCall = [&](Va Target) -> std::optional<Va> {
    if (auto Head = fillerHead(Dis, Target))
      Target = *Head;
    if (!P.Img->isExecutable(Target))
      return std::nullopt;
    auto Primary = pdataPrimary(Pdata, Target);
    if (Primary && *Primary != Target)
      return std::nullopt;
    return Target;
  };
  auto chunksAllZero = [&](const Function &F) {
    return std::all_of(F.Chunks.begin(), F.Chunks.end(), [&](const Range &C) {
      llvm::ArrayRef<uint8_t> Bytes =
          P.Img->dataAt(C.first, (size_t)(C.second - C.first));
      return std::all_of(Bytes.begin(), Bytes.end(),
                         [](uint8_t B) { return B == 0; });
    });
  };

  // One bounded speculative descent per candidate in a gap between claimed
  // bytes; a probe is kept only when it decodes cleanly and every path ends.
  auto orphanProbe = [&](Va Lo, Va Hi, std::map<Va, Va> &Claimed,
                         std::vector<Function> &Out) {
    Va Cur = Lo;
    while (Cur < Hi) {
      if (auto End = analysis_detail::pointerTableEnd(*P.Img, Cur)) {
        Cur = *End;
        continue;
      }
      if (uint64_t Pad = padLen(Dis, *P.Img, Cur)) {
        Cur = Cur.wrappingAdd(Pad);
        continue;
      }
      auto Containing = Claimed.upper_bound(Cur);
      if (Containing != Claimed.begin()) {
        --Containing;
        if (Cur < Containing->second) {
          Cur = std::max(Containing->second, Cur.wrappingAdd(1));
          continue;
        }
      }
      auto NextClaim = Claimed.lower_bound(Cur.wrappingAdd(1));
      const Va BoundEnd =
          NextClaim == Claimed.end() ? Hi : std::min(NextClaim->first, Hi);
      const std::vector<Range> Bound{{Cur, BoundEnd}};
      analysis_detail::BuildResult Result = analysis_detail::buildFunction(
          *P.Img, Dis, Hooks, Cur, Starts, Pdata, Groups, SharedRipRefs,
          IsNoreturn, false, nullptr, Bound);
      SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
      Function &F = Result.F;
      // A probe whose bytes are all zeros decoded padding or a pointer table
      // as `add [rax],al`, never real code.
      if (!F.Instrs.empty() && chunksAllZero(F))
        F.Instrs.clear();
      if (analysis_detail::orphanFunctionOk(*P.Img, F, BoundEnd, Hooks)) {
        F.Orphan = true;
        Starts.insert(Cur);
        for (const Range &R : F.Chunks)
          Claimed[R.first] = R.second;
        for (const Range &R : F.DataRanges)
          Claimed[R.first] = R.second;
        const Va End = F.Chunks.empty() ? Cur : F.Chunks.back().second;
        Out.push_back(std::move(F));
        Cur = std::max(End, Cur.wrappingAdd(1));
      } else {
        Cur = Va((Cur.Value + 16) & ~uint64_t(15));
      }
    }
  };

  for (unsigned Iteration = 1; Iteration <= 8; ++Iteration) {
    std::vector<Va> NewStrong;
    std::set_difference(StrongStarts.begin(), StrongStarts.end(),
                        Processed.begin(), Processed.end(),
                        std::back_inserter(NewStrong));

    // Weak owners whose bytes a new strong start lands in are reconsidered
    // after strong code claims them.
    struct ChunkOwner {
      Va A, B, Owner;
    };
    std::vector<ChunkOwner> Chunks;
    for (const auto &[Start, F] : Built)
      for (const Range &R : F.Chunks)
        Chunks.push_back({R.first, R.second, Start});
    llvm::sort(Chunks, [](const ChunkOwner &L, const ChunkOwner &R) {
      return std::tie(L.A, L.B, L.Owner) < std::tie(R.A, R.B, R.Owner);
    });
    std::vector<Va> PrefixEnd;
    PrefixEnd.reserve(Chunks.size());
    Va RunningEnd(0);
    for (const ChunkOwner &C : Chunks) {
      RunningEnd = std::max(RunningEnd, C.B);
      PrefixEnd.push_back(RunningEnd);
    }
    std::set<Va> Dirty;
    for (Va Start : NewStrong) {
      size_t I = std::partition_point(Chunks.begin(), Chunks.end(),
                                      [&](const ChunkOwner &C) {
                                        return C.A <= Start;
                                      }) -
                 Chunks.begin();
      while (I > 0 && PrefixEnd[I - 1] > Start) {
        --I;
        const ChunkOwner &C = Chunks[I];
        if (C.A <= Start && Start < C.B && C.Owner != Start)
          Dirty.insert(C.Owner);
      }
    }
    std::set<Va> Rebuild(NewStrong.begin(), NewStrong.end());
    for (Va Owner : Dirty) {
      if (StrongStarts.count(Owner)) {
        Rebuild.insert(Owner);
      } else {
        Built.erase(Owner);
        Weak.insert(Owner);
      }
    }

    {
      std::vector<analysis_detail::BuildResult> Results;
      Results.reserve(Rebuild.size());
      for (Va Start : Rebuild)
        Results.push_back(analysis_detail::buildFunction(
            *P.Img, Dis, Hooks, Start, StrongStarts, Pdata, Groups,
            SharedRipRefs, IsNoreturn));
      for (analysis_detail::BuildResult &Result : Results) {
        Weak.insert(Result.LeaTargets.begin(), Result.LeaTargets.end());
        Weak.insert(Result.ForeignTargets.begin(), Result.ForeignTargets.end());
        SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
        const Va Start = Result.F.Start;
        Built.insert_or_assign(Start, std::move(Result.F));
      }
    }
    Processed.insert(NewStrong.begin(), NewStrong.end());

    std::map<Va, Va> StrongClaims;
    for (const auto &[Start, F] : Built)
      if (StrongStarts.count(Start))
        appendFunctionClaims(StrongClaims, F);
    for (auto It = Built.begin(); It != Built.end();) {
      if (!StrongStarts.count(It->first) && claimedAt(StrongClaims, It->first))
        It = Built.erase(It);
      else
        ++It;
    }

    std::set<Va> Pending = std::move(Weak);
    Weak.clear();
    std::set<Va> Rejected;
    std::map<Va, Va> Claims;
    std::map<Va, Va> DataClaims;
    for (const auto &[Start, F] : Built) {
      (void)Start;
      appendFunctionClaims(Claims, F);
      for (const Range &R : F.DataRanges)
        addClaim(DataClaims, R.first, R.second);
    }
    while (!Pending.empty()) {
      std::vector<Va> Candidates;
      while (Candidates.size() < BatchSize && !Pending.empty()) {
        Va S = *Pending.begin();
        Pending.erase(Pending.begin());
        // An entry inside a filler instruction is a thunk start mid-nop;
        // build at the run's head instead unless the head is already
        // claimed, held by another pending candidate, or inside known data.
        if (auto Head = fillerHead(Dis, S); Head && *Head < S) {
          auto NextPending = Pending.lower_bound(*Head);
          if (claimedAt(Claims, *Head) || claimedAt(DataClaims, *Head) ||
              claimsOverlap(Claims, {*Head, S}) ||
              (NextPending != Pending.end() && *NextPending < S))
            continue;
          S = *Head;
        }
        // A weak candidate starting with a zero byte is data, not code.
        if (auto B = P.Img->read(S, 1); B && !B->empty() && (*B)[0] == 0x00) {
          Rejected.insert(S);
          continue;
        }
        // `.ctors`/`.refptr` pointer tables parked in `.text`.
        if (analysis_detail::pointerTableEnd(*P.Img, S)) {
          Rejected.insert(S);
          continue;
        }
        auto Primary = pdataPrimary(Pdata, S);
        if (Built.count(S) || Rejected.count(S) || !P.Img->isExecutable(S) ||
            !P.Img->isBacked(S) || P.Img->inInlineData(S) ||
            (Primary && *Primary != S) || claimedAt(Claims, S) ||
            claimedAt(DataClaims, S) || !Dis.decodeOne(S))
          continue;
        Candidates.push_back(S);
      }
      if (Candidates.empty())
        break;
      std::vector<analysis_detail::BuildResult> Results;
      Results.reserve(Candidates.size());
      for (Va S : Candidates)
        Results.push_back(analysis_detail::buildFunction(
            *P.Img, Dis, Hooks, S, StrongStarts, Pdata, Groups, SharedRipRefs,
            IsNoreturn, false, &Claims));
      for (analysis_detail::BuildResult &Result : Results) {
        Function &F = Result.F;
        if (claimedAt(Claims, F.Start)) {
          Rejected.insert(F.Start);
          continue;
        }
        if (std::any_of(F.Chunks.begin(), F.Chunks.end(),
                        [&](const Range &R) { return claimsOverlap(Claims, R); })) {
          Pending.insert(F.Start);
          continue;
        }
        if (F.Instrs.empty() ||
            analysis_detail::rejectWeakFunction(*P.Img, Hooks, F)) {
          Rejected.insert(F.Start);
          continue;
        }
        Pending.insert(Result.LeaTargets.begin(), Result.LeaTargets.end());
        Pending.insert(Result.ForeignTargets.begin(),
                       Result.ForeignTargets.end());
        SharedRipRefs.insert(Result.RipRefs.begin(), Result.RipRefs.end());
        for (const Range &R : F.Chunks)
          addClaim(Claims, R.first, R.second);
        const Va Start = F.Start;
        Built.insert_or_assign(Start, std::move(F));
      }
    }

    const size_t StrongBefore = StrongStarts.size();
    for (const auto &[Start, F] : Built) {
      (void)Start;
      for (Va Call : F.CallTargets)
        if (auto Target = promotableCall(Call))
          StrongStarts.insert(*Target);
    }
    if (StrongStarts.size() > StrongBefore && Iteration < 8)
      continue;

    Starts.clear();
    Functions.clear();
    for (auto &[Start, F] : Built) {
      Starts.insert(Start);
      Functions.push_back(std::move(F));
    }
    Built.clear();

    if (!OrphanDone) {
      // Code outside every `.pdata` range and chunk: leaf helpers with no
      // unwind data and no caller. Two passes, because arbitration inside
      // each function's run can free bytes that open new orphan runs.
      for (unsigned Pass = 0; Pass < 2; ++Pass) {
        analysis_detail::OrphanSweep Plan =
            analysis_detail::planOrphanSweep(Pe, Functions);
        for (const Range &Gap : Plan.Gaps)
          orphanProbe(Gap.first, Gap.second, Plan.Claimed, Functions);
        llvm::sort(Functions, [](const Function &A, const Function &B) {
          return A.Start < B.Start;
        });
      }
      OrphanDone = true;
    }

    // Seeds that landed inside another function's claimed code (lea /
    // reloc false positives) claim those bytes a second time. Keep a
    // no-pdata function only when something outside another function's
    // chunks remains.
    {
      struct ChunkOwner {
        Va A, B, Owner;
      };
      std::vector<ChunkOwner> All;
      for (const Function &F : Functions)
        for (const Range &R : F.Chunks)
          All.push_back({R.first, R.second, F.Start});
      llvm::sort(All, [](const ChunkOwner &L, const ChunkOwner &R) {
        return std::tie(L.A, L.B, L.Owner) < std::tie(R.A, R.B, R.Owner);
      });
      std::vector<Va> PrefixMax;
      PrefixMax.reserve(All.size());
      Va Running(0);
      for (const ChunkOwner &C : All) {
        Running = std::max(Running, C.B);
        PrefixMax.push_back(Running);
      }
      auto coveredChunk = [&](Va A, Va B, Va Own) {
        const ptrdiff_t I = std::partition_point(All.begin(), All.end(),
                                                 [&](const ChunkOwner &C) {
                                                   return C.A <= A;
                                                 }) -
                            All.begin();
        for (ptrdiff_t J = I - 1; J >= 0 && PrefixMax[J] >= B; --J)
          if (All[J].B >= B && All[J].Owner != Own)
            return true;
        return false;
      };
      llvm::erase_if(Functions, [&](const Function &F) {
        return !(F.HasPdata || F.Chunks.empty() ||
                 !std::all_of(F.Chunks.begin(), F.Chunks.end(),
                              [&](const Range &R) {
                                return coveredChunk(R.first, R.second,
                                                    F.Start);
                              }));
      });
    }

    const size_t BeforeOrphanCalls = StrongStarts.size();
    for (const Function &F : Functions)
      for (Va Call : F.CallTargets)
        if (auto Target = promotableCall(Call))
          StrongStarts.insert(*Target);
    if (StrongStarts.size() > BeforeOrphanCalls && Iteration < 8) {
      for (Function &F : Functions) {
        const Va Start = F.Start;
        Built.insert_or_assign(Start, std::move(F));
      }
      Functions.clear();
      continue;
    }

    // Noreturn fixpoint, then re-descent for callers whose block still has a
    // path continuing past a noreturn call.
    for (Function &F : Functions)
      setFunctionName(F, *P.Names);
    std::set<Va> NoreturnStarts =
        propagateNoreturn(Functions, *P.Externs, *P.Names, Hooks);
    for (unsigned Pass = 0; Pass < 4; ++Pass) {
      auto IsNoreturn2 = [&](Va Target) {
        return IsNoreturn(Target) || NoreturnStarts.count(Target) != 0;
      };
      std::set<Va> DirtyCallers;
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
            DirtyCallers.insert(F.Start);
        }
      }
      if (DirtyCallers.empty())
        break;
      for (Va Start : DirtyCallers) {
        analysis_detail::BuildResult Result = analysis_detail::buildFunction(
            *P.Img, Dis, Hooks, Start, StrongStarts, Pdata, Groups,
            SharedRipRefs, IsNoreturn2);
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
    break;
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
