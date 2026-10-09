#include "AnalysisImpl.h"
#include "revenant/Analysis/Padding.h"
#include "revenant/Analysis/JumpTable.h"

#include <algorithm>
#include <deque>
#include <set>
#include <tuple>

namespace revenant {
namespace {

using Range = std::pair<Va, Va>;

struct Builder {
  const Image &Img;
  const Disasm &Dis;
  const TargetHooks &Hooks;
  Va Start;
  const std::set<Va> &FunctionStarts;
  const std::map<Va, Range> &Pdata;
  const std::map<Va, std::vector<Range>> &PdataGroups;
  const std::set<Va> &SharedRipRefs;
  llvm::function_ref<bool(Va)> IsNoreturn;

  Function F;
  std::deque<Va> Queue;
  std::set<Va> Leaders;
  std::set<Va> Decoded;
  std::set<Va> InstrStarts;
  std::map<Va, Va> InstrRanges;
  std::vector<Va> LeaTargets;
  std::vector<Va> RipRefs;
  std::vector<Va> ForeignTargets;
  std::vector<std::pair<Va, Va>> DataRanges;
  std::set<Va> FailedRoots;
  std::map<Va, uint64_t> Limits;
  bool LimitsDirty = false;
  std::vector<std::tuple<Va, Va, Va>> Unbounded;
  std::set<Va> DeadStarts;
  std::optional<std::vector<Range>> Bound;
  const std::map<Va, Va> *ClaimedCode = nullptr;
  bool DiscoveryOnly = false;
  bool BoundExit = false;
  const char *Failure = nullptr;

  Builder(const Image &Image, const Disasm &Decoder, const TargetHooks &Target,
          Va FunctionStart, const std::set<Va> &Starts,
          const std::map<Va, Range> &FunctionRanges,
          const std::map<Va, std::vector<Range>> &GroupedRanges,
          const std::set<Va> &RipReferences,
          llvm::function_ref<bool(Va)> Noreturn)
      : Img(Image), Dis(Decoder), Hooks(Target), Start(FunctionStart),
        FunctionStarts(Starts), Pdata(FunctionRanges),
        PdataGroups(GroupedRanges), SharedRipRefs(RipReferences),
        IsNoreturn(Noreturn) {
    F.Start = Start;
    F.End = Start;
    auto P = Pdata.find(Start);
    F.HasPdata = P != Pdata.end() && P->second.first == Start;
    Queue.push_back(Start);
    Leaders.insert(Start);
  }

  bool inBound(Va Address) const {
    if (!Bound)
      return true;
    for (const Range &R : *Bound)
      if (Address >= R.first && Address < R.second)
        return true;
    return false;
  }

  bool inData(Va Address) const {
    for (const Range &R : DataRanges)
      if (Address >= R.first && Address < R.second)
        return true;
    return false;
  }

  bool midInstruction(Va Address) const {
    auto It = InstrRanges.upper_bound(Address);
    if (It == InstrRanges.begin())
      return false;
    --It;
    return Address > It->first && Address < It->second;
  }

  bool foreign(Va Address) const {
    if (ClaimedCode) {
      auto It = ClaimedCode->upper_bound(Address);
      if (It != ClaimedCode->begin()) {
        --It;
        if (It->second > Address)
          return true;
      }
    }
    if (Bound)
      return false;
    auto Group = PdataGroups.find(Start);
    if (Group != PdataGroups.end())
      for (const Range &R : Group->second)
        if (Address >= R.first && Address < R.second)
          return false;
    if (F.HasPdata)
      return true;
    if (Address != Start && FunctionStarts.count(Address))
      return true;
    auto It = Pdata.upper_bound(Address);
    if (It != Pdata.begin()) {
      --It;
      if (Address >= It->first && Address < It->second.second)
        return true;
    }
    return false;
  }

