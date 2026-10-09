//===- lib/Analysis/Candidates.cpp - Function-start candidates -----------===//
//
// Port of the candidate-collection half of crates/analysis/src/lib.rs
// `Program::analyze`: strong-start collection (entry, executable
// non-forwarder exports, TLS callbacks, primary `.pdata` begins), the
// initial weak set (relocation targets), and the weak-function rejection
// predicate the convergence loop applies.
//
// Seeding and rejection stay free of Builder state: the orchestration owns
// the fixpoint (strong descent, weak batches, `pending`, `built`/`rejected`,
// data claims) and calls these helpers. Ordering is deterministic: seed
// sets are `std::set<Va>`, the predicates are pure.
//
//===----------------------------------------------------------------------===//

#include "AnalysisImpl.h"

#include "revenant/Analysis/Padding.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace revenant {
namespace analysis_detail {
/// End of the pointer-table run at `V`, if `V` opens one: consecutive
/// 8-byte slots that are each an absolute executable VA, zero, or all-ones.
/// Mirrors `analysis::pointer_table_end`.
std::optional<Va> pointerTableEnd(const Image &Img, Va V) {
  Va End(V.Value & ~7ULL);
  uint32_t Good = 0;
  for (;;) {
    std::optional<uint64_t> Slot = Img.readU64(End);
    if (!Slot)
      break;
    if (*Slot == 0 || *Slot == UINT64_MAX) {
      ++Good;
    } else if (*Slot < (1ULL << 47) && Img.isExecutable(Va(*Slot))) {
      // A relocated absolute pointer into executable code. Kernel
      // addresses (>= 2^47) never appear in a user PE.
      ++Good;
    } else {
      break;
    }
    End = End.wrappingAdd(8);
  }
  if (Good >= 3 && V < End)
    return End;
  return std::nullopt;
}

namespace {

/// True when every backed byte of `[A, B)` is zero. Unmapped bytes read
/// back empty and count as zero, matching `data_at(..).all(zero)` with
/// `unwrap_or(true)`.
bool chunkAllZero(const Image &Img, Va A, Va B) {
  uint64_t Len = (B > A) ? B - A : 0;
  // The min mirrors the reference's `usize::MAX` clamp; the length always
  // fits on 64-bit hosts.
  size_t Want = (size_t)std::min<uint64_t>(Len, SIZE_MAX);
  llvm::ArrayRef<uint8_t> Bytes = Img.dataAt(A, Want);
  return std::all_of(Bytes.begin(), Bytes.end(),
                     [](uint8_t X) { return X == 0; });
}

} // end anonymous namespace
std::set<Va> collectStrongStarts(const PeFile &Pe, const Image &Img) {
  std::set<Va> Seeds;
  Seeds.insert(Pe.EntryPoint);
  for (const Export &E : Pe.Exports) {
    if (!E.Forwarder && Img.isExecutable(E.ExportVa))
      Seeds.insert(E.ExportVa);
  }
  Seeds.insert(Pe.TlsCallbacks.begin(), Pe.TlsCallbacks.end());
  for (const RuntimeFunction &R : Pe.Pdata) {
    if (!R.ChainedTo)
      Seeds.insert(R.Begin);
  }
  return Seeds;
}

std::set<Va> collectInitialWeakCandidates(const PeFile &Pe) {
  return std::set<Va>(Pe.RelocTargets.begin(), Pe.RelocTargets.end());
}

bool rejectWeakFunction(const Image &Img, const TargetHooks &Hooks,
                        const Function &F) {
  // All-zero bytes decode as `add [rax],al` but are never real code
  // (pointer-table zeros seeded as weak targets).
  bool AllZero = std::all_of(
      F.Chunks.begin(), F.Chunks.end(), [&](const std::pair<Va, Va> &C) {
        return chunkAllZero(Img, C.first, C.second);
      });
  if (AllZero)
    return true;
  // Filler-only function: the candidate pointed into padding, which the
  // padding decoder owns. Strict `{nop, int3}` set, `xchg` excluded.
  return std::all_of(F.Instrs.begin(), F.Instrs.end(),
                     [&](const Instr &I) { return Hooks.isNopOrInt3(I); });
}

} // namespace analysis_detail
} // namespace revenant
