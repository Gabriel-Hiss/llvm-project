//===- lib/Analysis/Orphan.h - Orphan-sweep helpers -------------*- C++ -*-==//
//
// Private header for this slice's translation units. Declares the helpers
// the orphan sweep in `ProgramAnalyze.cpp` calls: `planOrphanSweep`
// (executable gap carving plus the claimed-byte map) and
// `orphanFunctionOk` (bounded-probe acceptance). Mirrors the orphan-sweep
// range arithmetic and `orphan_ok` in crates/analysis/src/lib.rs.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIB_ANALYSIS_ORPHAN_H
#define REVENANT_LIB_ANALYSIS_ORPHAN_H

#include "revenant/Target/TargetHooks.h"

 #include "revenant/Core/Program.h"
 #include "revenant/Core/Va.h"

#include <map>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"

namespace revenant {

struct Image;
struct PeFile;

namespace analysis_detail {

/// Where an orphan sweep probes and what it must not touch.
struct OrphanSweep {
  /// Executable-section gaps minus `.pdata` ranges, function chunks and
  /// data ranges, clipped to each section's backed extent. Probe each
  /// `[lo, hi)` in turn.
  std::vector<std::pair<Va, Va>> Gaps;
  /// Start -> end of every byte span a probe must skip: the merged
  /// `.pdata`/chunk/data ranges plus inline-data and Go blob barriers.
  /// Probes add the chunks and data ranges they accept.
  std::map<Va, Va> Claimed;
};

OrphanSweep planOrphanSweep(const PeFile &Pe,
                            llvm::ArrayRef<Function> Functions);

/// TargetHooks-taking form: the opaque tail-call test comes from
/// `Hooks.isOpaqueTailJmp`. The driver calls this one.
bool orphanFunctionOk(const Image &Img, const Function &F, Va BoundEnd,
                      const TargetHooks &Hooks);

/// Decoder-only form: the opaque tail-call shapes are detected straight
/// from the MC operands (no register tables needed). Used by tests that
/// link this slice without the x86 hooks.
/// True when a bounded probe of `[.., BoundEnd)` is real code: no empty
/// decode, `EndOfFunction` only at exactly `BoundEnd`, unresolved
/// indirects only in the opaque tail-call shapes, and every non-tail edge
/// landing on a block start. Mirrors `analysis::orphan_ok`.
bool orphanFunctionOk(const Image &Img, const Function &F, Va BoundEnd);

} // namespace analysis_detail
} // namespace revenant

#endif // REVENANT_LIB_ANALYSIS_ORPHAN_H