  bool foreignTarget(Va Address) const {
    return (FunctionStarts.count(Address) && Address != Start) ||
           foreign(Address);
  }

  bool enqueue(Va Address) {
    if (!inBound(Address) || !Img.isExecutable(Address) || inData(Address))
      return false;
    if (Bound && midInstruction(Address))
      return false;
    if (Leaders.insert(Address).second)
      Queue.push_back(Address);
    return true;
  }

  std::optional<Va> callTarget(const Instr &I) const {
    if (I.Flow != FlowKind::Call)
      return std::nullopt;
    if (I.HasTarget)
      return I.Target;
    if (I.HasMemTarget)
      return I.MemTarget;
    return std::nullopt;
  }

  std::vector<Range> codeRegion() const {
    std::vector<Range> Own = ownCodeRanges();
    if (!Own.empty())
      return Own;
    if (Bound)
      return *Bound;
    const Section *S = Img.sectionOf(Start);
    Va End = S ? S->end() : Start;
    auto NextStart = FunctionStarts.upper_bound(Start);
    if (NextStart != FunctionStarts.end())
      End = std::min(End, *NextStart);
    auto NextPdata = Pdata.upper_bound(Start);
    if (NextPdata != Pdata.end())
      End = std::min(End, NextPdata->first);
    if (ClaimedCode) {
      auto NextClaim = ClaimedCode->upper_bound(Start);
      if (NextClaim != ClaimedCode->end())
        End = std::min(End, NextClaim->first);
    }
    return {{Start, End}};
  }

  bool isForeignTarget(Va Target) const {
    return (Target != Start && FunctionStarts.count(Target)) ||
           foreign(Target);
  }

  void resolveFixpoint() {
    for (;;) {
      const std::vector<Range> Ranges = codeRegion();
      const std::set<Va> LocalRefs(RipRefs.begin(), RipRefs.end());
      JumpTableCtx Ctx{Img, Ranges, LocalRefs, SharedRipRefs, Limits};
      bool ResolvedAny = false;
      const size_t NumBlocks = F.Blocks.size();
      for (size_t I = 0; I < NumBlocks; ++I) {
        BasicBlock &Dispatch = F.Blocks[I];
        if (Dispatch.EndKind != BlockEnd::UnresolvedIndirect)
          continue;
        std::optional<JumpTableInfo> Result =
            resolveJumpTable(Ctx, Hooks, F.Blocks, F.Instrs, I);
        if (Result) {
          ResolvedAny = true;
          if (Result->Spans) {
            const VaRange &Span = Result->Spans->first;
            DataRanges.emplace_back(Span.Start, Span.End);
            if (Result->Spans->second) {
              const VaRange &Span2 = *Result->Spans->second;
              DataRanges.emplace_back(Span2.Start, Span2.End);
            }
          }
          if (!Result->Guard && Dispatch.InstrsEnd > Dispatch.InstrsBegin)
            Unbounded.emplace_back(
                Dispatch.Start, Result->EnumBase,
                F.Instrs[Dispatch.InstrsEnd - 1].Addr);

          const Va DispatchStart = Dispatch.Start;
          if (Dispatch.InstrsEnd > Dispatch.InstrsBegin) {
            const Va BranchIp = F.Instrs[Dispatch.InstrsEnd - 1].Addr;
            F.UnresolvedIndirects.erase(
                std::remove(F.UnresolvedIndirects.begin(),
                            F.UnresolvedIndirects.end(), BranchIp),
                F.UnresolvedIndirects.end());
          }
          Dispatch.EndKind = BlockEnd::Branch;
          Dispatch.SwitchIndex = Result->IndexLoc;
          Dispatch.Edges.clear();
          for (const auto &[Target, Values] : Result->Targets) {
            const bool Foreign = isForeignTarget(Target);
            const bool Mid = midInstruction(Target);
            Edge E;
            E.To = Target;
            E.Kind = (Foreign || Mid) ? EdgeKind::TailCall : EdgeKind::Switch;
            if (E.Kind == EdgeKind::Switch) {
              E.SwitchTable = Result->Table;
              E.SwitchValues = Values;
            }
            Dispatch.Edges.push_back(std::move(E));
            if (Foreign)
              ForeignTargets.push_back(Target);
            else if (!Mid)
              enqueue(Target);
          }
          if (Result->Guard && *Result->Guard < F.Blocks.size()) {
            BasicBlock &Guard = F.Blocks[*Result->Guard];
            for (Edge &E : Guard.Edges)
              if (E.To != DispatchStart &&
                  (E.Kind == EdgeKind::CondTaken ||
                   E.Kind == EdgeKind::CondNotTaken))
                E.Kind = EdgeKind::SwitchDefault;
          }
        } else if (auto Spans =
                       jumpTableDataSpan(Ctx, Hooks, F.Instrs, F.Blocks, I)) {
          DataRanges.insert(DataRanges.end(), Spans->begin(), Spans->end());
        }
      }
      drainQueue();
      if (!ResolvedAny)
        break;
    }
  }
  void rebuildDecodeState() {
    InstrStarts.clear();
    InstrRanges.clear();
    Decoded.clear();
    for (const Instr &I : F.Instrs) {
      InstrStarts.insert(I.Addr);
      InstrRanges[I.Addr] = I.nextVa();
    }
    for (const BasicBlock &B : F.Blocks) {
      Leaders.insert(B.Start);
      Decoded.insert(B.Start);
    }
  }

