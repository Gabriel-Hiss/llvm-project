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
  // Decoder-only fallback for tests that link this slice without the x86
  // hooks. `jmp reg` decodes as JMP64r with a single register operand;
  // memory forms carry base/scale/index/disp/segment, with the index in
  // slot 2 (reg 0 = no index). RIP-relative detection uses the decoded
  // absolute slot instead of a register table: `HasMemTarget` into
  // non-executable bytes is a thunk slot however the base is spelled.
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
      if (M.getNumOperands() == 0 || !M.getOperand(0).isReg())
        return false;
      if (M.getNumOperands() == 1)
        return true; // jmp reg
      // Memory form: opaque only for the thunk slot shape, a mem target
      // into non-executable bytes ([rip+disp] at the IAT, [reg+disp]
      // through an object). The reference rejects any mem target in
      // executable bytes as a failed table, and anything else indexed.
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

OrphanSweep planOrphanSweep(const PeFile &Pe,
                            llvm::ArrayRef<Function> Functions) {
  const Image &Img = *Pe.Img;
  OrphanSweep Plan;
  std::vector<Range> Covered;
  Covered.reserve(Pe.Pdata.size() + Functions.size() * 2);
  for (const RuntimeFunction &R : Pe.Pdata)
    Covered.emplace_back(R.Begin, R.End);
  for (const Function &F : Functions) {
    Covered.insert(Covered.end(), F.Chunks.begin(), F.Chunks.end());
    // Resolved/recognized jump-table spans are data: they block probes
    // and bound the surrounding runs.
    Covered.insert(Covered.end(), F.DataRanges.begin(), F.DataRanges.end());
  }
  const std::vector<Range> Merged = mergeRanges(std::move(Covered));
  for (const Range &R : Merged)
    Plan.Claimed.emplace(R.first, R.second);

  // Recorded data inside executable sections (relocation slots, the Go
  // build-id blob) and Go jump-over-blob spans are never orphan code. They
  // join `Claimed` but not the gap carving, so a probe running into one is
  // bounded by it.
  std::vector<Range> Barriers;
  for (const VaRange &R : Img.InlineData)
    Barriers.emplace_back(R.Start, R.End);
  if (Pe.GoBuildId)
    appendGoBarriers(Functions, Img, Barriers);
  for (const Range &B : Barriers) {
    Va Cur = B.first;
    auto Prev = Plan.Claimed.upper_bound(Cur);
    if (Prev != Plan.Claimed.begin()) {
      --Prev;
      Cur = std::max(Cur, std::min(Prev->second, B.second));
    }
    std::vector<Range> Overlaps;
    for (auto It = Plan.Claimed.lower_bound(Cur);
         It != Plan.Claimed.end() && It->first < B.second; ++It)
      Overlaps.emplace_back(It->first, It->second);
    for (const Range &O : Overlaps) {
      if (Cur < O.first)
        Plan.Claimed[Cur] = O.first;
      Cur = std::min(std::max(Cur, O.second), B.second);
    }
    if (Cur < B.second)
      Plan.Claimed[Cur] = B.second;
  }

  for (const Section &S : Img.Sections) {
    if (!S.Flags.Execute)
      continue;
    const Va Lo = S.SecVa;
    // Only backed bytes are probeable: the mapped tail past `Data` is
    // zero-fill, and running a probe over it manufactures code out of
    // zeros.
    const Va Hi = Lo.wrappingAdd(
        std::min<uint64_t>(S.Vsize, (uint64_t)S.Data.size()));
    Va Cursor = Lo;
    for (const Range &C : Merged) {
      if (C.second <= Cursor || C.first >= Hi)
        continue;
      if (C.first > Cursor)
        Plan.Gaps.emplace_back(Cursor, std::min(C.first, Hi));
      Cursor = std::max(Cursor, C.second);
      if (Cursor >= Hi)
        break;
    }
    if (Cursor < Hi)
      Plan.Gaps.emplace_back(Cursor, Hi);
  }
  return Plan;
}

} // namespace analysis_detail
} // namespace revenant
