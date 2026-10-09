//==- revenant/Analysis/Xrefs.h - Cross-reference table builder -*- C++ -*-==//
//
// Builds the compact `Xrefs` table in revenant/Core/Program.h from decoded
// instructions. Mirrors `collect_xrefs`/`push_xref` plus the flattening loop
// in crates/analysis/src/lib.rs: direct near-branch calls/jumps and
// RIP-relative memory references (MC reports only RIP-relative ones on x86,
// matching the Rust side's `is_ip_rel_memory_operand` gate), skipping targets
// that map to no section. Plain data references split into Read/Write via
// `Disasm::isStore`, the counterpart of the Rust side's
// `InstructionInfoFactory` access query.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_XREFS_H
#define REVENANT_ANALYSIS_XREFS_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Program.h"

#include "llvm/ADT/ArrayRef.h"

namespace revenant {

class Disasm;

/// Collect cross-references over every function's decoded instructions,
/// flattened to the sorted `Xrefs` table. `Funcs` must be sorted by start;
/// each function's instructions are walked in order.
Xrefs collectXrefs(const Image &Img, llvm::ArrayRef<Function> Funcs,
                   const Disasm &D);

/// Collect cross-references over one flat instruction list (the per-function
/// worker behind the overload above; also handy for linear sweeps).
Xrefs collectXrefs(const Image &Img, llvm::ArrayRef<Instr> Instrs,
                   const Disasm &D);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_XREFS_H