  bool findTableLimits() {
    std::map<Va, size_t> ByStart;
    for (size_t I = 0; I < F.Blocks.size(); ++I)
      ByStart.emplace(F.Blocks[I].Start, I);
    std::set<Va> Reachable;
    std::vector<Va> Todo{Start};
    while (!Todo.empty()) {
      Va S = Todo.back();
      Todo.pop_back();
      auto It = ByStart.find(S);
      if (It == ByStart.end() || !Reachable.insert(S).second)
        continue;
      for (const Edge &E : F.Blocks[It->second].Edges)
        if (E.Kind == EdgeKind::Fallthrough || E.Kind == EdgeKind::Jump ||
            E.Kind == EdgeKind::CondTaken ||
            E.Kind == EdgeKind::CondNotTaken ||
            E.Kind == EdgeKind::SwitchDefault)
          Todo.push_back(E.To);
    }
    std::vector<std::pair<uint64_t, uint64_t>> Real;
    for (const auto &[S, I] : ByStart) {
      if (!Reachable.count(S))
        continue;
      const BasicBlock &B = F.Blocks[I];
      for (size_t J = B.InstrsBegin; J < B.InstrsEnd; ++J)
        Real.emplace_back(F.Instrs[J].Addr.Value,
                          F.Instrs[J].nextVa().Value);
    }
    llvm::sort(Real);
    auto Straddles = [&](Va T) {
      if (DeadStarts.count(T))
        return true;
      auto It = std::partition_point(
          Real.begin(), Real.end(),
          [&](const std::pair<uint64_t, uint64_t> &R) { return R.second <= T.Value; });
      return It != Real.end() && T.Value > It->first;
    };
    auto IsIndirect = [&](Va T) {
      auto It = InstrRanges.find(T);
      if (It == InstrRanges.end())
        return false;
      for (const Instr &I : F.Instrs)
        if (I.Addr == T)
          return I.Flow == FlowKind::IndirectBranch;
      return false;
    };
    const std::vector<Range> Code = codeRegion();
    const std::set<Va> Local(RipRefs.begin(), RipRefs.end());
    JumpTableCtx Ctx{Img, Code, Local, SharedRipRefs, Limits};
    bool Learned = false;
    for (const auto &[DispatchStart, Base, BranchIp] : Unbounded) {
      (void)BranchIp;
      auto It = ByStart.find(DispatchStart);
      if (It == ByStart.end())
        continue;
      auto Limit = bogusTableEntryIndex(Ctx, Hooks, F.Blocks, F.Instrs,
                                        It->second, Base, Straddles,
                                        IsIndirect);
      if (!Limit)
        continue;
      auto Old = Limits.find(Base);
      if (Old == Limits.end() || *Limit < Old->second) {
        Limits[Base] = *Limit;
        Learned = true;
      }
    }
    return Learned;
  }
  void decodeBlock(Va Leader) {
    if (!Img.isExecutable(Leader) || Img.inInlineData(Leader) ||
        DeadStarts.count(Leader))
      return;
    if (foreign(Leader))
      return;
    if (InstrStarts.count(Leader)) {
      Leaders.insert(Leader);
      return;
    }
    if (midInstruction(Leader) && Bound) {
      Failure = Failure ? Failure : "mid-instruction root";
      return;
    }
    if (inData(Leader) || !Decoded.insert(Leader).second)
      return;

    const size_t First = F.Instrs.size();
    Va IP = Leader;
    BlockEnd EndKind = BlockEnd::EndOfFunction;
    std::vector<Edge> Edges;
    bool Traps = false;
    while (true) {
      if (IP != Leader) {
        if (!inBound(IP)) {
          EndKind = BlockEnd::EndOfFunction;
          BoundExit = true;
          break;
        }
        const bool Foreign = foreign(IP) || !Img.isBacked(IP);
        if (InstrStarts.count(IP) || Leaders.count(IP) || Foreign ||
            inData(IP) || Img.inInlineData(IP) ||
            (Bound && midInstruction(IP))) {
          if (midInstruction(IP) || inData(IP)) {
            EndKind = BlockEnd::EndOfFunction;
            break;
          }
          EndKind = Foreign ? BlockEnd::TailCall : BlockEnd::Fallthrough;
          Edges.push_back({IP, Foreign ? EdgeKind::TailCall
                                       : EdgeKind::Fallthrough});
          if (Foreign && !Img.inInlineData(IP))
            ForeignTargets.push_back(IP);
          else if (!Foreign)
            Leaders.insert(IP);
          break;
        }
      }
      if (Bound && !inBound(IP)) {
        EndKind = BlockEnd::TailCall;
        Edges.push_back({IP, EdgeKind::TailCall});
        break;
      }
      std::optional<Instr> MaybeI = Dis.decodeOne(IP);
      if (!MaybeI) {
        if (Bound)
          Failure = Failure ? Failure : "unreadable";
        break;
      }
      const Instr I = *MaybeI;
      if (Traps &&
          Hooks.trapKind(I) != TargetHooks::TrapKind::Exception &&
          Hooks.trapKind(I) != TargetHooks::TrapKind::Absorbable)
        break;
      const Va Next = I.nextVa();
      if (Bound) {
        bool Fits = false;
        for (const Range &R : *Bound)
          Fits |= IP >= R.first && Next <= R.second;
        if (!Fits) {
          Failure = Failure ? Failure : "instruction crosses region end";
          break;
        }
      }
      if (Bound && InstrStarts.lower_bound(IP.wrappingAdd(1)) !=
                       InstrStarts.lower_bound(Next)) {
        Failure = Failure ? Failure : "overlaps existing instruction";
        EndKind = BlockEnd::EndOfFunction;
        break;
      }
      if (!Bound &&
          (foreign(Next.wrappingAdd(-1)) ||
           (ClaimedCode && [&] {
             auto It = ClaimedCode->upper_bound(IP);
             if (It != ClaimedCode->begin()) {
               --It;
               if (It->second > IP)
                 return true;
             }
             It = ClaimedCode->lower_bound(IP);
             return It != ClaimedCode->end() && It->first < Next;
           }()) ||
           [&] {
             auto It = FunctionStarts.upper_bound(IP);
             return It != FunctionStarts.end() && *It < Next;
           }())) {
        EndKind = BlockEnd::TailCall;
        Edges.push_back({IP, EdgeKind::TailCall});
        break;
      }
      InstrStarts.insert(IP);
      InstrRanges[IP] = Next;
      if (I.HasMemTarget) {
        if (Img.isExecutable(I.MemTarget)) {
          if (Hooks.mnemonic(I) == "LEA64r" || Hooks.mnemonic(I) == "LEA64_32r")
            LeaTargets.push_back(I.MemTarget);
        } else {
          RipRefs.push_back(I.MemTarget);
        }
      }
      F.Instrs.push_back(I);
      IP = Next;
      switch (I.Flow) {
      case FlowKind::Return:
        EndKind = BlockEnd::Return;
        break;
      case FlowKind::CondBranch:
        EndKind = BlockEnd::Branch;
        if (I.HasTarget) {
          const Va Target = I.Target;
          if (foreignTarget(Target) || !inBound(Target)) {
            ForeignTargets.push_back(Target);
            Edges.push_back({Target, EdgeKind::TailCall});
          } else {
            Edges.push_back({Target, EdgeKind::CondTaken});
            enqueue(Target);
          }
        }
        if (enqueue(IP))
          Edges.push_back({IP, EdgeKind::CondNotTaken});
        goto done;
      case FlowKind::Branch:
        if (I.HasTarget) {
          const Va Target = I.Target;
          if (foreignTarget(Target) || !inBound(Target)) {
            ForeignTargets.push_back(Target);
            EndKind = BlockEnd::TailCall;
            Edges.push_back({Target, EdgeKind::TailCall});
          } else {
            EndKind = BlockEnd::Branch;
            Edges.push_back({Target, EdgeKind::Jump});
            enqueue(Target);
          }
        } else {
          EndKind = BlockEnd::Branch;
        }
        goto done;
      case FlowKind::IndirectBranch:
        F.UnresolvedIndirects.push_back(I.Addr);
        EndKind = BlockEnd::UnresolvedIndirect;
        goto done;
      case FlowKind::Call:
        if (std::optional<Va> Target = callTarget(I)) {
          F.CallTargets.push_back(*Target);
          if (IsNoreturn(*Target)) {
            EndKind = BlockEnd::Noreturn;
            F.NoreturnCalls.push_back(I.Addr);
            Traps = true;
          }
        }
        continue;
      case FlowKind::Trap:
        EndKind = BlockEnd::Noreturn;
        if (Traps &&
            (Hooks.trapKind(I) == TargetHooks::TrapKind::Exception ||
             Hooks.trapKind(I) == TargetHooks::TrapKind::Absorbable))
          continue;
        goto done;
      case FlowKind::None:
        if (Hooks.isNoreturnTrap(I)) {
          EndKind = BlockEnd::Noreturn;
          if (Traps && (Hooks.trapKind(I) == TargetHooks::TrapKind::Exception ||
                        Hooks.trapKind(I) == TargetHooks::TrapKind::Absorbable))
            continue;
          goto done;
        }
        continue;
      }
      break;
    }
  done:
    if (F.Instrs.size() == First)
      return;
    const Va End = F.Instrs.back().nextVa();
    F.End = std::max(F.End, End);
    BasicBlock B;
    B.Start = Leader;
    B.End = End;
    B.InstrsBegin = First;
    B.InstrsEnd = F.Instrs.size();
    B.Edges = std::move(Edges);
    B.EndKind = EndKind;
    F.Blocks.push_back(std::move(B));
  }

