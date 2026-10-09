//===- lib/Analysis/Orphan.h - Orphan-sweep helpers -------------*- C++ -*-==//
//
// Private header for this slice's translation units. Declares the two
// helpers Discovery's driver calls: `orphanProbeRanges` (executable gap
// carving) and `orphanFunctionOk` (bounded-probe acceptance). Mirrors
// the orphan-sweep range arithmetic and `orphan_ok` in
// crates/analysis/src/lib.rs.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIB_ANALYSIS_ORPHAN_H
#define REVENANT_LIB_ANALYSIS_ORPHAN_H

#include "revenant/Target/TargetHooks.h"

 #include "revenant/Core/Program.h"
 #include "revenant/Core/Va.h"

#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"

namespace revenant {

struct Image;
struct PeFile;

namespace analysis_detail {

/// Executable-section gaps minus `.pdata` ranges, function chunks, data
/// ranges, inline-data barriers and Go blob barriers, clipped to each
/// executable section's backed extent. Each returned `[lo, hi)` is usable
/// directly as a `Builder::attempt` bound.
std::vector<std::pair<Va, Va>>
orphanProbeRanges(const PeFile &Pe, llvm::ArrayRef<Function> Functions);

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
