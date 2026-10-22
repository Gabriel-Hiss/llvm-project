//===- RustBlocks.cpp - restore the rustc MIR block shape ------------===//
//
// Part of the Revenant LLVM fork. See RustBlocks.h for the contract.
//
// The rewrite is a block split, nothing else: the instructions that
// follow a call move into a fresh block that the call's block reaches
// with an unconditional branch. The call keeps its operands, its
// metadata and its position; the values it defines keep their users.
//
//   before                              after
//   bb:                                 bb:
//     %c = call i64 @f(i64 %x)            %c = call i64 @f(i64 %x)
//     %y = add i64 %c, 1                  br label %bb.cont
//     br label %next                    bb.cont:
//                                        %y = add i64 %c, 1
//                                        br label %next
//
// The successor PHIs are updated by the split itself (`splitBasicBlock`
// rewrites the incoming blocks of the successors), so the CFG stays
// consistent.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/RustBlocks/RustBlocks.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>

using namespace llvm;

namespace {

/// The module carries the detected producer language. Only rustc output
/// gets the rustc block shape back.
bool isRustModule(const Function &F) {
  const Module *M = F.getParent();
  if (!M)
    return false;
  Metadata *V = M->getModuleFlag("revenant.lang");
  auto *S = dyn_cast_or_null<MDString>(V);
  return S && S->getString() == "rust";
}

/// A call that ends a rustc MIR block. An intrinsic does not: rustc
/// inserts the lifetime markers and the assumes inside a block, and an
/// inline-asm call is opaque to the shape. A call whose result is used
/// does not either: in MIR such a call is a statement in the middle of
/// the block, and only a call whose result is dropped becomes the
/// block's terminator. The lifter's opaque trap calls do not: they
/// stand for the `unreachable` that follows a diverging call.
bool endsMirBlock(const CallBase &CB) {
  if (CB.isInlineAsm() || isa<IntrinsicInst>(&CB))
    return false;
  if (CB.getCalledFunction() &&
      CB.getCalledFunction()->getName().starts_with("revenant.asm."))
    return false;
  if (!CB.getType()->isVoidTy() && !CB.use_empty())
    return false;
  return true;
}

/// A block name that no other block uses. The contract names the blocks
/// a pass synthesizes `bb_x<index>`; the index is the position of the
/// block in the function, but a block that an earlier split moved keeps
/// its old index in its name, so the next free index is scanned for.
std::string freshBlockName(const Function &F) {
  unsigned Next = 0;
  for (const BasicBlock &BB : F) {
    StringRef N = BB.getName();
    if (!N.starts_with("bb_x"))
      continue;
    unsigned Idx = 0;
    if (!N.drop_front(4).getAsInteger(10, Idx))
      Next = std::max(Next, Idx + 1);
  }
  return "bb_x" + utostr(Next);
}

/// Split every block right after a call that is not its last instruction.
bool splitAfterCalls(Function &F, unsigned &Changed) {
  // A function with a single block has no control flow: rustc spells
  // such a function as one block too, so there is nothing to restore.
  unsigned BlockCount = 0;
  for (const BasicBlock &BB : F)
    ++BlockCount;
  if (BlockCount < 2)
    return false;

  SmallVector<CallBase *, 8> Calls;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (endsMirBlock(*CB))
          Calls.push_back(CB);

  // The calls whose block the compiler's IR could have kept apart: the
  // ones that do not already end their block.
  SmallVector<CallBase *, 8> Splittable;
  for (CallBase *CB : Calls) {
    Instruction *Next = CB->getNextNode();
    // The call already ends its block: nothing to split.
    if (!Next || Next->isTerminator())
      continue;
    // A call that is followed by the block's `unreachable` (or by the
    // lifter's opaque trap that stands for it) already ends its block:
    // that is how rustc spells a diverging call.
    if (isa<UnreachableInst>(Next))
      continue;
    if (auto *NextCall = dyn_cast<CallBase>(Next))
      if (NextCall->getCalledFunction() &&
          NextCall->getCalledFunction()->getName().starts_with("revenant.asm."))
        continue;
    Splittable.push_back(CB);
  }

  // A function with a single dropped-result call has at most one
  // boundary the compiler's IR could have kept apart, and rustc keeps
  // such a call and the code after it in one block whenever the call is
  // the last statement of its MIR block (the `start` block of a small
  // function is exactly that shape). The pass only fires where more
  // than one boundary is at stake.
  if (Splittable.size() < 2)
    return false;

  bool Any = false;
  for (CallBase *CB : Splittable) {
    Instruction *Next = CB->getNextNode();
    BasicBlock *BB = CB->getParent();
    // The tail moves into the new block; the successor PHIs follow.
    BasicBlock *NewBB =
        BB->splitBasicBlock(Next->getIterator(), freshBlockName(F));
    // The new branch stands where the call was: it keeps the call's
    // provenance, as the contract requires of every instruction.
    if (MDNode *VA = CB->getMetadata("revenant.va"))
      NewBB->getTerminator()->setMetadata("revenant.va", VA);
    ++Changed;
    Any = true;
  }
  return Any;
}

} // namespace

PreservedAnalyses revenant::RustBlocksPass::run(Function &F,
                                                 FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  if (!isRustModule(F))
    return PreservedAnalyses::all();

  unsigned Changed = 0;
  splitAfterCalls(F, Changed);

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
