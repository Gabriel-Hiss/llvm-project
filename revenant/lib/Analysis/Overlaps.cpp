//===- revenant/lib/Analysis/Overlaps.cpp - Overlap arbitration ---------===//
//
// Port of `Builder::resolve_overlaps`, `redecode_orphans`,
// `prune_data_instrs` and the `finish()` tail (chunk clipping, data ranges,
// unresolved indirects, mid-instruction edge filter) from
// crates/analysis/src/lib.rs.
//
// Decode order is not alignment truth: a bogus switch entry or sweep root
// can claim bytes a real edge target needs before that target is reached.
// Where two blocks' instruction bytes overlap, arbitration keeps the block
// whose start has real control-flow demand (ties: earlier start) and drops
// the loser, whose start is remembered as dead so the sweep never
// re-decodes the same garbage. Edges into dropped blocks become TailCall;
// edges into bytes no survivor covers are orphans, decoded again so real
// code the loser covered comes back and the edge is restored.
//
// What this slice does not own stays out: switch resolution fixpoints and
// the own-region sweep live in Discovery's Builder, which calls these
// helpers. `resolveOverlaps` therefore re-decodes orphans with a plain
// linear walk (no table resolution); the Builder re-runs its fixpoint
// afterwards the same way `redecode_orphans` ends in `resolve_fixpoint`.
//
//===----------------------------------------------------------------------===//

#include "AnalysisImpl.h"

#include "revenant/Core/Image.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Target/TargetHooks.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

