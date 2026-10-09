//===- UntailDup.h - undo tail duplication and block cloning -----*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// `revenant-untaildup` inverts one family of optimizer transforms: code the
// middle end duplicated and the control flow it threaded around the copies.
//
// - tail duplication (CodeGenPrepare / early-taildup shape on IR): a shared
//   tail block T with predecessors P0..Pn is copied once per predecessor so
//   each copy has a single predecessor and the join disappears. The undo
//   deletes the copies and re-creates the join.
// - jump threading: a conditional branch whose condition is already decided
//   by a predecessor's branch is folded into direct edges (diamond becomes
//   a threaded edge, cascade becomes a direct jump). The undo re-inserts the
//   join: predecessors of identical successor blocks are re-routed through
//   one shared block again.
// - block cloning in general (simplifycfg tail merging in reverse): two or
//   more successor blocks holding the same instruction sequence are merged
//   into one join block with a phi per divergent value.
// - loop unswitching (single-block copies only): a loop-invariant branch
//   guarding two identical single-block loop copies folds back into one
//   loop with the guard deleted. Multi-block unswitched loops are left for
//   a future change.
// - loop peeling (pure bodies only): a straight-line first-iteration copy
//   before a loop header folds back when the peeled block holds no stores
//   or calls; its phi feed sinks into the new entry edge.
//
// Every rewrite is a control-flow join that removes duplicated instructions;
// no instruction is ever speculated or reordered across a side effect, so
// each rewrite is semantics-preserving by construction (see the Alive2-style
// argument on mergeIdenticalBlocks below). Anything ambiguous stays put.
// Provenance: every instruction the pass creates copies `!revenant.va` from
// the instruction it replaces; `!revenant.opaque` calls are never merged,
// created, or dropped; `!revenant.region` on touched terminators is kept.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNTAILUDP_H
#define REVENANT_TRANSFORMS_UNTAILUDP_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UntailDupPass : public llvm::PassInfoMixin<UntailDupPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
