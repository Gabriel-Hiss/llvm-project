//==- lib/Analysis/StackPtr.cpp - Stack-pointer delta tracking ----------==//
//
// Port of crates/analysis/src/stackptr.rs (`analyze_stack`, `StackInfo`,
// `StackError`). Generic CFG walk; every x86 question goes through
// `TargetHooks` (stackDelta / stackSlots / fpEstablish / restoresSpFromFp /
// movEaxImm / writesSpDynamically, declared in Target/TargetHooks.h by
// LunaDiscovery).
//
// Behavior notes matching the reference:
// - Deltas are recorded per instruction on entry (before its effect).
// - `bump` runs per instruction with the entry delta and the current
//   rbp state, including dynamic-region instructions (which keep the
//   pre-dynamic delta).
// - Frame-pointer establishment is latched once (first establisher wins;
//   later ones are ignored).
// - `chkstk_size` is per-block state: the last `mov eax, imm` in the
//   block feeds a `call __chkstk`. A `__chkstk` call with no preceding
//   constant size marks DynVa instead of failing.
// - A `call` changes nothing (return address is transient); only the
//   `__chkstk` target check can consume/continue it.
// - `leave` / `mov rsp,rbp` restore `delta = rbp_delta` when an fp
//   exists, and skip `apply` for that instruction.
// - Only Fallthrough/Jump/CondTaken/CondNotTaken/SwitchDefault/Switch
//   edges propagate deltas (TailCall edges do not); Return/TailCall/
//   Noreturn block ends propagate nothing.
// - A block reached a second time with a different entry delta fails
//   with Inconsistent. Blocks not present in `F.Blocks` are skipped.
// - Dynamic rsp writes (writesSpDynamically with no simple delta) record
//   the first DynVa and continue; they never fail.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/StackPtr.h"



#include <utility>
#include <vector>

namespace revenant {

std::string StackError::message() const {
  if (Kind == StackErr::Inconsistent)
    return "inconsistent rsp delta " + std::to_string(Have) + " vs " +
           std::to_string(Want) + " at block " + std::to_string(At.Value);
  return "non-constant rsp modification at " + std::to_string(At.Value);
}

namespace {

bool isChkstk(const Program &Prog, Va Target) {
  if (Prog.Externs)
    for (const auto &KV : *Prog.Externs)
      if (KV.second.Slot == Target &&
          KV.second.Name.find("chkstk") != std::string::npos)
        return true;
  if (Prog.Names) {
    auto It = Prog.Names->find(Target);
    if (It != Prog.Names->end() &&
        It->second.find("chkstk") != std::string::npos)
      return true;
  }
  return false;
}

bool propagates(EdgeKind K) {
  switch (K) {
  case EdgeKind::Fallthrough:
  case EdgeKind::Jump:
  case EdgeKind::CondTaken:
  case EdgeKind::CondNotTaken:
  case EdgeKind::SwitchDefault:
  case EdgeKind::Switch:
    return true;
  case EdgeKind::TailCall:
    return false;
  }
  return false;
}

bool terminates(BlockEnd K) {
  return K == BlockEnd::Return || K == BlockEnd::TailCall ||
         K == BlockEnd::Noreturn;
}

} // end anonymous namespace

llvm::Expected<StackInfo> analyzeStack(const Program &Prog, const Function &F,
                                       const StackHooks &Hooks) {
  StackInfo Out;
  std::map<Va, int64_t> BlockIn;
  std::vector<std::pair<Va, int64_t>> Work;
  Work.emplace_back(F.Start, 0);

  while (!Work.empty()) {
    Va Start;
    int64_t InDelta;
    std::tie(Start, InDelta) = Work.back();
    Work.pop_back();

    size_t BI = F.Blocks.size();
    for (size_t I = 0; I < F.Blocks.size(); ++I)
      if (F.Blocks[I].Start == Start) {
        BI = I;
        break;
      }
    if (BI == F.Blocks.size())
      continue;

    auto Known = BlockIn.find(Start);
    if (Known != BlockIn.end()) {
      if (Known->second != InDelta)
        return llvm::createStringError(
            std::make_error_code(std::errc::invalid_argument),
            "inconsistent rsp delta %lld vs %lld at block %llx",
            (long long)Known->second, (long long)InDelta,
            (unsigned long long)Start.Value);
      continue;
    }
    BlockIn.emplace(Start, InDelta);

    const BasicBlock &B = F.Blocks[BI];
    int64_t Delta = InDelta;
    // Last `mov eax, imm` feeding a possible `call __chkstk`.
    std::optional<int64_t> ChkstkSize;

    for (size_t I = B.InstrsBegin; I < B.InstrsEnd; ++I) {
      const Instr &Ins = F.Instrs[I];
      Out.Deltas.emplace(Ins.Addr, Delta);

      // Frame-bounds bookkeeping with the entry delta.
      StackSlots Slots = Hooks.stackSlots(Ins);
      for (const auto &S : Slots.Slots) {
        int64_t Off;
        if (S.first == StackBase::Rsp)
          Off = S.second - Delta;
        else if (Out.RbpDelta)
          Off = S.second - *Out.RbpDelta;
        else
          continue;
        if (Off < -Out.FrameSize)
          Out.FrameSize = -Off;
        if (Off > Out.ArgsSize)
          Out.ArgsSize = Off;
      }

      if (!Out.RbpDelta) {
        FpEstablish Fp = Hooks.fpEstablish(Ins);
        if (Fp.Tag == FpEstablish::Kind::MovRbpRsp)
          Out.RbpDelta = Delta;
        else if (Fp.Tag == FpEstablish::Kind::LeaRbpRsp)
          Out.RbpDelta = Delta - Fp.Disp;
      }

      if (auto Imm = Hooks.movEaxImm(Ins))
        ChkstkSize = *Imm;

      std::optional<Va> CallTarget;
      if (Ins.Flow == FlowKind::Call && Ins.HasTarget)
        CallTarget = Ins.Target;
      if (CallTarget && isChkstk(Prog, *CallTarget)) {
        if (ChkstkSize)
          Delta += *ChkstkSize;
        else if (!Out.DynVa)
          Out.DynVa = Ins.Addr;
        continue;
      }

      // `leave` / `mov rsp, rbp` restore the frame pointer's delta.
      if (Hooks.restoresSpFromFp(Ins) && Out.RbpDelta) {
        Delta = *Out.RbpDelta;
        continue;
      }

      // stackDelta is the persistent per-instruction delta change
      // (Rust apply(): push=+8, sub rsp,imm=+imm); add it to the running
      // delta.
      if (auto D = Hooks.stackDelta(Ins)) {
        Delta += *D;
      } else if (Hooks.writesSpDynamically(Ins)) {
        if (!Out.DynVa)
          Out.DynVa = Ins.Addr;
      }
    }

    if (terminates(B.EndKind))
      continue;
    for (const Edge &E : B.Edges)
      if (propagates(E.Kind))
        Work.emplace_back(E.To, Delta);
  }

  return Out;
}

} // end namespace revenant