  void drainQueue() {
    while (!Queue.empty()) {
      Va Leader = Queue.front();
      Queue.pop_front();
      decodeBlock(Leader);
    }
  }
  void splitAtLeaders() {
    const std::vector<Instr> &Instructions = F.Instrs;
    std::vector<BasicBlock> Old = std::move(F.Blocks);
    std::vector<BasicBlock> Out;
    Out.reserve(Old.size());
    for (BasicBlock &B : Old) {
      auto It = Leaders.upper_bound(B.Start);
      bool HasInner = It != Leaders.end() && *It < B.End;
      if (!HasInner) {
        Out.push_back(std::move(B));
        continue;
      }
      Va StartBlock = B.Start;
      size_t First = B.InstrsBegin;
      for (; It != Leaders.end() && *It < B.End; ++It) {
        Va Leader = *It;
        size_t Cut = First;
        while (Cut < B.InstrsEnd && Instructions[Cut].Addr < Leader)
          ++Cut;
        if (Cut == First || Cut >= B.InstrsEnd ||
            Instructions[Cut].Addr != Leader)
          continue;
        BasicBlock Piece;
        Piece.Start = StartBlock;
        Piece.End = Leader;
        Piece.InstrsBegin = First;
        Piece.InstrsEnd = Cut;
        Piece.Edges.push_back({Leader, EdgeKind::Fallthrough});
        Piece.EndKind = BlockEnd::Fallthrough;
        Piece.SwitchIndex = B.SwitchIndex;
        Out.push_back(std::move(Piece));
        StartBlock = Leader;
        First = Cut;
      }
      BasicBlock Last;
      Last.Start = StartBlock;
      Last.End = B.End;
      Last.InstrsBegin = First;
      Last.InstrsEnd = B.InstrsEnd;
      Last.Edges = std::move(B.Edges);
      Last.EndKind = B.EndKind;
      Last.SwitchIndex = B.SwitchIndex;
      Out.push_back(std::move(Last));
    }
    llvm::sort(Out, [](const BasicBlock &A, const BasicBlock &B) {
      return A.Start < B.Start;
    });
    F.Blocks = std::move(Out);
  }

