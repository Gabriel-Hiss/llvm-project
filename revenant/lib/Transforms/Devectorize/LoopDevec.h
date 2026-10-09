//===- LoopDevec.h - single-block vector reduction loops -------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Loop half of `devectorize` (the other half is straight-line SLP
// explosion, still to land): a loop whose vector PHIs are all integer
// reduction accumulators is exploded into per-element scalar operations
// feeding one scalar accumulator chain per PHI. `llvm.vector.reduce.*`
// sums in exit blocks alias to the chain end. Mirrors
// `crates/unopt/src/devectorize.rs` (`LoopCx`).
//
// Scope (v1): self-loop single-block headers, integer reduction PHIs that
// share one opcode/element type, splat-constant inits, 128-bit vectors,
// lane-wise body ops, exit values consumed only by matching reduces (see
// Loop.cpp). Anything else leaves the loop untouched; the CFG never
// changes.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_DEVECTORIZE_LOOPDEVEC_H
#define REVENANT_TRANSFORMS_DEVECTORIZE_LOOPDEVEC_H

namespace llvm {
class Function;
}

namespace revenant {
namespace loopdevec {

// Rewrite matching loops in F. Returns the rewrite count.
unsigned devectorizeLoops(llvm::Function &F);

} // namespace loopdevec
} // namespace revenant

#endif
