#ifndef REVENANT_TRANSFORMS_DEVECTORIZE_H
#define REVENANT_TRANSFORMS_DEVECTORIZE_H

#include "llvm/IR/PassManager.h"

namespace revenant {

// Devectorization + lane-0 scalarization.
//
// Undoes SLP and loop-vectorizer output: fixed-width vector code produced
// from scalar sources is rewritten back into scalar operations. Runs in
// three phases, mirroring `crates/unopt` (`lane0`, then loop
// `devectorize`, then straight-line SLP explosion):
//
//  1. Lane-0: a vector web where every use reads only lane 0
//     (`extractelement .., 0`) or forwards the whole vector transparently
//     (`phi`, vector `bitcast`, `insertelement .., 0` base) collapses to a
//     scalar web of the element type.
//  2. Loops: a loop whose vector PHIs are all reduction accumulators
//     (backedge is a lane-wise add/mul/and/or/xor over the PHI) or lane-0
//     accumulators (backedge is `insertelement base, s, 0`) is exploded
//     into per-lane scalar operations feeding one scalar accumulator
//     chain. `llvm.vector.reduce.*` sums in exit blocks alias to the chain
//     end. Anything unrecognized (non-splat init, cross-type bitcasts,
//     masked memory ops) leaves the loop untouched.
//  3. Straight-line: remaining vector producers in blocks outside any loop
//     are exploded in place so a later reroll sees scalar code.
//
// The pass never changes the CFG, so structuring annotations on
// terminators (`!revenant.region`) survive untouched. Provenance
// (`!revenant.va`) and debug locations are copied from each replaced
// instruction onto its scalar replacements.
struct DevectorizePass : public llvm::PassInfoMixin<DevectorizePass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static llvm::StringRef name() { return "revenant-devectorize"; }
};

} // namespace revenant

#endif