  void run() {
    drainQueue();
    if (DiscoveryOnly) {
      finish();
      return;
    }
    for (unsigned Iteration = 0;; ++Iteration) {
      resolveFixpoint();
      F.DataRanges = DataRanges;
      analysis_detail::pruneDataInstrs(F, Img);
      rebuildDecodeState();
      bool Learned = findTableLimits() || LimitsDirty;
      if (!Learned) {
        std::vector<Va> WeakIps;
        for (const auto &[Dispatch, Base, Ip] : Unbounded) {
          (void)Dispatch;
          (void)Base;
          WeakIps.push_back(Ip);
        }
        auto Veto = [&](Va Address) {
          return foreign(Address) || inData(Address) ||
                 Img.inInlineData(Address);
        };
        analysis_detail::OverlapEnv Env{WeakIps, Bound.has_value(),
                                        &DeadStarts, Veto};
        analysis_detail::resolveOverlaps(F, Img, Dis, Hooks, Env);
        rebuildDecodeState();
        Learned = findTableLimits() || LimitsDirty;
      }
      if (!Learned) {
        splitAtLeaders();
        if (!Bound) {
          sweepOwnRegion();
          F.DataRanges = DataRanges;
          analysis_detail::pruneDataInstrs(F, Img);
          rebuildDecodeState();
          splitAtLeaders();
        }
      }
      if ((!Learned && !LimitsDirty) || Iteration >= 8)
        break;
      reset();
      drainQueue();
    }
    finish();
  }

