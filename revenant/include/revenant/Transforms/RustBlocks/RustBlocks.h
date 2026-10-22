//===- RustBlocks.h - restore the rustc MIR block shape ------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// rustc lowers every MIR block to one LLVM block, and a MIR block that
// ends in a call ends *at* the call. The compiler's own pre-optimization
// IR therefore keeps one basic block per call site:
//
//   bb3:
//     %0 = call i64 @core..slice..index..SliceIndex..get(...)
//     br label %bb4
//
// The optimizer merges those blocks (SimplifyCFG folds the `br label`
// chain into the block that follows) and the decompiler's cleanup
// pipeline does the same, so the lifted IR ends up with several calls in
// one block. The pre shape keeps them apart, and the block count is what
// the IR fidelity metric compares.
//
// This pass splits a block right after every call that is not the last
// instruction of its block: the call ends the rustc block, everything
// after it belongs to the next one. The rewrite is a pure block split
// (the tail moves into a new block reached by an unconditional branch),
// so the semantics do not change: no value is redefined, no edge is
// added or removed, and the call keeps its position, its operands and
// its metadata.
//
// Gating: the module flag `revenant.lang == "rust"` (the contract's
// producer-language marker, set from the detected compiler profile). A C
// or C++ module never sees this rewrite, so its IR stays bit-identical.
//
// The pass runs before `revenant-structure`: the structuring pass names
// the blocks and records the region annotations with block-list indices,
// so the CFG must be final before it runs.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_RUSTBLOCKS_RUSTBLOCKS_H
#define REVENANT_TRANSFORMS_RUSTBLOCKS_RUSTBLOCKS_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct RustBlocksPass : public llvm::PassInfoMixin<RustBlocksPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
