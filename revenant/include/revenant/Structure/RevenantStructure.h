//===- RevenantStructure.h - goto-free structuring annotations -*- C++ -*-===//
//
// RevenantStructure: DREAM-style region structuring over LLVM IR.
//
// The pass does not change the CFG. It classifies each function's control
// flow (acyclic if/switch regions, natural loops refined to while /
// do-while / for, short-circuit condition folds) and records the result as
// `!revenant.region` metadata on header terminators, plus stable `bb_*`
// block names, per the frontend IR contract.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_STRUCTURE_REVENANTSTRUCTURE_H
#define REVENANT_STRUCTURE_REVENANTSTRUCTURE_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
class DominatorTree;
class PostDominatorTree;
class LoopInfo;
} // namespace llvm

namespace revenant {

/// Result of structuring one function. Counts are informational; the
/// annotations carry the real payload.
struct StructureResult {
  /// Irreducible edges or regions that could not be represented structurally.
  unsigned Irreducible = 0;
  /// True when recursion, depth, or visit limits force flat labels + goto;
  /// the pass clears all region annotations in that function.
  bool FlatFallback = false;
};

/// Run the structuring analysis on F, using the provided analyses.
/// Names blocks and attaches `!revenant.region` metadata. Never modifies
/// the CFG.
StructureResult annotateFunction(llvm::Function &F, llvm::DominatorTree &DT,
                                 llvm::PostDominatorTree &PDT,
                                 llvm::LoopInfo &LI);

class RevenantStructurePass
    : public llvm::PassInfoMixin<RevenantStructurePass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
};

} // namespace revenant

#endif // REVENANT_STRUCTURE_REVENANTSTRUCTURE_H
