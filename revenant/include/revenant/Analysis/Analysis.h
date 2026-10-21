//==- revenant/Analysis/Analysis.h - Program analysis ------------*- C++ -*-==//
//
// Function discovery, CFG building, and the analysis driver. Mirrors
// crates/analysis/src/lib.rs `Program::analyze`: strong seeds (entry,
// exports, TLS, `.pdata`, call targets), weak candidates, the orphan
// sweep, and the fixpoint that glues them together. All target-specific
// shape tests go through `revenant::TargetHooks`.
//
// Sibling slices provide the pieces `analyze` calls:
//   Analysis/JumpTable.h  switch-table resolution      (Swe2JumpTables)
//   Analysis/Noreturn.h   noreturn name table + fixpoint (MuseNoreturn)
//   Analysis/Padding.h    alignment-filler decoding      (MusePadding)
//   Analysis/StackPtr.h   rsp-delta tracking             (MuseStackPtr)
//   Analysis/Xrefs.h      cross-reference collection     (MuseXrefs)
//
//===----------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_ANALYSIS_H
#define REVENANT_ANALYSIS_ANALYSIS_H

#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"
#include "revenant/Object/PeFile.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "llvm/Support/Error.h"

namespace revenant {

class Disasm;
class TargetHooks;
struct PeFile;

/// Run the full analysis over a loaded PE: discovery, CFGs, noreturn
/// propagation, xrefs, strings, padding. Deterministic: the result never
/// depends on scheduling or thread count. Mirrors
/// `analysis::Program::analyze(pe, None)`.
Program analyze(const PeFile &Pe, const TargetHooks &Hooks);

/// The call target of a call instruction: the near target of a direct
/// call, or the IAT slot VA of an indirect `call [rip+x]`. Mirrors
/// `analysis::call_site_target`.
std::optional<Va> callSiteTarget(const Instr &I);

/// The absolute target of a direct branch/call (`I.HasTarget`), if any.
/// Mirrors iced `near_branch_target` use sites.
std::optional<Va> nearTarget(const Instr &I);

/// Re-decode `R`'s instructions at their stored RVAs and rebuild the
/// owned `Function` (blocks, edges, switch payloads). Mirrors
/// `Program::materialize`. `R` must come from `P`.
Function materialize(const Program &P, const FuncRecord &R);

/// The full `Function` for `Start`, re-decoded. Mirrors
/// `Program::function`.
std::optional<Function> function(const Program &P, Va Start);

/// Rebuilds `P.ChunkIndex`: the disjoint elementary intervals between chunk
/// bounds, each owned by the function the linear scan would pick (first in
/// `Functions` order covering it). `analyze` calls it once after all
/// functions are inserted so later parallel `functionAt` calls never race
/// the lazy build; `insertFunction` marks it stale again.
void buildChunkIndex(const Program &P);

/// The function whose code chunks contain `Addr`, if any. Mirrors
/// `Program::function_at`.
const FuncRecord *functionAt(const Program &P, Va Addr);

/// The padding run starting at RVA `R`, if any. Mirrors
/// `Program::padding_at`.
const PaddingRun *paddingAt(const Program &P, Rva R);

/// Fold a built `Function` into `P.Functions` as a compact record.
/// Mirrors `Program::insert_function`.
void insertFunction(Program &P, Function F);

/// Every instruction the listing shows — function code plus padding
/// filler — as `(rva, len)`, sorted and unique. Re-decoding serves the
/// length; failure carries the RVA whose bytes no longer decode.
/// Mirrors `Program::instruction_items`.
llvm::Expected<std::vector<std::pair<uint32_t, uint32_t>>>
instructionItems(const Program &P);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_ANALYSIS_H
