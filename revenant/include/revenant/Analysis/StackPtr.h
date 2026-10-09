//==- revenant/Analysis/StackPtr.h - Stack-pointer delta tracking -*- C++ -*-==//
//
// Port of crates/analysis/src/stackptr.rs: for every instruction, how far
// `rsp` sits below its value at function entry (`delta >= 0` inside the
// frame). `push`/`pop`/`add`/`sub`/`lea rsp`/`__chkstk` probes are tracked;
// a `call` pushes the return address transiently and does not change the
// delta between instructions. A join whose predecessors disagree on the
// delta makes the function unliftable.
//
// All x86 knowledge (opcode shapes, register identity, memory-operand
// layout) lives behind `TargetHooks`; this header and StackPtr.cpp are
// target-neutral. The x86 side lives in lib/Target/X86/X86StackEffects.cpp
// (free helpers returning exactly the TargetHooks types, wired into
// X86TargetHooks by LunaDiscovery, who owns that file).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_STACKPTR_H
#define REVENANT_ANALYSIS_STACKPTR_H

#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"
#include "revenant/Target/TargetHooks.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Support/Error.h"

namespace revenant {


/// Per-instruction rsp deltas plus frame-pointer info for one function
/// (Rust `analysis::stackptr::StackInfo`).
struct StackInfo {
  /// Instr VA -> bytes of stack between entry rsp and rsp at that point.
  std::map<Va, int64_t> Deltas;
  /// If `mov rbp,rsp` / `lea rbp,[rsp+X]` established a frame pointer:
  /// rbp == entry_rsp - RbpDelta at that point.
  std::optional<int64_t> RbpDelta;
  /// Deepest offset touched via rsp or rbp (frame size).
  int64_t FrameSize = 0;
  /// Highest positive offset touched (incoming stack args region).
  int64_t ArgsSize = 0;
  /// VA of the instruction that modifies rsp non-constantly
  /// (`sub rsp, rax`, `and rsp,-N`, non-const `__chkstk`), if any. The rsp
  /// delta at that instruction is `deltaAt(DynVa)`; afterwards rsp-relative
  /// offsets go through the dynamic alloca.
  std::optional<Va> DynVa;

  /// rsp delta at the given instruction VA (0 when unknown).
  int64_t deltaAt(Va Addr) const {
    auto It = Deltas.find(Addr);
    return It != Deltas.end() ? It->second : 0;
  }

  /// Frame offset of `[rsp + disp]` at instruction `va`: bytes relative to
  /// entry rsp (locals are negative).
  int64_t rspOff(Va Addr, int64_t Disp) const { return Disp - deltaAt(Addr); }

  /// Frame offset of `[rbp + disp]` if an fp exists.
  std::optional<int64_t> rbpOff(int64_t Disp) const {
    if (!RbpDelta)
      return std::nullopt;
    return Disp - *RbpDelta;
  }

  /// Does this instruction execute with a dynamic rsp (at or after the
  /// non-constant rsp modification)?
  bool isDynamicAt(Va Addr) const {
    return DynVa && Addr >= *DynVa;
  }
};

/// Reason a function's stack frame cannot be modeled (Rust
/// `analysis::stackptr::StackError`).
enum class StackErr : uint8_t {
  /// Join predecessors disagree on the delta.
  Inconsistent,
  /// Non-constant rsp modification (informational: analyzeStack records
  /// DynVa and keeps going; only `Inconsistent` fails the call).
  Dynamic,
};

struct StackError {
  StackErr Kind;
  /// Block start (Inconsistent) or instruction VA (Dynamic).
  Va At;
  int64_t Have = 0;
  int64_t Want = 0;

  std::string message() const;
};

/// Walk the CFG computing rsp deltas; records frame bounds (Rust
/// `analysis::analyze_stack`). Fails only on `StackErr::Inconsistent`;
/// dynamic rsp writes record `DynVa` and continue with the pre-dynamic
/// delta for frame bookkeeping.
llvm::Expected<StackInfo> analyzeStack(const Program &Prog, const Function &F,
                                       const TargetHooks &Hooks);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_STACKPTR_H