  void reset() {
    std::map<Va, uint64_t> KeptLimits = std::move(Limits);
    std::vector<Va> KeptRipRefs = std::move(RipRefs);
    F.Instrs.clear();
    F.Blocks.clear();
    F.Chunks.clear();
    F.DataRanges.clear();
    F.End = Start;
    F.UnresolvedIndirects.clear();
    F.NoreturnCalls.clear();
    F.CallTargets.clear();
    F.ExtraRoots.clear();
    LeaTargets.clear();
    ForeignTargets.clear();
    DataRanges.clear();
    Unbounded.clear();
    FailedRoots.clear();
    DeadStarts.clear();
    Queue = {Start};
    Leaders = {Start};
    Decoded.clear();
    InstrStarts.clear();
    InstrRanges.clear();
    Limits = std::move(KeptLimits);
    RipRefs = std::move(KeptRipRefs);
    Failure = nullptr;
    LimitsDirty = false;
  }

  void finish() {
    F.DataRanges = DataRanges;
    std::vector<Va> WeakIps;
    for (const auto &[Dispatch, Base, Ip] : Unbounded) {
      (void)Dispatch;
      (void)Base;
      WeakIps.push_back(Ip);
    }
    auto Veto = [&](Va Address) {
      return foreign(Address) || inData(Address) || Img.inInlineData(Address);
    };
    analysis_detail::OverlapEnv Env{WeakIps, Bound.has_value(), &DeadStarts,
                                    Veto};
    analysis_detail::resolveOverlaps(F, Img, Dis, Hooks, Env);
    analysis_detail::pruneDataInstrs(F, Img);
    std::vector<Range> Own =
        Bound ? std::vector<Range>{} : ownCodeRanges();
    if (Own.empty() && !Bound) {
      auto It = Pdata.find(Start);
      if (It != Pdata.end())
        Own.emplace_back(Va(0), It->second.second);
    }
    analysis_detail::buildChunks(F, Own);
  }
  std::vector<Range> ownCodeRanges() const {
    auto It = PdataGroups.find(Start);
    return It == PdataGroups.end() ? std::vector<Range>{} : It->second;
  }

