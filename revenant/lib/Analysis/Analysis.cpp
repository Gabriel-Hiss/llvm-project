#include "revenant/Analysis/Analysis.h"

#include "AnalysisImpl.h"
#include "revenant/Disasm/Disasm.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <map>
#include <set>

namespace revenant {
namespace {

uint32_t toRva(const Image &Img, Va Address) {
  auto R = Img.rvaOf(Address);
  return R.value_or(UINT32_MAX);
}

EdgeKind edgeKind(EdgeTag Tag) {
  switch (Tag) {
  case EdgeTag::Fallthrough:
    return EdgeKind::Fallthrough;
  case EdgeTag::Jump:
    return EdgeKind::Jump;
  case EdgeTag::CondTaken:
    return EdgeKind::CondTaken;
  case EdgeTag::CondNotTaken:
    return EdgeKind::CondNotTaken;
  case EdgeTag::Switch:
    return EdgeKind::Switch;
  case EdgeTag::SwitchDefault:
    return EdgeKind::SwitchDefault;
  case EdgeTag::TailCall:
    return EdgeKind::TailCall;
  }
  llvm_unreachable("unknown edge tag");
}

EdgeTag edgeTag(EdgeKind Kind) {
  switch (Kind) {
  case EdgeKind::Fallthrough:
    return EdgeTag::Fallthrough;
  case EdgeKind::Jump:
    return EdgeTag::Jump;
  case EdgeKind::CondTaken:
    return EdgeTag::CondTaken;
  case EdgeKind::CondNotTaken:
    return EdgeTag::CondNotTaken;
  case EdgeKind::Switch:
    return EdgeTag::Switch;
  case EdgeKind::SwitchDefault:
    return EdgeTag::SwitchDefault;
  case EdgeKind::TailCall:
    return EdgeTag::TailCall;
  }
  llvm_unreachable("unknown edge kind");
}

} // namespace

std::optional<Va> callSiteTarget(const Instr &I) {
  if (I.Flow != FlowKind::Call)
    return std::nullopt;
  if (I.HasTarget)
    return I.Target;
  if (I.HasMemTarget)
    return I.MemTarget;
  return std::nullopt;
}

std::optional<Va> nearTarget(const Instr &I) {
  return I.HasTarget ? std::optional<Va>(I.Target) : std::nullopt;
}

void insertFunction(Program &P, Function F) {
  if (!P.Img)
    return;
  FuncRecord R;
  R.Start = F.Start;
  R.End = F.End;
  R.Name = std::move(F.Name);
  R.HasPdata = F.HasPdata;
  R.InstrRvas.reserve(F.Instrs.size());
  for (const Instr &I : F.Instrs)
    R.InstrRvas.push_back(toRva(*P.Img, I.Addr));
  R.Sorted.resize(R.InstrRvas.size());
  for (uint32_t I = 0; I < R.Sorted.size(); ++I)
    R.Sorted[I] = I;
  std::stable_sort(R.Sorted.begin(), R.Sorted.end(),
                   [&](uint32_t A, uint32_t B) {
                     return R.InstrRvas[A] < R.InstrRvas[B];
                   });
  R.Blocks.reserve(F.Blocks.size());
  for (const BasicBlock &B : F.Blocks) {
    BlockRec BR;
    BR.Start = toRva(*P.Img, B.Start);
    BR.End = toRva(*P.Img, B.End);
    BR.InstrsBegin = static_cast<uint32_t>(B.InstrsBegin);
    BR.InstrsEnd = static_cast<uint32_t>(B.InstrsEnd);
    BR.EdgesBegin = static_cast<uint32_t>(R.Edges.size());
    BR.EndKind = B.EndKind;
    if (B.SwitchIndex) {
      BR.SwitchIndex = static_cast<uint32_t>(R.SwitchMeta.size());
      R.SwitchMeta.push_back(*B.SwitchIndex);
    }
    for (const Edge &E : B.Edges) {
      const uint32_t EdgeIndex = static_cast<uint32_t>(R.Edges.size());
      EdgeRec ER;
      ER.To = toRva(*P.Img, E.To);
      ER.Kind = edgeTag(E.Kind);
      if (ER.To == UINT32_MAX)
        R.EdgeOut.emplace_back(EdgeIndex, E.To);
      if (E.Kind == EdgeKind::Switch)
        R.SwitchEdges.push_back({EdgeIndex, E.SwitchTable, E.SwitchValues});
      R.Edges.push_back(ER);
    }
    BR.EdgesEnd = static_cast<uint32_t>(R.Edges.size());
    R.Blocks.push_back(BR);
  }
  R.Chunks = std::move(F.Chunks);
  R.UnresolvedIndirects = std::move(F.UnresolvedIndirects);
  R.NoreturnCalls = std::move(F.NoreturnCalls);
  R.CallTargets = std::move(F.CallTargets);
  R.ExtraRoots = std::move(F.ExtraRoots);
  R.Orphan = F.Orphan;
  R.IsNoreturn = F.IsNoreturn;
  P.Functions.insert_or_assign(R.Start, std::move(R));
}

Function materialize(const Program &P, const FuncRecord &R) {
  Function F;
  F.Start = R.Start;
  F.End = R.End;
  F.Name = R.Name;
  F.HasPdata = R.HasPdata;
  F.Chunks = R.Chunks;
  F.UnresolvedIndirects = R.UnresolvedIndirects;
  F.NoreturnCalls = R.NoreturnCalls;
  F.CallTargets = R.CallTargets;
  F.ExtraRoots = R.ExtraRoots;
  F.Orphan = R.Orphan;
  F.IsNoreturn = R.IsNoreturn;
  if (!P.Img)
    return F;
  auto ExpectedDis = Disasm::create(*P.Img);
  if (!ExpectedDis) {
    llvm::consumeError(ExpectedDis.takeError());
    return F;
  }
  Disasm Dis = std::move(*ExpectedDis);
  F.Instrs.reserve(R.InstrRvas.size());
  for (Rva RVA : R.InstrRvas) {
    auto I = Dis.decodeOne(P.Img->vaOfRva(RVA));
    if (I)
      F.Instrs.push_back(std::move(*I));
    else
      F.Instrs.emplace_back();
  }
  std::map<uint32_t, const SwitchEdge *> SwitchPayloads;
  for (const SwitchEdge &S : R.SwitchEdges)
    SwitchPayloads.emplace(S.Edge, &S);
  for (const BlockRec &BR : R.Blocks) {
    BasicBlock B;
    B.Start = P.Img->vaOfRva(BR.Start);
    B.End = P.Img->vaOfRva(BR.End);
    B.InstrsBegin = BR.InstrsBegin;
    B.InstrsEnd = BR.InstrsEnd;
    B.EndKind = BR.EndKind;
    if (BR.SwitchIndex != UINT32_MAX && BR.SwitchIndex < R.SwitchMeta.size())
      B.SwitchIndex = R.SwitchMeta[BR.SwitchIndex];
    for (uint32_t I = BR.EdgesBegin; I < BR.EdgesEnd; ++I) {
      Edge E;
      E.To = R.edgeTo(*P.Img, I);
      E.Kind = edgeKind(R.Edges[I].Kind);
      if (auto It = SwitchPayloads.find(I); It != SwitchPayloads.end()) {
        E.SwitchTable = It->second->Table;
        E.SwitchValues = It->second->Values;
      }
      B.Edges.push_back(std::move(E));
    }
    F.Blocks.push_back(std::move(B));
  }
  return F;
}

std::optional<Function> function(const Program &P, Va Start) {
  auto It = P.Functions.find(Start);
  if (It == P.Functions.end())
    return std::nullopt;
  return materialize(P, It->second);
}

const FuncRecord *functionAt(const Program &P, Va Address) {
  for (const auto &[Start, F] : P.Functions) {
    (void)Start;
    for (const auto &[A, B] : F.Chunks)
      if (Address >= A && Address < B)
        return &F;
  }
  return nullptr;
}

const PaddingRun *paddingAt(const Program &P, Rva RVA) {
  auto It = llvm::partition_point(P.Padding, [RVA](const PaddingRun &Run) {
    return Run.End <= RVA;
  });
  return It != P.Padding.end() && RVA >= It->Start ? &*It : nullptr;
}

llvm::Expected<std::vector<std::pair<uint32_t, uint32_t>>>
instructionItems(const Program &P) {
  if (!P.Img)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "program has no image");
  auto ExpectedDis = Disasm::create(*P.Img);
  if (!ExpectedDis)
    return ExpectedDis.takeError();
  Disasm Dis = std::move(*ExpectedDis);
  std::vector<std::pair<uint32_t, uint32_t>> Out;
  for (const auto &[Start, Record] : P.Functions) {
    (void)Start;
    for (Rva RVA : Record.InstrRvas) {
      auto I = Dis.decodeOne(P.Img->vaOfRva(RVA));
      if (!I)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "cannot decode instruction at RVA 0x%x",
                                       RVA);
      Out.emplace_back(RVA, I->Size);
    }
  }
  for (const PaddingRun &Run : P.Padding)
    for (Rva RVA : Run.Instrs) {
      auto I = Dis.decodeOne(P.Img->vaOfRva(RVA));
      if (!I)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "cannot decode padding at RVA 0x%x",
                                       RVA);
      Out.emplace_back(RVA, I->Size);
    }
  llvm::sort(Out);
  Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
  return Out;
}

} // namespace revenant
