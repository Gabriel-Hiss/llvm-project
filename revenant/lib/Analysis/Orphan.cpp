//===- lib/Analysis/Orphan.cpp - Orphan-sweep helpers -------------------===//
//
// Port of the orphan-sweep range arithmetic and `orphan_ok` in
// crates/analysis/src/lib.rs: carve executable-section gaps that no
// `.pdata` range, function chunk, resolved data range, inline-data
// barrier or Go jump-over-blob barrier claims, and validate bounded
// probes against the bound-edge acceptance rule.
//
//===----------------------------------------------------------------------===//

#include "Orphan.h"

#include "revenant/Core/Image.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

#include "llvm/MC/MCInst.h"

namespace revenant {
namespace analysis_detail {
namespace {

using Range = std::pair<Va, Va>;


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

void addClaim(std::vector<Range> &Covered, Va Start, Va End) {
  if (End <= Start)
    return;
  // Covered stays sorted/disjoint with few entries, so a linear merge is
  // the boring correct choice.
  Range R{Start, End};
  auto It = Covered.begin();
  while (It != Covered.end() && It->second < R.first)
    ++It;
  while (It != Covered.end() && It->first <= R.second) {
    R.first = std::min(R.first, It->first);
    R.second = std::max(R.second, It->second);
    It = Covered.erase(It);
  }
  Covered.insert(It, R);
}

/// Jump-over-blob barriers: Go assembly wrappers emit raw `BYTE` data
/// right after an unconditional `jmp` that lands on the function's last
/// instruction. The skipped `[next_ip, target)` span is data the probe
/// must not read. A jump into the middle of the flow is real code and
/// stays probeable.
void appendGoBarriers(llvm::ArrayRef<Function> Functions,
                      const Image &Img, std::vector<Range> &Barriers) {
  for (const Function &F : Functions) {
    if (F.Instrs.empty())
      continue;
    Va LastIp = F.Instrs.front().Addr;
    for (const Instr &I : F.Instrs)
      LastIp = std::max(LastIp, I.Addr);
    for (const Instr &I : F.Instrs) {
      if (I.Flow != FlowKind::Branch || !I.HasTarget)
        continue;
      const Va Next = I.nextVa();
      const Va T = I.Target;
      if (T > Next && T >= LastIp && T <= F.End)
        Barriers.emplace_back(Next, T);
    }
  }
  (void)Img;
}

/// True when `I` is an indirect jump whose target stays unknown even to
/// jump-table recovery. Opaque tail calls read a single pointer:
/// `jmp reg`, `jmp [reg]` / `jmp [reg+disp]` (virtual dispatch through
/// an object), and `jmp [rip+disp]` into data (IAT thunks).
bool indirectTail(const Image &Img, const Instr &I, const TargetHooks &Hooks) {
  return Hooks.isOpaqueTailJmp(Img, I);
}

} // end anonymous namespace

/// Declared in `Orphan.h`; the TargetHooks-taking form. `ProgramAnalyze`
/// wraps this with the real x86 hooks.
bool orphanFunctionOk(const Image &Img, const Function &F, Va BoundEnd,
                      const TargetHooks &Hooks) {
  if (F.Instrs.empty())
    return false;
  std::set<Va> Starts;
  for (const BasicBlock &B : F.Blocks)
    Starts.insert(B.Start);
  if (Starts.empty())
    return false;
  // Map instruction VA -> index for the terminator lookup.
  for (const BasicBlock &B : F.Blocks) {
    bool Terminated = false;
    switch (B.EndKind) {
    case BlockEnd::EndOfFunction:
      // Running out of region is a clean stop only at exactly the probe
      // bound; a mid-gap `EndOfFunction` means the decode hit junk.
      Terminated = B.End == BoundEnd;
      break;
    case BlockEnd::UnresolvedIndirect: {
      const Instr *Term = nullptr;
      for (const Instr &I : F.Instrs)
        if (I.Addr >= B.Start && I.Addr < B.End)
          if (!Term || Term->Addr < I.Addr)
            Term = &I;
      Terminated = Term && Term->Flow == FlowKind::IndirectBranch &&
                   indirectTail(Img, *Term, Hooks);
      break;
    }
    default:
      Terminated = true;
      break;
    }
    if (!Terminated)
      return false;
    for (const Edge &E : B.Edges) {
      if (E.Kind == EdgeKind::TailCall)
        continue;
      if (!Starts.count(E.To))
        return false;
    }
  }
  return true;
}

bool orphanFunctionOk(const Image &Img, const Function &F, Va BoundEnd) {
  // Without target hooks, accept only direct-register jumps and resolved
  // memory slots into non-executable bytes. Other memory forms stay unknown.
  struct LocalHooks final : public TargetHooks {
    llvm::StringRef mnemonic(const Instr &) const override { return ""; }
    bool isFiller(const Instr &) const override { return false; }
    TrapKind trapKind(const Instr &) const override {
      return TrapKind::NotTrap;
    }
    bool isNopOrInt3(const Instr &) const override { return false; }
    bool isOpaqueTailJmp(const Image &Img, const Instr &I) const override {
      if (I.Flow != FlowKind::IndirectBranch)
        return false;
      const llvm::MCInst &M = I.Inst;
      if (M.getNumOperands() == 1 && M.getOperand(0).isReg())
        return true;
      return M.getNumOperands() >= 5 && I.HasMemTarget &&
             !Img.isExecutable(I.MemTarget);
    }
    std::optional<int64_t> stackDelta(const Instr &) const override {
      return std::nullopt;
    }
    StackSlots stackSlots(const Instr &) const override { return {}; }
    FpEstablish fpEstablish(const Instr &) const override { return {}; }
    bool restoresSpFromFp(const Instr &) const override { return false; }
    std::optional<int64_t> movEaxImm(const Instr &) const override {
      return std::nullopt;
    }
    bool writesSpDynamically(const Instr &) const override { return false; }
    std::optional<JumpTableInfo>
    matchJumpTable(const Image &, llvm::ArrayRef<std::pair<Va, Va>>,
                   llvm::ArrayRef<BasicBlock>, llvm::ArrayRef<Instr>, size_t,
                   const std::vector<Va> &, const std::vector<Va> &,
                   const std::map<Va, uint64_t> &) const override {
      return std::nullopt;
    }
  };
  LocalHooks Hooks;
  return orphanFunctionOk(Img, F, BoundEnd, Hooks);
}

std::vector<Range> orphanProbeRanges(const PeFile &Pe,
                                     llvm::ArrayRef<Function> Functions) {
  const Image &Img = *Pe.Img;
  std::vector<Range> Covered;
  Covered.reserve(Pe.Pdata.size() + Functions.size() * 2);
  for (const RuntimeFunction &R : Pe.Pdata)
    addClaim(Covered, R.Begin, R.End);
  for (const Function &F : Functions) {
    for (const Range &C : F.Chunks)
      addClaim(Covered, C.first, C.second);
    // Resolved/recognized jump-table spans are data: they block probes
    // and bound the surrounding runs.
    for (const Range &D : F.DataRanges)
      addClaim(Covered, D.first, D.second);
  }
  // Recorded data inside executable sections (relocation slots, the Go
  // build-id blob) is never orphan code.
  for (const VaRange &R : Img.InlineData)
    addClaim(Covered, R.Start, R.End);
  // Go blob barriers on top of the claims so a barrier overlapping a
  // covered span only fills its unclaimed remainder.
  if (Pe.GoBuildId) {
    std::vector<Range> Barriers;
    appendGoBarriers(Functions, Img, Barriers);
    for (const Range &B : Barriers) {
      if (B.second <= B.first)
        continue;
      // Snapshot: addClaim mutates Covered, so iterate a copy.
      const std::vector<Range> Snap = Covered;
      Va Cur = B.first;
      for (const Range &C : Snap) {
        if (C.second <= Cur)
          continue;
        if (C.first >= B.second)
          break;
        if (Cur < C.first)
          addClaim(Covered, Cur, C.first);
        Cur = std::max(Cur, C.second);
        if (Cur >= B.second)
          break;
      }
      if (Cur < B.second)
        addClaim(Covered, Cur, B.second);
    }
  }

  std::vector<Range> Out;
  for (const Section &S : Img.Sections) {
    if (!S.Flags.Execute)
      continue;
    const Va Lo = S.SecVa;
    // Only backed bytes are probeable: the mapped tail past `Data` is
    // zero-fill, and running a probe over it manufactures code out of
    // zeros.
    const uint64_t Backed =
        std::min<uint64_t>(S.Vsize, (uint64_t)S.Data.size());
    const Va Hi = Lo.wrappingAdd(Backed);
    Va Cursor = Lo;
    for (const Range &C : Covered) {
      if (C.second <= Cursor)
        continue;
      if (C.first >= Hi)
        break;
      if (C.first > Cursor)
        Out.emplace_back(Cursor, std::min(C.first, Hi));
      Cursor = std::max(Cursor, C.second);
      if (Cursor >= Hi)
        break;
    }
    if (Cursor < Hi)
      Out.emplace_back(Cursor, Hi);
  }
  return mergeRanges(std::move(Out));
}

} // namespace analysis_detail
} // namespace revenant
