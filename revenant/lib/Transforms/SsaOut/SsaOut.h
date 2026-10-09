#ifndef REVENANT_TRANSFORMS_SSAOUT_H
#define REVENANT_TRANSFORMS_SSAOUT_H

#include "llvm/IR/PassManager.h"

namespace revenant {

// SSA-out: undo mem2reg/sroa into named locals.
//
// The cleanup pipeline (and the optimizer generally) leaves lifted
// functions in SSA form: `phi` nodes, plus single-use pure values that a
// programmer would read as inline expressions. Frontends (Clang, rustc)
// want the opposite shape: a small set of named locals (`alloca`s with DI
// names), single-use expressions left for the frontend to inline, and one
// store per assignment. This pass is that lowering step; it runs after
// structuring, right before the frontends (explicit
// `-passes=revenant-ssaout`, not part of the default pipeline).
//
// What it does, mirroring `crates/structure` (`coalesce_phis`,
// expression propagation, `block_stmts` materialization):
//
//  1. Phi-web coalescing: phi destinations that feed each other through
//     phi incoming edges share one local (the classic `sum` loop web).
//     The canonical member carries the name; other members become
//     bitcast-free aliases resolved at copy insertion.
//  2. Copy insertion on predecessor edges: every phi becomes a set of
//     `store incoming, local` placed at the end of each predecessor.
//     Critical edges are split first (LLVM `SplitCriticalEdge`) so a copy
//     runs only on its own edge. Parallel copies on one edge are ordered
//     so no copy clobbers a value another pending copy still reads;
//     cycles (`x = y; y = x`) spill through a temporary alloca.
//  3. Expression propagation: a pure single-use definition (binop, cmp,
//     cast, gep, select, single-use call) stays where it is and is left
//     for the frontend to inline — the pass only guarantees its operands
//     resolve to locals/constants. Multi-use or side-effecting
//     definitions (loads, calls with >1 use, atomics, allocas) are
//     demoted to a named local: one alloca, one store per definition,
//     loads at each use.
//
// Naming: locals are named after a surviving member of their phi web or
// def (`v_<name>`, sanitized); synthesized temporaries are `v_tmp<N>`.
// Every local gets an alloca in the entry block plus a `DILocalVariable`
// with `#dbg_declare` (debug records, LLVM 22 spelling tolerated by the
// LLVM 23 reader).
//
// Guarantees: the CFG is unchanged apart from critical-edge splits;
// provenance (`!revenant.va`) is copied onto every new instruction;
// `!revenant.opaque` markers are never moved or dropped; structuring
// annotations (`!revenant.region`) on terminators survive (split edges
// copy them to the new terminator).
struct SsaOutPass : public llvm::PassInfoMixin<SsaOutPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
