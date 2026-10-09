//===- StraightDevec.h - straight-line SLP explosion ---------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Straight-line half of `devectorize` (the loop half is LoopDevec): blocks
// without vector PHIs have their vector producers exploded in place, so a
// later reroll sees scalar code. Each vector instruction is replaced at
// its own position (memory ops and calls keep their order); scalar uses
// resolve through an alias map. Mirrors
// `crates/unopt/src/devectorize.rs` (`Straight`).
//
// Conservative: a load consumed across an intervening call/store refuses
// the whole block (reloading would reread clobbered memory); any
// unrecognized vector producer refuses the block; the CFG never changes.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_DEVECTORIZE_STRAIGHTDEVEC_H
#define REVENANT_TRANSFORMS_DEVECTORIZE_STRAIGHTDEVEC_H

namespace llvm {
class Function;
}

namespace revenant {
namespace straight {

unsigned explodeStraight(llvm::Function &F);

} // namespace straight
} // namespace revenant

#endif
