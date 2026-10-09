//==- lib/CAPI/AnalysisFallback.cpp - .pdata-seeded discovery -------------==//
//
// Fallback analysis used only while no RevenantAnalysis library is present:
// seeds functions from .pdata runtime entries, exports, the entry point,
// TLS callbacks and direct call targets discovered during decode, then
// decodes each body and builds a direct-branch CFG. Indirect jumps stay
// unresolved (jump tables need the real TargetHooks shapes). Marked as a
// stub stage by Session; replaced by the real analysis port when it lands.
//
//===----------------------------------------------------------------------===//

#include "revenant/Engine/Session.h"

#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"

using namespace llvm;

namespace revenant {

namespace {

/// One in-progress function being decoded.
struct Builder {
  Va Bound; // exclusive decode bound
  std::map<Va, std::vector<Instr>> Blocks; // block start -> decoded instrs
  std::map<Va, std::vector<Edge>> Edges;
  std::map<Va, BlockEnd> Ends;
  std::set<Va> BlockStarts;
  std::vector<Va> CallTargets;
  std::vector<Va> Unresolved;
  /// Every VA covered by a decoded block of this function.
  std::vector<std::pair<Va, Va>> Claimed;

  bool inClaimed(Va Addr) const {
    for (auto [S, E] : Claimed)
      if (Addr >= S && Addr < E)
        return true;
    return false;
  }
};

/// Decode one function body by recursive descent over direct branches.
/// `Bound` is the .pdata end for pdata seeds, else the next known code
/// boundary. Blocks never cross `Bound` and never enter inline data.
void decodeBody(Disasm &D, const Image &Img, Va Start, Va Bound,
                Builder &B, const std::set<Va> &OtherFuncStarts) {
  std::deque<Va> Queue;
  Queue.push_back(Start);
  B.BlockStarts.insert(Start);
  while (!Queue.empty()) {
    Va Ip = Queue.front();
    Queue.pop_front();
    if (Ip < Start || Ip >= Bound || B.inClaimed(Ip) ||
        !Img.isExecutable(Ip) || Img.inInlineData(Ip))
      continue;

    std::vector<Instr> Is;
    std::vector<Edge> Es;
    BlockEnd End = BlockEnd::EndOfFunction;
    Va Cur = Ip;
    for (;;) {
      if (Cur >= Bound || Img.inInlineData(Cur)) {
        End = BlockEnd::EndOfFunction;
        break;
      }
      // Stop before flowing into another function's start or into a block
      // another path already decoded (split point).
      if (Cur != Ip && (OtherFuncStarts.count(Cur) || B.BlockStarts.count(Cur))) {
        Es.push_back({Cur, EdgeKind::Fallthrough, Va(), {}});
        End = BlockEnd::Fallthrough;
        break;
      }
      std::optional<Instr> I = D.decodeOne(Cur);
      if (!I) {
        End = BlockEnd::EndOfFunction;
        break;
      }
      Va Next = I->nextVa();
      Is.push_back(*I);
      switch (I->Flow) {
      case FlowKind::None:
        Cur = Next;
        continue;
      case FlowKind::Call:
        if (I->HasTarget)
          B.CallTargets.push_back(I->Target);
        Cur = Next;
        continue;
      case FlowKind::CondBranch:
        Es.push_back({I->Target, EdgeKind::CondTaken, Va(), {}});
        Es.push_back({Next, EdgeKind::CondNotTaken, Va(), {}});
        End = BlockEnd::Branch;
        break;
      case FlowKind::Branch: {
        Va T = I->Target;
        if (OtherFuncStarts.count(T) || T < Start || T >= Bound)
          Es.push_back({T, EdgeKind::TailCall, Va(), {}});
        else
          Es.push_back({T, EdgeKind::Jump, Va(), {}});
        End = Es.back().Kind == EdgeKind::TailCall ? BlockEnd::TailCall
                                                   : BlockEnd::Branch;
        break;
      }
      case FlowKind::IndirectBranch:
        B.Unresolved.push_back(Cur);
        End = BlockEnd::UnresolvedIndirect;
        break;
      case FlowKind::Return:
        End = BlockEnd::Return;
        break;
      case FlowKind::Trap:
        End = BlockEnd::Noreturn;
        break;
      }
      break; // instruction ended the block
    }
    if (Is.empty())
      continue;
    Va BlockEnd_ = Is.back().nextVa();
    B.Claimed.push_back({Ip, BlockEnd_});
    B.Blocks.emplace(Ip, std::move(Is));
    B.Edges[Ip] = std::move(Es);
    B.Ends[Ip] = End;
    for (const Edge &E : B.Edges[Ip]) {
      if (E.Kind == EdgeKind::TailCall || E.Kind == EdgeKind::Fallthrough)
        continue;
      if (E.To >= Start && E.To < Bound && !B.inClaimed(E.To) &&
          Img.isExecutable(E.To) && !Img.inInlineData(E.To) &&
          B.BlockStarts.insert(E.To).second)
        Queue.push_back(E.To);
    }
    // A fallthrough edge continues into the next block.
    if (End == BlockEnd::Fallthrough) {
      Va T = B.Edges[Ip].back().To;
      if (T >= Start && T < Bound && !B.inClaimed(T) &&
          B.BlockStarts.insert(T).second)
        Queue.push_back(T);
    }
  }
}

} // namespace

Expected<Program> fallbackAnalyze(const PeFile &Pe, Disasm &D) {
  Program Prog;
  Prog.Img = Pe.Img;
  Prog.Strings = std::make_shared<std::map<Va, std::string>>();
  Prog.Names = std::make_shared<std::map<Va, std::string>>();
  Prog.Externs = std::make_shared<std::map<Va, Extern>>();
  const Image &Img = *Pe.Img;

  for (const Import &I : Pe.Imports) {
    Extern X;
    X.Slot = I.IatVa;
    X.Name = I.Name ? (I.Dll + "!" + *I.Name)
                    : (I.Dll + ".#" + std::to_string(I.Ordinal.value_or(0)));
    (*Prog.Externs)[I.IatVa] = std::move(X);
  }
  for (const Export &E : Pe.Exports)
    (*Prog.Names)[E.ExportVa] = E.Name;
  (*Prog.Names)[Pe.EntryPoint] = "entry";
  for (size_t I = 0; I < Pe.TlsCallbacks.size(); ++I)
    (*Prog.Names)[Pe.TlsCallbacks[I]] =
        formatv("TlsCallback_{0}", I).str();

  // Seed set: pdata primary entries (bounded), plus unbounded code seeds.
  std::set<Va> FuncStarts;
  std::map<Va, Va> PdataBound;
  for (const RuntimeFunction &R : Pe.Pdata) {
    if (R.ChainedTo)
      continue; // chained entries share the primary's code
    if (!Img.isExecutable(R.Begin))
      continue;
    FuncStarts.insert(R.Begin);
    PdataBound[R.Begin] = R.End;
  }
  std::vector<Va> Loose = {Pe.EntryPoint};
  for (Va T : Pe.TlsCallbacks)
    Loose.push_back(T);
  for (const Export &E : Pe.Exports)
    if (Img.isExecutable(E.ExportVa))
      Loose.push_back(E.ExportVa);
  for (Va R : Pe.RelocTargets)
    if (Img.isExecutable(R))
      Loose.push_back(R);
  for (Va S : Loose)
    FuncStarts.insert(S);

  std::deque<Va> Work(FuncStarts.begin(), FuncStarts.end());
  std::set<Va> Done;
  size_t Iterations = 0;
  while (!Work.empty() && Iterations++ < 200000) {
    Va Start = Work.front();
    Work.pop_front();
    if (!Img.isExecutable(Start) || Img.inInlineData(Start))
      continue;
    // Skip seeds already inside a decoded function body.
    auto Inside = Prog.Functions.upper_bound(Start);
    if (Inside != Prog.Functions.begin()) {
      --Inside;
      if (Inside->first != Start)
        for (auto [S, E] : Inside->second.Chunks)
          if (Start >= S && Start < E) {
            Start = Va();
            break;
          }
      if (Start.Value == 0 && Inside->first.Value != 0)
        continue;
      if (Inside->first == Start && !Inside->second.Blocks.empty())
        continue;
    }
    if (Done.count(Start))
      continue;
    Done.insert(Start);

    // Bound: the pdata end, else the next function start/section end.
    Va Bound;
    if (auto It = PdataBound.find(Start); It != PdataBound.end()) {
      Bound = It->second;
    } else {
      Bound = Start.wrappingAdd(1u << 20);
      if (const Section *S = Img.sectionOf(Start))
        Bound = std::min(Bound, S->end());
      for (Va Other : FuncStarts)
        if (Other > Start && Other < Bound)
          Bound = Other;
    }

    Builder B;
    B.Bound = Bound;
    decodeBody(D, Img, Start, Bound, B, FuncStarts);
    if (B.Blocks.empty())
      continue;

    // Finalize into a FuncRecord. InstrRvas in decode order (blocks were
    // decoded breadth-first; keep map order = sorted, also fine).
    FuncRecord Rec;
    Rec.Start = Start;
    Rec.HasPdata = PdataBound.count(Start) != 0;
    if (auto It = Prog.Names->find(Start); It != Prog.Names->end())
      Rec.Name = It->second;
    else
      Rec.Name = formatv("sub_{0:x-8}", Start.Value).str();
    Rec.CallTargets = std::move(B.CallTargets);
    Rec.UnresolvedIndirects = std::move(B.Unresolved);
    Rec.Orphan = !Rec.HasPdata;

    for (auto &[Bs, Is] : B.Blocks) {
      BlockRec Br;
      auto RvaS = Img.rvaOf(Bs);
      auto RvaE = Img.rvaOf(Is.back().nextVa());
      if (!RvaS || !RvaE)
        continue;
      Br.Start = *RvaS;
      Br.End = *RvaE;
      Br.InstrsBegin = Rec.InstrRvas.size();
      for (const Instr &I : Is) {
        Rec.InstrRvas.push_back(*Img.rvaOf(I.Addr));
      }
      Br.InstrsEnd = Rec.InstrRvas.size();
      Br.EdgesBegin = Rec.Edges.size();
      for (const Edge &E : B.Edges[Bs]) {
        EdgeRec Er;
        Er.Kind = static_cast<EdgeTag>(E.Kind);
        if (auto R = Img.rvaOf(E.To))
          Er.To = *R;
        else {
          Er.To = UINT32_MAX;
          Rec.EdgeOut.push_back(
              {(uint32_t)Rec.Edges.size(), E.To});
        }
        Rec.Edges.push_back(Er);
      }
      Br.EdgesEnd = Rec.Edges.size();
      Br.EndKind = B.Ends[Bs];
      Rec.Blocks.push_back(Br);
    }
    if (Rec.Blocks.empty())
      continue;
    llvm::sort(Rec.Blocks, [](const BlockRec &A, const BlockRec &B2) {
      return A.Start < B2.Start;
    });
    // Chunks: contiguous block ranges.
    Va CS = Img.vaOfRva(Rec.Blocks.front().Start);
    Va CE = Img.vaOfRva(Rec.Blocks.front().End);
    Rec.End = CE;
    for (size_t I = 1; I < Rec.Blocks.size(); ++I) {
      Va S = Img.vaOfRva(Rec.Blocks[I].Start);
      Va E = Img.vaOfRva(Rec.Blocks[I].End);
      Rec.End = std::max(Rec.End, E);
      if (S == CE)
        CE = E;
      else {
        Rec.Chunks.push_back({CS, CE});
        CS = S;
        CE = E;
      }
    }
    Rec.Chunks.push_back({CS, CE});

    Rec.Sorted.resize(Rec.InstrRvas.size());
    for (uint32_t I = 0; I < Rec.InstrRvas.size(); ++I)
      Rec.Sorted[I] = I;
    llvm::sort(Rec.Sorted, [&](uint32_t A, uint32_t B2) {
      return Rec.InstrRvas[A] < Rec.InstrRvas[B2];
    });

    // Feed direct call targets back into discovery.
    for (Va T : Rec.CallTargets)
      if (Img.isExecutable(T) && !Done.count(T) &&
          !Img.inInlineData(T) && FuncStarts.insert(T).second)
        Work.push_back(T);

    Prog.Functions.emplace(Start, std::move(Rec));
  }

  return Prog;
}

Expected<Function> materializeFunction(const PeFile &Pe, Disasm &D,
                                       const FuncRecord &Rec) {
  const Image &Img = *Pe.Img;
  Function F;
  F.Start = Rec.Start;
  F.End = Rec.End;
  F.Name = Rec.Name;
  F.HasPdata = Rec.HasPdata;
  F.Chunks = Rec.Chunks;
  F.UnresolvedIndirects = Rec.UnresolvedIndirects;
  F.NoreturnCalls = Rec.NoreturnCalls;
  F.CallTargets = Rec.CallTargets;
  F.ExtraRoots = Rec.ExtraRoots;
  F.Orphan = Rec.Orphan;
  F.IsNoreturn = Rec.IsNoreturn;

  F.Instrs.reserve(Rec.InstrRvas.size());
  for (Rva R : Rec.InstrRvas) {
    if (auto I = D.decodeOne(Img.vaOfRva(R)))
      F.Instrs.push_back(*I);
  }
  // Decoded instrs may be fewer than recorded RVAs (shouldn't happen:
  // the analyzer decoded them already); the block index ranges still
  // align when every decode succeeded.
  if (F.Instrs.size() != Rec.InstrRvas.size())
    return createStringError(
        formatv("re-decode mismatch in {0}: {1} of {2} instructions",
                Rec.Name, F.Instrs.size(), Rec.InstrRvas.size())
            .str()
            .c_str());

  F.Blocks.reserve(Rec.Blocks.size());
  for (const BlockRec &Br : Rec.Blocks) {
    BasicBlock B;
    B.Start = Img.vaOfRva(Br.Start);
    B.End = Img.vaOfRva(Br.End);
    B.InstrsBegin = Br.InstrsBegin;
    B.InstrsEnd = Br.InstrsEnd;
    B.EndKind = Br.EndKind;
    B.SwitchIndex = Rec.blockSwitchIndex(Br);
    for (uint32_t I = Br.EdgesBegin; I < Br.EdgesEnd; ++I) {
      Edge E;
      E.To = Rec.edgeTo(Img, I);
      E.Kind = static_cast<EdgeKind>(Rec.Edges[I].Kind);
      // Switch payloads live in SwitchEdges keyed by edge index.
      for (const SwitchEdge &SE : Rec.SwitchEdges)
        if (SE.Edge == I) {
          E.SwitchTable = SE.Table;
          E.SwitchValues = SE.Values;
          break;
        }
      B.Edges.push_back(std::move(E));
    }
    F.Blocks.push_back(std::move(B));
  }
  return F;
}

} // namespace revenant