namespace revenant::analysis_detail {
namespace {

bool instrOverlapsData(llvm::ArrayRef<std::pair<Va, Va>> Data, Va A, Va B) {
  for (const auto &[S, E] : Data)
    if (S < B && E > A)
      return true;
  return false;
}

bool vaInData(llvm::ArrayRef<std::pair<Va, Va>> Data, Va V) {
  for (const auto &[S, E] : Data)
    if (V >= S && V < E)
      return true;
  return false;
}

/// True when `V` lands strictly inside a decoded instruction's byte range.
bool midInstr(const std::map<Va, Va> &Ranges, Va V) {
  auto It = Ranges.upper_bound(V);
  if (It == Ranges.begin())
    return false;
  --It;
  return V > It->first && V < It->second;
}

bool isOrphanKind(EdgeKind K) {
  return K == EdgeKind::Fallthrough || K == EdgeKind::Jump ||
         K == EdgeKind::CondTaken || K == EdgeKind::CondNotTaken;
}

/// Blocks whose terminator is a guard-free (unbounded) table dispatch are
/// the speculative class: a block reached only through their switch edges
/// never wins arbitration against anything reached another way.
std::set<Va> weakSources(const Function &F, llvm::ArrayRef<Va> WeakIps) {
  std::set<Va> Out;
  if (WeakIps.empty())
    return Out;
  for (const BasicBlock &B : F.Blocks) {
    if (B.InstrsEnd == B.InstrsBegin || B.InstrsEnd > F.Instrs.size())
      continue;
    Va Ip = F.Instrs[B.InstrsEnd - 1].Addr;
    if (llvm::is_contained(WeakIps, Ip))
      Out.insert(B.Start);
  }
  return Out;
}

/// Block starts reachable without traversing a TailCall or a weak-source
/// switch edge.
std::set<Va> strongStarts(const Function &F, const std::set<Va> &WeakSrc) {
  std::map<Va, std::vector<const BasicBlock *>> ByStart;
  for (const BasicBlock &B : F.Blocks)
    ByStart[B.Start].push_back(&B);
  std::set<Va> Strong;
  std::vector<Va> Todo{F.Start};
  Todo.insert(Todo.end(), F.ExtraRoots.begin(), F.ExtraRoots.end());
  while (!Todo.empty()) {
    Va S = Todo.back();
    Todo.pop_back();
    auto It = ByStart.find(S);
    if (It == ByStart.end() || !Strong.insert(S).second)
      continue;
    for (const BasicBlock *B : It->second) {
      for (const Edge &E : B->Edges) {
        if (E.Kind == EdgeKind::TailCall)
          continue;
        if (WeakSrc.count(B->Start) &&
            (E.Kind == EdgeKind::Switch || E.Kind == EdgeKind::SwitchDefault))
          continue;
        Todo.push_back(E.To);
      }
    }
  }
  return Strong;
}

/// Demand per block start, counting only edges out of strong blocks:
/// the function start and extra roots demand 2, ordinary edges 2,
/// guarded switch edges 1, TailCall and weak-source switch edges 0.
std::map<Va, uint8_t> demandOf(const Function &F, const std::set<Va> &Strong,
                               const std::set<Va> &WeakSrc) {
  std::map<Va, uint8_t> Demand;
  Demand[F.Start] = 2;
  for (Va R : F.ExtraRoots)
    Demand[R] = 2;
  for (const BasicBlock &B : F.Blocks) {
    if (!Strong.count(B.Start))
      continue;
    for (const Edge &E : B.Edges) {
      uint8_t D = 2;
      if (E.Kind == EdgeKind::Switch || E.Kind == EdgeKind::SwitchDefault)
        D = WeakSrc.count(B.Start) ? 0 : 1;
      else if (E.Kind == EdgeKind::TailCall)
        D = 0;
      auto It = Demand.find(E.To);
      if (It == Demand.end() || It->second < D)
        Demand[E.To] = D;
    }
  }
  return Demand;
}

/// `true` when block `W` beats or ties block `K`: higher demand wins,
/// ties go to the earlier start, then to the larger end.
bool beatsOrTies(const Function &F, const std::map<Va, uint8_t> &Demand,
                 size_t W, size_t K) {
  auto key = [&](size_t I) {
    uint8_t D = 0;
    auto It = Demand.find(F.Blocks[I].Start);
    if (It != Demand.end())
      D = It->second;
    return std::make_tuple(D, F.Blocks[I].Start, F.Blocks[I].End);
  };
  // Earlier start wins ties, so compare starts in reverse.
  auto Kw = key(W);
  auto Kk = key(K);
  if (std::get<0>(Kw) != std::get<0>(Kk))
    return std::get<0>(Kw) > std::get<0>(Kk);
  if (F.Blocks[W].Start != F.Blocks[K].Start)
    return F.Blocks[W].Start < F.Blocks[K].Start;
  return F.Blocks[W].End >= F.Blocks[K].End;
}

bool foreignAt(const OverlapEnv &Env, Va V) {
  return Env.ForeignVeto && Env.ForeignVeto(V);
}

/// Linear redecode of one orphan root, mirroring the unbounded
/// `decode_block` path: stop at existing code, data, inline data,
/// unmapped or foreign bytes; direct branch targets are queued so the
/// walk follows local flow like `drain_queue` does.
void decodeOrphanRoot(Function &F, const Image &Img, const Disasm &Dis,
                      const OverlapEnv &Env, Va Root, std::set<Va> &BlockStarts,
                      std::set<Va> &InstrStarts, std::map<Va, Va> &Ranges,
                      std::vector<Va> &Worklist) {
  if (BlockStarts.count(Root) || InstrStarts.count(Root))
    return;
  if (!Img.isExecutable(Root) || vaInData(F.DataRanges, Root) ||
      foreignAt(Env, Root) || midInstr(Ranges, Root))
    return;
  if (Env.DeadStarts && Env.DeadStarts->count(Root))
    return;

  Va Ip = Root;
  size_t First = F.Instrs.size();
  std::vector<Edge> Edges;
  BlockEnd EndKind = BlockEnd::EndOfFunction;
  bool Done = false;

  auto pushEdge = [&](Va To, EdgeKind Kind) {
    Edge E;
    E.To = To;
    E.Kind = Kind;
    Edges.push_back(std::move(E));
  };

  while (!Done) {
    if (Ip != Root) {
      bool Known = BlockStarts.count(Ip) || InstrStarts.count(Ip);
      bool Data = vaInData(F.DataRanges, Ip);
      bool Mid = midInstr(Ranges, Ip);
      bool Foreign = foreignAt(Env, Ip) || Img.inInlineData(Ip) ||
                     !Img.isBacked(Ip) || !Img.isExecutable(Ip);
      if (Mid || Data) {
        EndKind = BlockEnd::EndOfFunction;
        break;
      }
      if (Known || Foreign) {
        EndKind = Foreign ? BlockEnd::TailCall : BlockEnd::Fallthrough;
        pushEdge(Ip, Foreign ? EdgeKind::TailCall : EdgeKind::Fallthrough);
        if (!Foreign)
          Worklist.push_back(Ip);
        break;
      }
    }
    std::optional<Instr> Got = Dis.decodeOne(Ip);
    if (!Got) {
      EndKind = BlockEnd::EndOfFunction;
      break;
    }
    Instr I = *Got;
    Va Next = I.nextVa();
    InstrStarts.insert(Ip);
    Ranges[Ip] = Next;
    F.Instrs.push_back(I);
    Ip = Next;
    switch (I.Flow) {
    case FlowKind::Return:
      EndKind = BlockEnd::Return;
      Done = true;
      break;
    case FlowKind::Trap:
      EndKind = BlockEnd::Noreturn;
      Done = true;
      break;
    case FlowKind::Call:
      if (I.HasTarget)
        F.CallTargets.push_back(I.Target);
      break;
    case FlowKind::CondBranch: {
      EndKind = BlockEnd::Branch;
      if (I.HasTarget) {
        if (foreignAt(Env, I.Target)) {
          pushEdge(I.Target, EdgeKind::TailCall);
        } else {
          pushEdge(I.Target, EdgeKind::CondTaken);
          Worklist.push_back(I.Target);
        }
      }
      pushEdge(Ip, EdgeKind::CondNotTaken);
      Worklist.push_back(Ip);
      Done = true;
      break;
    }
    case FlowKind::Branch:
      if (I.HasTarget) {
        if (foreignAt(Env, I.Target)) {
          EndKind = BlockEnd::TailCall;
          pushEdge(I.Target, EdgeKind::TailCall);
        } else {
          EndKind = BlockEnd::Branch;
          pushEdge(I.Target, EdgeKind::Jump);
          Worklist.push_back(I.Target);
        }
      } else {
        EndKind = BlockEnd::Branch;
      }
      Done = true;
      break;
    case FlowKind::IndirectBranch:
      EndKind = BlockEnd::UnresolvedIndirect;
      Done = true;
      break;
    case FlowKind::None:
      break;
    }
  }

  if (F.Instrs.size() == First)
    return;
  BasicBlock B;
  B.Start = Root;
  B.End = F.Instrs.back().nextVa();
  B.InstrsBegin = First;
  B.InstrsEnd = F.Instrs.size();
  B.Edges = std::move(Edges);
  B.EndKind = EndKind;
  if (B.End > F.End)
    F.End = B.End;
  F.Blocks.push_back(std::move(B));
  BlockStarts.insert(Root);
}

// No helper needed: callers index `Ranges` by start VA directly.

} // end anonymous namespace

void resolveOverlaps(Function &F, const Image &Img, const Disasm &Dis,
                     const TargetHooks &Hooks, const OverlapEnv &Env) {
  (void)Hooks;
  if (F.Blocks.empty())
    return;
  std::sort(F.Blocks.begin(), F.Blocks.end(), [](const BasicBlock &A,
                                                 const BasicBlock &B) {
    return A.Start < B.Start || (A.Start == B.Start && A.End < B.End);
  });

  std::set<Va> WeakSrc = weakSources(F, Env.WeakIps);
  std::set<Va> Strong = strongStarts(F, WeakSrc);
  std::map<Va, uint8_t> Demand = demandOf(F, Strong, WeakSrc);

  // Pairwise arbitration: a block only loses to a stronger survivor it
  // actually overlaps. Merging transitively-connected groups into one
  // winner lets a single bogus bridge decode across real code kill
  // unrelated real blocks on both ends.
  std::vector<char> Keep(F.Blocks.size(), 1);
  std::vector<size_t> Kept;
  Kept.reserve(F.Blocks.size());
  for (size_t K = 0; K < F.Blocks.size(); ++K) {
    while (!Kept.empty() && F.Blocks[Kept.back()].End > F.Blocks[K].Start) {
      if (beatsOrTies(F, Demand, Kept.back(), K))
        break;
      Keep[Kept.back()] = 0;
      Kept.pop_back();
    }
    if (!Kept.empty() && F.Blocks[Kept.back()].End > F.Blocks[K].Start) {
      Keep[K] = 0;
      continue;
    }
    Kept.push_back(K);
  }
  if (llvm::all_of(Keep, [](char K) { return K; }))
    return;

  // Rebuild `Instrs` from kept blocks only, remapping index ranges.
  std::vector<Instr> Old = std::move(F.Instrs);
  F.Instrs.clear();
  std::vector<BasicBlock> Survivors;
  Survivors.reserve(F.Blocks.size());
  std::vector<Va> Dead;
  for (size_t I = 0; I < F.Blocks.size(); ++I) {
    if (!Keep[I]) {
      Dead.push_back(F.Blocks[I].Start);
      continue;
    }
    BasicBlock B = F.Blocks[I];
    size_t NS = F.Instrs.size();
    for (size_t J = B.InstrsBegin; J < B.InstrsEnd; ++J)
      F.Instrs.push_back(Old[J]);
    B.InstrsBegin = NS;
    B.InstrsEnd = F.Instrs.size();
    Survivors.push_back(std::move(B));
  }
  if (Env.DeadStarts)
    Env.DeadStarts->insert(Dead.begin(), Dead.end());

  std::set<Va> InstrStarts;
  std::map<Va, Va> Ranges;
  for (const Instr &I : F.Instrs) {
    InstrStarts.insert(I.Addr);
    Ranges[I.Addr] = I.nextVa();
  }

  // Edges into dropped blocks (or leaders that never materialized)
  // become TailCall. An edge into bytes no survivor covers is an orphan:
  // arbitration freed those bytes, so the caller decodes the target
  // again and restores the edge.
  std::set<Va> Live;
  for (const BasicBlock &B : Survivors)
    Live.insert(B.Start);
  std::vector<std::tuple<Va, Va, EdgeKind>> Orphans;
  for (BasicBlock &B : Survivors) {
    for (Edge &E : B.Edges) {
      if (Live.count(E.To) || E.Kind == EdgeKind::TailCall)
        continue;
      if (!Env.Bounded && !midInstr(Ranges, E.To) && isOrphanKind(E.Kind))
        Orphans.emplace_back(B.Start, E.To, E.Kind);
      E.Kind = EdgeKind::TailCall;
    }
  }
  F.Blocks = std::move(Survivors);

  // Decode again each orphan edge target that arbitration left without a
  // block. A restored edge keeps its kind; a target that still does not
  // decode stays a TailCall.
  if (!Orphans.empty() && !Env.Bounded) {
    std::set<Va> BlockStarts = Live;
    std::vector<Va> Worklist;
    for (const auto &[Src, To, Kind] : Orphans) {
      (void)Src;
      (void)Kind;
      if (Img.isExecutable(To) && !vaInData(F.DataRanges, To) &&
          !foreignAt(Env, To))
        Worklist.push_back(To);
    }
    // Deduplicate seeds; the walk itself guards repeats.
    std::sort(Worklist.begin(), Worklist.end(),
              [](Va A, Va B) { return A.Value < B.Value; });
    Worklist.erase(std::unique(Worklist.begin(), Worklist.end(),
                               [](Va A, Va B) { return A == B; }),
                   Worklist.end());
    while (!Worklist.empty()) {
      Va Root = Worklist.back();
      Worklist.pop_back();
      decodeOrphanRoot(F, Img, Dis, Env, Root, BlockStarts, InstrStarts,
                       Ranges, Worklist);
    }
    std::set<Va> NewLive;
    for (const BasicBlock &B : F.Blocks)
      NewLive.insert(B.Start);
    for (const auto &[Src, To, Kind] : Orphans) {
      if (!NewLive.count(To))
        continue;
      for (BasicBlock &B : F.Blocks) {
        if (B.Start != Src)
          continue;
        for (Edge &E : B.Edges) {
          if (E.To == To && E.Kind == EdgeKind::TailCall)
            E.Kind = Kind;
        }
        break;
      }
    }
  }
}

void resolveOverlaps(Function &F, const Image &Img, const Disasm &Dis,
                     const TargetHooks &Hooks) {
  OverlapEnv Env;
  resolveOverlaps(F, Img, Dis, Hooks, Env);
}

void pruneDataInstrs(Function &F, const Image &Img) {
  (void)Img;
  if (F.DataRanges.empty())
    return;
  auto Overlaps = [&](Va A, Va B) {
    return instrOverlapsData(F.DataRanges, A, B);
  };
  bool Any = false;
  for (const Instr &I : F.Instrs) {
    if (Overlaps(I.Addr, I.nextVa())) {
      Any = true;
      break;
    }
  }
  if (!Any)
    return;

  // A span marked after those bytes were already decoded leaves stale
  // claims: bogus table entries decoded as code before resolution bound
  // the enumeration. Blocks keep only their prefix up to the first
  // overlapping instruction; a fully covered block dies and its start
  // stays dead so nothing re-decodes the straddle.
  std::vector<Instr> Old = std::move(F.Instrs);
  F.Instrs.clear();
  F.Instrs.reserve(Old.size());
  std::vector<BasicBlock> Survivors;
  Survivors.reserve(F.Blocks.size());
  // Dead starts are reported through the Env overload; the plain
  // signature has no out-set, so they die with the block.
  for (BasicBlock B : F.Blocks) {
    size_t OrigEnd = B.InstrsEnd;
    size_t KeepEnd = OrigEnd;
    for (size_t J = B.InstrsBegin; J < OrigEnd; ++J) {
      if (Overlaps(Old[J].Addr, Old[J].nextVa())) {
        KeepEnd = J;
        break;
      }
    }
    if (KeepEnd == B.InstrsBegin)
      continue;
    size_t NS = F.Instrs.size();
    for (size_t J = B.InstrsBegin; J < KeepEnd; ++J)
      F.Instrs.push_back(Old[J]);
    B.InstrsBegin = NS;
    B.InstrsEnd = F.Instrs.size();
    B.End = F.Instrs.back().nextVa();
    if (KeepEnd != OrigEnd) {
      // Cut before its terminator: edges and flow died with it.
      B.Edges.clear();
      B.EndKind = BlockEnd::EndOfFunction;
    }
    Survivors.push_back(std::move(B));
  }
  std::set<Va> Live;
  for (const BasicBlock &B : Survivors)
    Live.insert(B.Start);
  for (BasicBlock &B : Survivors) {
    for (Edge &E : B.Edges) {
      if (!Live.count(E.To) && E.Kind != EdgeKind::TailCall)
        E.Kind = EdgeKind::TailCall;
    }
  }
  if (!Survivors.empty()) {
    Va MaxEnd = Survivors[0].End;
    for (const BasicBlock &B : Survivors)
      if (B.End > MaxEnd)
        MaxEnd = B.End;
    F.End = MaxEnd;
  }
  F.Blocks = std::move(Survivors);
}

void buildChunks(Function &F, llvm::ArrayRef<std::pair<Va, Va>> OwnRanges) {
  // Chunks: merge adjacent blocks, clipped to the function's own `.pdata`
  // region (the primary range plus every chained range). Unwind regions
  // end where the epilogue ends; bytes outside them (dead zones:
  // strings, filler, stale bytes) belong to no function even when a
  // fallthrough decode runs over them.
  std::vector<std::pair<Va, Va>> Own(OwnRanges.begin(), OwnRanges.end());
  std::sort(Own.begin(), Own.end());
  std::sort(F.Blocks.begin(), F.Blocks.end(),
            [](const BasicBlock &A, const BasicBlock &B) {
              return A.Start < B.Start;
            });
  std::vector<std::pair<Va, Va>> Chunks;
  for (const BasicBlock &B : F.Blocks) {
    std::vector<std::pair<Va, Va>> Pieces;
    if (Own.empty()) {
      Pieces.emplace_back(B.Start, B.End);
    } else {
      for (const auto &[RA, RB] : Own) {
        Va S = B.Start > RA ? B.Start : RA;
        Va E = B.End < RB ? B.End : RB;
        if (S < E)
          Pieces.emplace_back(S, E);
      }
    }
    for (const auto &[S, E] : Pieces) {
      if (!Chunks.empty() && S <= Chunks.back().second) {
        if (E > Chunks.back().second)
          Chunks.back().second = E;
        continue;
      }
      Chunks.emplace_back(S, E);
    }
  }
  F.Chunks = std::move(Chunks);

  std::sort(F.DataRanges.begin(), F.DataRanges.end());
  F.DataRanges.erase(std::unique(F.DataRanges.begin(), F.DataRanges.end()),
                     F.DataRanges.end());

  F.UnresolvedIndirects.clear();
  for (const BasicBlock &B : F.Blocks) {
    if (B.EndKind != BlockEnd::UnresolvedIndirect)
      continue;
    if (B.InstrsEnd == B.InstrsBegin || B.InstrsEnd > F.Instrs.size())
      continue;
    F.UnresolvedIndirects.push_back(F.Instrs[B.InstrsEnd - 1].Addr);
  }
  std::sort(F.CallTargets.begin(), F.CallTargets.end(),
            [](Va A, Va B) { return A.Value < B.Value; });
  F.CallTargets.erase(std::unique(F.CallTargets.begin(), F.CallTargets.end(),
                                  [](Va A, Va B) { return A == B; }),
                      F.CallTargets.end());

  // A TailCall edge into the middle of a surviving instruction is a
  // stale claim, not a call: drop it. A TailCall block left with no
  // edges pointed nowhere real, so it is just out of bytes.
  std::map<Va, Va> Ranges;
  for (const Instr &I : F.Instrs)
    Ranges[I.Addr] = I.nextVa();
  for (BasicBlock &B : F.Blocks) {
    std::vector<Edge> Kept;
    Kept.reserve(B.Edges.size());
    for (const Edge &E : B.Edges) {
      if (E.Kind == EdgeKind::TailCall && midInstr(Ranges, E.To))
        continue;
      Kept.push_back(E);
    }
    B.Edges = std::move(Kept);
    if (B.Edges.empty() && B.EndKind == BlockEnd::TailCall)
      B.EndKind = BlockEnd::EndOfFunction;
  }
}

} // namespace revenant::analysis_detail
