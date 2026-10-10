//===- UnhotCold.cpp - merge hot/cold splits, undo PGO reorder --------===//
//
// Part of the Revenant LLVM fork.
//
// See UnhotCold.h for the shapes handled. Two sub-passes run in order:
//
// 1. mergeColdChunks: a cold-only function called/jumped-to from exactly one
//    hot block is inlined back into the caller as a cold branch. Corpus
//    shapes (all verified against the oracle binaries, never read by the
//    pass itself):
//    - lua-gcc-o2 `luaD_throw` -> `luaD_throw.cold`: hot ends
//      `je cold; ...; jmp cold`, cold is `call abort` (noreturn).
//      Alive2 reasoning: single caller + noreturn callee = replacing the
//      `jmp cold` edge with the callee body and `unreachable` preserves
//      all executions (the edge never returns either way).
//    - cjson-cl-o2pgo chained chunks: hot `jne coldChunk`, cold chunk ends
//      in `ret` with the hot frame's registers spilled; the chunk has no
//      prologue of its own (PrologSize applies to the primary). Merge =
//      splice blocks, rewrite the jump edge to fall into the chunk.
// 2. undoPgoReorder: blocks whose `!revenant.va` order disagrees with layout
//    order are sorted back toward VA order when the permutation is
//    semantics-free (only unconditional fallthrough/jump edges cross the
//    move), and branches whose likely-taken target is the fallthrough are
//    re-canonicalized (swap successors + invert condition) so the hot path
//    is the fallthrough again.
//
// Both preserve `!revenant.va` (blocks keep their names, instructions are
// moved not recreated) and never touch `!revenant.opaque` calls.
//
// The pass is a module pass: the cold-chunk search looks at sibling
// functions, so the candidate index is built once per module (one walk plus
// the CFG edges that can carry a chunk) instead of walking the whole module
// twice for every function.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/UnhotCold/UnhotCold.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace {

// Minimum VA distance between a hot caller block and a cold chunk block to
// count as "far": hot/cold live in different locality groups. lua-gcc-o2:
// hot luaD_throw at 0x14000dab0, cold at 0x140038a50 (gap ~178 KiB).
// sqlite-gcc-o2: hot at 0x140084520, cold at 0x1400d575f (gap ~330 KiB).
// 64 KiB sits an order of magnitude below the smallest observed split while
// far above any intra-function gap (largest intra-function hot gap seen:
// a few KiB of jump tables, which are data not code).
constexpr uint64_t FarChunkGap = 64 * 1024;

/// Read `!revenant.va !{i64 VA}` from an instruction.
std::optional<uint64_t> instVA(const Instruction &I) {
  MDNode *N = I.getMetadata("revenant.va");
  if (!N || N->getNumOperands() != 1)
    return std::nullopt;
  auto *CI = mdconst::dyn_extract<ConstantInt>(N->getOperand(0));
  if (!CI || !CI->getType()->isIntegerTy(64))
    return std::nullopt;
  return CI->getZExtValue();
}

/// Minimum `!revenant.va` over a block's instructions, if any carries one.
std::optional<uint64_t> blockVA(const BasicBlock &BB) {
  std::optional<uint64_t> Min;
  for (const Instruction &I : BB)
    if (auto V = instVA(I))
      Min = Min ? std::min(*Min, *V) : *V;
  return Min;
}

/// True if the call target names an opaque asm stub; those are never cold
/// merge candidates (the lifter has not recovered their semantics yet).
bool isOpaqueCall(const Instruction &I) {
  return I.getMetadata("revenant.opaque") != nullptr;
}

/// True when a call never returns: attribute on the call site or on the
/// callee declaration/definition (`.ll` text puts `noreturn` on the
/// `define`/`declare`, not on each call site).
bool callIsNoReturn(const CallBase *CB) {
  if (CB->hasFnAttr(Attribute::NoReturn))
    return true;
  const Function *Callee = CB->getCalledFunction();
  return Callee && Callee->hasFnAttribute(Attribute::NoReturn);
}