  bool attempt(Va Root, const std::vector<Range> &Ranges) {
    const size_t NumInstrs = F.Instrs.size();
    const std::vector<BasicBlock> Blocks = F.Blocks;
    const Va End = F.End;
    const std::vector<Va> Unresolved = F.UnresolvedIndirects;
    const std::vector<Va> NoreturnCalls = F.NoreturnCalls;
    const std::vector<Va> Calls = F.CallTargets;
    const std::vector<Va> ExtraRoots = F.ExtraRoots;
    const auto LeadersBefore = Leaders;
    const auto DecodedBefore = Decoded;
    const auto StartsBefore = InstrStarts;
    const auto RangesBefore = InstrRanges;
    const auto QueueBefore = Queue;
    const size_t NumLea = LeaTargets.size();
    const size_t NumRip = RipRefs.size();
    const size_t NumData = DataRanges.size();
    const size_t NumUnbounded = Unbounded.size();
    const size_t NumForeign = ForeignTargets.size();

    Bound = Ranges;
    BoundExit = false;
    Failure = nullptr;
    enqueue(Root);
    drainQueue();
    resolveFixpoint();
    const bool Overrun = findTableLimits();
    if (Overrun)
      LimitsDirty = true;
    const bool BoundExitOk =
        llvm::all_of(F.Blocks, [&](const BasicBlock &B) {
          if (B.EndKind != BlockEnd::EndOfFunction)
            return true;
          return llvm::any_of(Ranges,
                              [&](const Range &R) { return B.End == R.second; });
        });
    bool HitsData = false;
    for (size_t I = NumInstrs; I < F.Instrs.size(); ++I)
      for (const Range &R : DataRanges)
        HitsData |= F.Instrs[I].Addr < R.second &&
                    F.Instrs[I].nextVa() > R.first;
    Bound.reset();
    if (!Failure && BoundExitOk && !Overrun && !HitsData &&
        F.Instrs.size() > NumInstrs) {
      F.ExtraRoots.push_back(Root);
      return true;
    }

    F.Instrs.resize(NumInstrs);
    F.Blocks = Blocks;
    F.End = End;
    F.UnresolvedIndirects = Unresolved;
    F.NoreturnCalls = NoreturnCalls;
    F.CallTargets = Calls;
    F.ExtraRoots = ExtraRoots;
    Leaders = LeadersBefore;
    Decoded = DecodedBefore;
    InstrStarts = StartsBefore;
    InstrRanges = RangesBefore;
    Queue = QueueBefore;
    LeaTargets.resize(NumLea);
    RipRefs.resize(NumRip);
    DataRanges.resize(NumData);
    Unbounded.resize(NumUnbounded);
    ForeignTargets.resize(NumForeign);
    Failure = nullptr;
    FailedRoots.insert(Root);
    return false;
  }

