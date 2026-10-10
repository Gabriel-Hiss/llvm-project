//===- PairSplit.h - scalar i128 copy-web splitting ---------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Scalar-i128 half of `devectorize`: MSVC lowers 16-byte struct copies and
// spills through xmm as i128 loads/stores/phis. When an i128 web is only a
// copy (every def is a constant, a simple load, or a phi of those, and every
// use is a store or a web phi), the web splits into two i64 halves: same
// bytes, scalar statements, and no i128 constants for the C emitter to
// materialize. Anything else (i128 arithmetic, live-out uses, shared webs,
// volatile/atomic) leaves the web untouched; the CFG never changes.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_DEVECTORIZE_PAIRSPLIT_H
#define REVENANT_TRANSFORMS_DEVECTORIZE_PAIRSPLIT_H

namespace llvm {
class Function;
}

namespace revenant {
namespace pairsplit {

unsigned splitPairs(llvm::Function &F);

} // namespace pairsplit
} // namespace revenant

#endif
