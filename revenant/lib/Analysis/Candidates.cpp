//===- lib/Analysis/Candidates.cpp - Function-start candidates -----------===//
//
// Port of the candidate-collection half of crates/analysis/src/lib.rs
// `Program::analyze`: strong-start collection (entry, executable
// non-forwarder exports, TLS callbacks, primary `.pdata` begins), the
// initial weak set (relocation targets), and the weak-candidate /
// weak-function rejection predicates the convergence loop applies.
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
namespace {

/// True when a sorted, disjoint claim list covers `V`.
/// Mirrors `analysis::claimed_at`.
bool claimedAt(llvm::ArrayRef<std::pair<Va, Va>> Claims, Va V) {
  auto It = std::partition_point(
      Claims.begin(), Claims.end(),
      [V](const std::pair<Va, Va> &R) { return R.first <= V; });
  return It != Claims.begin() && V < (It - 1)->second;
}

/// True when `[S, E)` touches any claimed range: `S` is covered, or a
/// range starts inside. Mirrors `analysis::claims_overlap`.
bool claimsOverlap(llvm::ArrayRef<std::pair<Va, Va>> Claims, Va S, Va E) {
  if (claimedAt(Claims, S))
    return true;
  auto It = std::partition_point(
      Claims.begin(), Claims.end(),
      [S](const std::pair<Va, Va> &R) { return R.first < S; });
  return It != Claims.end() && It->first < E;
}

/// The `.pdata` primary owning `V`, if `V` sits inside a `.pdata` range.
/// `Pdata` maps begin -> (primary, end). Mirrors `analysis::pdata_range`.
std::optional<Va> pdataPrimary(const std::map<Va, std::pair<Va, Va>> &Pdata,
                               Va V) {
  auto It = Pdata.upper_bound(V);
  if (It == Pdata.begin())
    return std::nullopt;
  --It;
  if (V < It->second.second)
    return It->second.first;
  return std::nullopt;
}

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

bool rejectWeakCandidate(const Image &Img, const Disasm &Dis,
                         const TargetHooks &,
                         const std::map<Va, std::pair<Va, Va>> &Pdata,
                         llvm::ArrayRef<std::pair<Va, Va>> Claimed,
                         Va Candidate) {
  // A candidate inside a filler instruction is a thunk entry mid-nop: the
  // build starts at the run's head so the compiler's instruction is listed
  // whole. When the head is already claimed (or the head-to-candidate span
  // overlaps a claim) the candidate is dead; otherwise every check below
  // applies to the head. The orchestration maps to the head itself for the
  // build; normalizing here keeps the verdict identical either way.
  Va Eff = Candidate;
  if (std::optional<Va> H = fillerHead(Dis, Candidate);
      H && *H < Candidate) {
    if (claimedAt(Claimed, *H) || claimsOverlap(Claimed, *H, Candidate))
      return true;
    Eff = *H;
  }
  // A weak candidate starting with a zero byte is data, not code: no
  // compiler starts a function with `add [rax],al`.
  if (auto B = Img.read(Eff, 1); B && !B->empty() && (*B)[0] == 0x00)
    return true;
  // `.ctors`/`.refptr` tables parked in `.text` decode valid-looking but
  // are pointer runs, never code.
  if (pointerTableEnd(Img, Eff))
    return true;
  if (!Img.isExecutable(Eff) || !Img.isBacked(Eff) ||
      Img.inInlineData(Eff))
    return true;
  // Starts strictly inside a primary `.pdata` range belong to that
  // function; only the primary begin itself seeds.
  if (auto Primary = pdataPrimary(Pdata, Eff);
      Primary && *Primary != Eff)
    return true;
  if (claimedAt(Claimed, Eff))
    return true;
  // MC has no "invalid instruction" value: decode failure is the signal.
  // The orchestration also drops candidates whose bytes decode but the
  // Builder flags invalid; that needs Builder state.
  if (!Dis.decodeOne(Eff))
    return true;
  return false;
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