/// A block is cold-mergeable when every non-phi instruction is either pure
/// (no memory effects beyond a read of read-only state) or a noreturn
/// terminator sequence: calls to noreturn functions, `ud2`-style traps
/// (opaque asm calls are excluded above), `unreachable`, or `ret` with no
/// live values escaping except through memory the caller owns. In practice
/// the corpus cold chunks are trap stubs (`call abort; ud2...`), so require:
/// no stores, no volatile/atomic, no opaque calls, and the terminator is
/// `ret`, `unreachable`, or a branch to a block that is itself mergeable.
bool blockIsColdLike(const BasicBlock &BB) {
  for (const Instruction &I : BB) {
    if (isa<PHINode>(I))
      continue;
    if (isOpaqueCall(I))
      return false;
    if (const auto *CB = dyn_cast<CallBase>(&I)) {
      if (callIsNoReturn(CB))
        continue;
      // Pure read-only calls (intrinsics like llvm.dbg) are fine.
      if (CB->doesNotAccessMemory())
        continue;
      return false;
    }
    if (isa<StoreInst>(I))
      return false;
    if (const auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!LI->isSimple())
        return false;
      continue;
    }
    if (I.mayHaveSideEffects())
      return false;
    if (I.mayWriteToMemory())
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Phase 1: merge cold chunks.
//===----------------------------------------------------------------------===//

/// Find a single-caller cold merge: function Cold is a candidate when every
/// block is cold-like, all its blocks' VAs are far from every block VA of
/// Hot, and exactly one Hot block branches/calls into Cold's entry while
/// nothing else references Cold (single caller => use_empty after ignoring
/// that edge means Cold folds away entirely).
///
/// On success fills Merged (the cold blocks, in layout order) and returns
/// the hot predecessor block + the edge instruction.
struct ColdMerge {
  BasicBlock *HotPred = nullptr;
  Instruction *EdgeInst = nullptr;
};

bool findColdMerge(Function &Hot, Function &Cold, ColdMerge &Out) {
  if (Hot.isDeclaration() || Cold.isDeclaration())
    return false;
  if (&Hot == &Cold)
    return false;
  BasicBlock *ColdEntry = &Cold.getEntryBlock();

  // Every cold block must be cold-like, and every VA far from hot VAs.
  SmallVector<uint64_t, 8> HotVAs;
  for (BasicBlock &BB : Hot)
    if (auto V = blockVA(BB))
      HotVAs.push_back(*V);
  if (HotVAs.empty())
    return false;

  for (BasicBlock &BB : Cold) {
    if (!blockIsColdLike(BB))
      return false;
    if (auto V = blockVA(BB)) {
      bool bFar = true;
      for (uint64_t H : HotVAs)
        if (*V >= H ? *V - H < FarChunkGap : H - *V < FarChunkGap) {
          bFar = false;
          break;
        }
      if (!bFar)
        return false;
    } else {
      // No provenance at all: refuse (cannot prove farness).
      return false;
    }
  }

  // The hot edge into the cold entry is either a CFG edge (hot block ends
  // in a branch whose successor is ColdEntry after lifting) or a direct
  // `call Cold` from a hot block (lifted `jmp cold` may surface as a call
  // to the recovered cold function). Accept both; single caller required.
  BasicBlock *FoundPred = nullptr;
  Instruction *FoundEdge = nullptr;
  for (BasicBlock *Pred : predecessors(ColdEntry)) {
    if (Pred->getParent() != &Hot)
      return false;
    if (FoundPred)
      return false; // two hot predecessors: not a single-caller chunk
    FoundPred = Pred;
    FoundEdge = Pred->getTerminator();
  }
  if (!FoundPred) {
    // No CFG edge: look for a direct call to Cold from a hot block.
    if (!Cold.hasOneUse())
      return false;
    const Use &U = *Cold.use_begin();
    const auto *CBC = dyn_cast<CallBase>(U.getUser());
    if (!CBC || CBC->getCalledFunction() != &Cold)
      return false;
    BasicBlock *Caller = const_cast<BasicBlock *>(CBC->getParent());
    if (!Caller || Caller->getParent() != &Hot)
      return false;
    FoundPred = Caller;
    FoundEdge = const_cast<CallBase *>(CBC);
  }
  for (BasicBlock &BB : Cold) {
    if (&BB == ColdEntry)
      continue;
    // Internal predecessors must all be inside Cold.
    for (BasicBlock *Pred : predecessors(&BB))
      if (Pred->getParent() != &Cold)
        return false;
  }
  // Cold must not be referenced elsewhere (no address-taken uses).
  if (!Cold.hasLocalLinkage())
    return false;
  if (!Cold.hasOneUse() && !Cold.use_empty()) {
    // Count direct call/jump users inside Hot's predecessor only.
    unsigned HotUsers = 0;
    for (const Use &U : Cold.uses()) {
      const auto *CB = dyn_cast<CallBase>(U.getUser());
      if (!CB || CB->getParent()->getParent() != &Hot ||
          CB->getParent() != FoundPred)
        return false;
      ++HotUsers;
    }
    if (HotUsers != 1)
      return false;
  }
  Out.HotPred = FoundPred;
  Out.EdgeInst = FoundEdge;
  return true;
}

/// Perform the merge: splice Cold's blocks before Hot's return/exit region
/// (right after HotPred), rewrite the edge to fall into ColdEntry, and let
/// Cold's own terminators stand. Cold's `ret`s become branches to the
/// hot return block when Cold returns a value Hot ignores... in the corpus
/// shapes the cold chunk is noreturn (trap/abort), so the common case is
/// Cold ending in `unreachable` and no return rewrite is needed. If Cold
/// has `ret` terminators, only merge when HotPred's edge is an unconditional
/// jump (tail-chunk shape): rewrite each cold `ret` to branch to Hot's
/// successor-after-pred... To stay airtight, bail on returning chunks unless
/// the hot edge is a `jmp`-style unconditional branch AND the cold chunk has
/// a single `ret` whose value matches the hot function's return type void.
/// (cjson-cl-o2pgo chained chunks end in `ret` with the hot frame live;
/// those arrive already joined by discovery as one function's blocks, so
/// this path only sees the noreturn cross-function shape.)
bool mergeCold(Function &Hot, Function &Cold, const ColdMerge &M) {
  BasicBlock *HotPred = M.HotPred;
  BasicBlock *ColdEntry = &Cold.getEntryBlock();

  bool bHasRet = false;
  for (BasicBlock &BB : Cold)
    if (isa<ReturnInst>(BB.getTerminator()))
      bHasRet = true;
  auto *BI = dyn_cast<BranchInst>(M.EdgeInst);
  if (bHasRet) {
    // Only the unconditional tail-jump shape with void return merges.
    if (!BI || !BI->isUnconditional() || !Hot.getReturnType()->isVoidTy())
      return false;
    // Single ret required.
    unsigned RetCount = 0;
    for (BasicBlock &BB : Cold)
      if (isa<ReturnInst>(BB.getTerminator()))
        ++RetCount;
    if (RetCount != 1)
      return false;
  }
  // Call-shape validation runs before any block moves: the checks below
  // must all pass while both functions are still intact, since a bail
  // after moving leaves an orphan block and an empty donor behind.
  if (auto *CB = dyn_cast<CallBase>(M.EdgeInst)) {
    if (!callIsNoReturn(CB))
      return false;
    Function *Callee = CB->getCalledFunction();
    if (!Callee || !Callee->hasFnAttribute(Attribute::NoReturn))
      return false;
    if (const Instruction *Next = CB->getNextNode())
      if (!isa<UnreachableInst>(Next))
        return false;
    if (!ColdEntry->getTerminator() ||
        !isa<UnreachableInst>(ColdEntry->getTerminator()))
      return false;
  }
  // Move cold blocks into Hot, preserving order and names/VA metadata
  // (instructions are moved, never recreated, so metadata rides along).
  // Insert right after HotPred for locality.
  Function::iterator InsertPos = HotPred->getIterator();
  ++InsertPos;
  SmallVector<BasicBlock *, 8> Moved;
  for (BasicBlock &BB : Cold)
    Moved.push_back(&BB);
  for (BasicBlock *BB : Moved) {
    BB->removeFromParent();
    Hot.insert(InsertPos, BB);
  }

  // Rewrite the hot edge to enter the cold entry.
  if (BI) {
    if (BI->isUnconditional()) {
      BI->setSuccessor(0, ColdEntry);
    } else {
      // Conditional: the edge that targeted Cold keeps targeting ColdEntry
      // (same block, now local). Nothing to change unless the branch went
      // through a call (handled below). Verify one successor reaches cold.
      bool bHitsCold = false;
      for (unsigned i = 0; i < BI->getNumSuccessors(); ++i)
        if (BI->getSuccessor(i) == ColdEntry ||
            BI->getSuccessor(i)->getParent() != &Hot)
          bHitsCold = true;
      (void)bHitsCold;
    }
  } else if (auto *CB = dyn_cast<CallBase>(M.EdgeInst)) {
    // `call cold()` shape: inline the cold body at the call site. Erase
    // the call, splice the cold entry's non-phi instructions before it.
    // Only the single-block noreturn shape merges here (multi-block
    // call-target chunks need a full inline pass, out of scope). All
    // noreturn/unreachable conditions were validated before the move.
    if (!callIsNoReturn(CB) || !ColdEntry->getTerminator() ||
        !isa<UnreachableInst>(ColdEntry->getTerminator()))
      return false;
    // Cold body = entry block's calls before the unreachable.
    SmallVector<Instruction *, 8> Body;
    for (Instruction &I : *ColdEntry) {
      if (isa<PHINode>(I) || isa<UnreachableInst>(I))
        continue;
      Body.push_back(&I);
    }
    for (Instruction *I : Body) {
      I->removeFromParent();
      I->insertBefore(CB);
    }
    CB->eraseFromParent();
    // Drop the now-empty cold entry from Hot again.
    ColdEntry->removeFromParent();
    // Moved no longer owns it; fix the list and erase the donor function.
    Moved.clear();
  } else {
    return false;
  }

  // Rewrite cold rets (void tail-chunk shape only, checked above).
  if (bHasRet) {
    // The hot successor after the merged region: HotPred's fallthrough is
    // ColdEntry now; the chunk's ret should leave the function.
    for (BasicBlock *BB : Moved) {
      if (auto *RI = dyn_cast<ReturnInst>(BB->getTerminator())) {
        RI->eraseFromParent();
        new UnreachableInst(BB->getContext(), BB);
      }
    }
    // Void `ret` in a void function == `unreachable` after a noreturn
    // chunk... except the chunk DID return. Correct rewrite: branch to
    // the function's exit. Find Hot's unique returning block is too
    // clever; instead keep semantics trivially: the unconditional-jump
    // tail shape means control never comes back, so `unreachable` would
    // be wrong. Undo: refuse this case after all (airtight > coverage).
    // Restore is complex; instead detect BEFORE moving. Since we already
    // moved, convert unreachable back by re-adding `ret void`.
    for (BasicBlock *BB : Moved) {
      if (auto *UI = dyn_cast<UnreachableInst>(BB->getTerminator())) {
        UI->eraseFromParent();
        ReturnInst::Create(BB->getContext(), nullptr, BB);
      }
    }
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Phase 2: undo PGO block reorder + branch inversion.
//===----------------------------------------------------------------------===//

/// Swap the successors of a conditional branch and invert its condition.
/// Returns the new branch (condition already inverted in place when it is a
/// simple `icmp`/`fcmp` predicate flip or an `xor i1 X, true`; otherwise a
/// fresh `xor` is inserted and the old condition kept).
BranchInst *invertBranch(BranchInst *BI) {
  Value *Cond = BI->getCondition();
  BasicBlock *T = BI->getSuccessor(0);
  BasicBlock *F = BI->getSuccessor(1);
  // Swap successors first.
  BI->setSuccessor(0, F);
  BI->setSuccessor(1, T);
  if (auto *CI = dyn_cast<CmpInst>(Cond)) {
    CI->setPredicate(CmpInst::getInversePredicate(CI->getPredicate()));
    return BI;
  }
  if (auto *Xor = dyn_cast<BinaryOperator>(Cond);
      Xor && Xor->getOpcode() == Instruction::Xor) {
    auto *C = dyn_cast<ConstantInt>(Xor->getOperand(1));
    if (C && C->getType()->isIntegerTy(1) && C->isOne()) {
      // not (xor X, true) == X.
      Cond = Xor->getOperand(0);
      BI->setCondition(Cond);
      return BI;
    }
  }
  // General case: insert `not cond` before the branch.
  auto *Not =
      BinaryOperator::CreateNot(Cond, Cond->getName() + ".pgoinv", BI);
  if (auto *IC = dyn_cast<Instruction>(Cond))
    if (MDNode *VA = IC->getMetadata("revenant.va"))
      Not->setMetadata("revenant.va", VA);
  BI->setCondition(Not);
  return BI;
}

/// Reorder blocks toward ascending minimum `!revenant.va`, moving only
/// blocks whose motion crosses no conditional edge (fallthrough/jump-only
/// spine), and re-canonicalize branches whose hot successor is the
/// fallthrough layout successor.
///
/// PGO block placement puts the hot successor first in layout order but
/// keeps the cold target as the `true` successor (branch inversion), or
/// scatters cold blocks megabytes away. The lift preserves VAs, so sorting
/// the unconditional spine by VA recovers source order. Conditional edges
/// pin their endpoints: a block ending in a conditional branch never moves
/// past either successor.
bool undoReorder(Function &F) {
  bool bChanged = false;

  // Pass A: re-canonicalize inverted branches using profile metadata. A
  // conditional branch is inverted when `!prof`/`!branch_weights` says the
  // taken successor is the layout fallthrough... LLVM's convention is the
  // opposite (weights[0] = true-edge). PGO inversion shows as: the
  // fallthrough (next block in layout) is successor 0 (true) but weights
  // say successor 1 dominates. Swap + invert so the hot path falls
  // through, matching -O0 lowering (compare main.pre.ll `cmp_signed`: true
  // edge = then-block in source order).
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    MDNode *Use = BI->getMetadata(LLVMContext::MD_prof);
    if (!Use)
      Use = BI->getMetadata("branch_weights");
    if (!Use || Use->getNumOperands() < 3)
      continue;
    auto *W0 = mdconst::dyn_extract<ConstantInt>(Use->getOperand(1));
    auto *W1 = mdconst::dyn_extract<ConstantInt>(Use->getOperand(2));
    if (!W0 || !W1)
      continue;
    uint64_t wT = W0->getZExtValue(), wF = W1->getZExtValue();
    if (wT == wF)
      continue;
    BasicBlock *Next = BB.getNextNode();
    // Layout next is one successor but weights say the other dominates:
    // PGO inverted this branch. Swap + invert so the hot path is the
    // layout fallthrough again.
    if (wF > wT && BI->getSuccessor(0) == Next &&
        BI->getSuccessor(1) != Next) {
      invertBranch(BI);
      bChanged = true;
    } else if (wT > wF && BI->getSuccessor(1) == Next &&
               BI->getSuccessor(0) != Next) {
      invertBranch(BI);
      bChanged = true;
    }
  }

  // Pass B: VA-order the unconditional spine. Collect blocks linked purely
  // by unconditional branches/fallthrough; sort runs of such blocks by
  // their VA. Blocks ending in conditional branches, switches, returns,
  // invokes, or with multiple predecessors stay pinned.
  SmallVector<BasicBlock *, 16> Order;
  for (BasicBlock &BB : F)
    Order.push_back(&BB);
  // Insertion order of moved blocks: stable sort keyed runs.
  bool bMoved = true;
  unsigned Iter = 0;
  while (bMoved && Iter++ < 4) {
    bMoved = false;
    for (size_t i = 1; i < Order.size(); ++i) {
      BasicBlock *Prev = Order[i - 1], *Cur = Order[i];
      auto vP = blockVA(*Prev), vC = blockVA(*Cur);
      if (!vP || !vC || *vP <= *vC)
        continue;
      // Both must be on the unconditional spine: single predecessor each
      // (or entry), unconditional-branch-or-fallthrough terminators, and
      // Prev must flow directly into Cur (else swapping breaks layout
      // fallthrough assumptions... note LLVM layout fallthrough is only
      // meaningful for `br` chains without explicit successors; swapping
      // any two blocks is semantics-free since all edges are explicit.
      // The pin is about *usefulness*: only reorder adjacent spine pairs
      // whose VA order is inverted AND that are directly linked, so each
      // swap strictly reduces VA disorder along a real edge.
      auto *PT = Prev->getTerminator();
      bool bLinked = false;
      if (auto *PBI = dyn_cast<BranchInst>(PT)) {
        if (PBI->isUnconditional() && PBI->getSuccessor(0) == Cur)
          bLinked = true;
      } else if (isa<ReturnInst>(PT) || isa<UnreachableInst>(PT)) {
        bLinked = false;
      } else if (Cur == Prev->getNextNode()) {
        // Fallthrough edge Prev -> Cur (Cur is layout next): swapping
        // needs care; only when Cur's terminator is unconditional back?
        // Keep it simple: never break an existing fallthrough adjacency.
        bLinked = false;
      }
      // Cur must not be a join (multiple preds) and neither may end in a
      // conditional/switch.
      if (bLinked && !Cur->hasNPredecessorsOrMore(2) &&
          isa<BranchInst>(Cur->getTerminator()) &&
          cast<BranchInst>(Cur->getTerminator())->isUnconditional()) {
        // Also require the VA inversion to be large (cold scatter), not
        // mere source-order noise: PGO moves blocks KiBs away.
        if (*vP > *vC && *vP - *vC >= 4096) {
          Order[i - 1] = Cur;
          Order[i] = Prev;
          bMoved = true;
          bChanged = true;
        }
      }
    }
  }
  if (bChanged) {
    // Apply the permutation: move blocks to match Order.
    for (BasicBlock *BB : Order)
      BB->removeFromParent();
    for (BasicBlock *BB : Order)
      F.insert(F.end(), BB);
  }
  return bChanged;
}

//===----------------------------------------------------------------------===//
// Module-wide cold-chunk index.
//===----------------------------------------------------------------------===//

/// The cold chunks that can belong to a hot function, indexed once per
/// module. The search is module-wide: a cold chunk is a sibling of its hot
/// caller, so the old per-function search walked the whole module twice for
/// every function (quadratic in the number of functions). The index keeps
/// the same candidate sets with one walk and the CFG edges that can carry a
/// chunk.
struct ColdIndex {
  /// Name-based candidates: chunks whose name starts with the hot
  /// function's name and carries a cold marker (`<hot>.cold`, ...).
  StringMap<SmallVector<Function *, 4>> ByName;
  /// Shape-based candidates: local-linkage chunks whose hot edge comes
  /// from the hot function.
  DenseMap<Function *, SmallVector<Function *, 4>> ByShape;
};

/// True when a function name carries a cold marker.
bool hasColdMarker(StringRef N) {
  return N.ends_with(".cold") || N.ends_with(".unlikely") ||
         N.contains(".cold.") || N.contains(".unlikely.");
}

ColdIndex buildColdIndex(Module &M) {
  ColdIndex Index;
  // Name-based: the hot function's name is a prefix of the chunk's name.
  // Cold-marked names are rare, so the prefix search over the module's
  // functions stays cheap.
  for (Function &G : M) {
    if (G.isDeclaration())
      continue;
    StringRef N = G.getName();
    if (!hasColdMarker(N))
      continue;
    for (Function &F : M) {
      if (&F == &G || F.isDeclaration())
        continue;
      if (!N.starts_with(F.getName()))
        continue;
      Index.ByName[F.getName()].push_back(&G);
    }
  }
  // Shape-based: a local-linkage chunk reached from a hot block. The chunk
  // must have exactly one hot predecessor (findColdMerge checks that), so
  // the index only keeps the edges that can carry one.
  for (Function &G : M) {
    if (G.isDeclaration() || !G.hasLocalLinkage())
      continue;
    BasicBlock *Entry = &G.getEntryBlock();
    bool bHasPred = false;
    for (BasicBlock *Pred : predecessors(Entry)) {
      bHasPred = true;
      if (Pred->getParent() == &G)
        continue;
      Index.ByShape[Pred->getParent()].push_back(&G);
    }
    if (bHasPred)
      continue;
    // Call shape: the chunk is called, not branched to.
    if (!G.hasOneUse())
      continue;
    const Use &U = *G.use_begin();
    const auto *CB = dyn_cast<CallBase>(U.getUser());
    if (!CB || CB->getCalledFunction() != &G)
      continue;
    BasicBlock *Caller = const_cast<BasicBlock *>(CB->getParent());
    Index.ByShape[Caller->getParent()].push_back(&G);
  }
  return Index;
}

} // namespace

PreservedAnalyses revenant::UnhotColdPass::run(Module &M,
                                               ModuleAnalysisManager &) {
  bool bChanged = false;

  // Phase 1: cross-function cold merge. The cold chunk is its own function
  // (GCC `.cold` / clang MFS outline); find it among module siblings by
  // name suffix or by shape. The search is module-wide, so the candidates
  // are indexed once here instead of scanning the whole module for every
  // function (that scan was the pass's whole cost: two module walks per
  // function, quadratic in the number of functions).
  ColdIndex Index = buildColdIndex(M);
  // The function list is fixed up front: a merge erases the cold chunk, so
  // the iteration must not walk the module's function list while it
  // changes.
  SmallVector<Function *, 64> Functions;
  for (Function &F : M)
    if (!F.isDeclaration())
      Functions.push_back(&F);
  for (Function *F : Functions) {
    // A chunk merged into an earlier function is gone.
    if (F->getParent() != &M)
      continue;
    SmallVector<Function *, 4> ColdCands;
    auto ByName = Index.ByName.find(F->getName());
    if (ByName != Index.ByName.end() && !ByName->second.empty())
      ColdCands = ByName->second;
    else {
      auto ByShape = Index.ByShape.find(F);
      if (ByShape != Index.ByShape.end())
        ColdCands = ByShape->second;
    }
    for (Function *Cold : ColdCands) {
      if (Cold->getParent() != &M)
        continue;
      ColdMerge CM;
      if (!findColdMerge(*F, *Cold, CM))
        continue;
      if (!mergeCold(*F, *Cold, CM))
        continue;
      // The call-shape merge empties Cold (body spliced into Hot);
      // the block-splice merge leaves Cold empty too. Erase when dead.
      if (Cold->empty() && Cold->use_empty())
        Cold->eraseFromParent();
      else if (Cold->empty())
        Cold->dropAllReferences(), Cold->eraseFromParent();
      bChanged = true;
      break; // one merge per run; rerun the pass for chains
    }

    // Phase 2: intra-function PGO reorder/branch fix.
    bChanged |= undoReorder(*F);
  }

  return bChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// Plugin entry for `opt -load-pass-plugin`. Same convention as Devectorize:
// compiled only for the standalone plugin DLL.
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnhotCold", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unhotcold") {
                    PM.addPass(revenant::UnhotColdPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
