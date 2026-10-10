#ifndef REVENANT_TRANSFORMS_REROLL_H
#define REVENANT_TRANSFORMS_REROLL_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace revenant {

struct RerollPass : llvm::PassInfoMixin<RerollPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

// Multi-accumulator unroll fold: merges 2+ accumulator PHIs in a
// self-latch loop into one (gcc -O3 interleaved unrolls). Returns the
// number of loops folded. Called after single-accumulator fold rounds.
unsigned unrollAccumFold(llvm::Function &F,
                        llvm::SmallVectorImpl<llvm::StringRef> &Notes);

} // namespace revenant

#endif