  void sweepOwnRegion() {
    const std::vector<Range> Ranges = ownCodeRanges();
    if (Ranges.empty())
      return;
    for (unsigned Pass = 0; Pass < 32; ++Pass) {
      std::vector<Range> Covered;
      for (const BasicBlock &B : F.Blocks)
        Covered.emplace_back(B.Start, B.End);
      llvm::sort(Covered);
      std::vector<Range> Merged;
      for (const Range &R : Covered) {
        if (!Merged.empty() && R.first <= Merged.back().second)
          Merged.back().second = std::max(Merged.back().second, R.second);
        else
          Merged.push_back(R);
      }
      std::vector<Va> Candidates;
      for (const Range &Region : Ranges) {
        Va Cursor = Region.first;
        auto addGap = [&](Va End) {
          while (Cursor < End) {
            uint64_t Pad = padLen(Dis, Img, Cursor);
            if (Pad) {
              Cursor = Cursor.wrappingAdd(Pad);
              continue;
            }
            if (auto InlineEnd = Img.inlineDataEnd(Cursor)) {
              Cursor = std::min(*InlineEnd, End);
              continue;
            }
            if (!Img.isBacked(Cursor) || inData(Cursor)) {
              Cursor = Cursor.wrappingAdd(1);
              continue;
            }
            if (!FailedRoots.count(Cursor) && !DeadStarts.count(Cursor))
              Candidates.push_back(Cursor);
            break;
          }
        };
        for (const Range &Used : Merged) {
          if (Used.second <= Cursor || Used.first >= Region.second)
            continue;
          if (Cursor < Used.first)
            addGap(std::min(Used.first, Region.second));
          Cursor = std::max(Cursor, Used.second);
          if (Cursor >= Region.second)
            break;
        }
        if (Cursor < Region.second)
          addGap(Region.second);
      }
      if (Candidates.empty())
        break;
      bool Added = false;
      for (Va Candidate : Candidates)
        Added |= attempt(Candidate, Ranges);
      if (!Added)
        break;
      splitAtLeaders();
    }
  }
};

} // namespace
namespace analysis_detail {

BuildResult buildFunction(
    const Image &Img, const Disasm &Dis, const TargetHooks &Hooks, Va Start,
    const std::set<Va> &FunctionStarts,
    const std::map<Va, std::pair<Va, Va>> &Pdata,
    const std::map<Va, std::vector<std::pair<Va, Va>>> &PdataGroups,
    const std::set<Va> &SharedRipRefs,
    llvm::function_ref<bool(Va)> IsNoreturn, bool DiscoveryOnly,
    const std::map<Va, Va> *ClaimedCode,
    llvm::ArrayRef<std::pair<Va, Va>> Bound) {
  Builder B(Img, Dis, Hooks, Start, FunctionStarts, Pdata, PdataGroups,
            SharedRipRefs, IsNoreturn);
  B.DiscoveryOnly = DiscoveryOnly;
  B.ClaimedCode = ClaimedCode;
  if (!Bound.empty())
    B.Bound = std::vector<std::pair<Va, Va>>(Bound.begin(), Bound.end());
  B.run();
  return {std::move(B.F), std::move(B.LeaTargets), std::move(B.RipRefs),
          std::move(B.ForeignTargets)};
}

} // namespace analysis_detail
} // namespace revenant
