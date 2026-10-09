//===- Unhoist.cpp - undo LICM/GVN-hoisting/PRE -----------------------===//
//
// Part of the Revenant LLVM fork.
//
// Reverses three optimizer shapings that move computations away from the
// blocks a straightforward -O0-style lowering would put them in:
//
//   Phase 1 (sink LICM-hoisted code). Clang's LICM lifts loop-invariant
//   pure expressions and loop-invariant loads into the loop preheader. A
//   lifted loop body then reads values computed outside the loop, while
//   the -O0 lowering (corpus ir-pre) recomputes or reloads them inside
//   the body on every iteration. This phase clones the hoisted
//   instruction back into the loop body before each use, when that is
//   semantics-preserving, and drops the preheader copy once no loop-body
//   use remains. Loads sink only when no store inside the loop may alias
//   them; the check is a local pointer-equality walk over the loop body
//   (stripped pointers, same address reused), which covers the lifted
//   -O0 idiom where the same alloca or argument pointer is reloaded.
//   Phase 2 (undo GVN load forwarding). GVN replaces a reload after a
//   store with the stored value (or with an earlier load of the same
//   address), merging two source reads into one SSA value. This phase
//   restores the second read: a use fed by a value stored earlier in
//   the same block becomes a fresh load of the same pointer, when the
//   load is provably safe there (the pointer already loads or stores
//   in the block, so no new fault is introduced).
//
//   Phase 3 (undo cross-branch CSE). GVN/PRE merge one identical pure
//   computation shared by sibling branches into the common predecessor,
//   threading the result down both sides. This phase pushes the
//   computation back into each successor that uses it, cloning per
//   successor, when every use block shares the same predecessor and
//   the operands are available on each edge.
//
// Alive2-style reasoning for the non-obvious cases:
//
//   Sinking a pure hoisted instruction I from preheader P into the loop
//   body is a refinement because I has no side effects: recomputing it
//   per iteration yields the same value (all operands are loop
//   invariant, so each recomputation sees identical inputs) and
//   introduces no new observable behavior. The transform is skipped
//   when I may read memory (loads need the alias check) or may raise
//   (div/rem need a dominance check, see below).
//
//   Sinking a hoisted div/rem needs more care: hoisting a faulting
//   division out of a loop that runs zero times turns a trap into a
//   value, so sinking it back is safe only when the loop body provably
//   executes whenever the preheader does. Skipped unless the preheader
//   ends in an unconditional branch to the header and the header has
//   exactly that one predecessor outside the loop.
//
//   Duplicating a load (phase 2) or a pure computation (phase 3) adds
//   reads or recomputations the optimizer removed. Loads duplicate only
//   at points where the same address already accesses in that block
//   (load safe: the address is dereferenceable there on every path
//   through the block, since execution reaches the new load only via
//   the existing access), and only when no intervening store may alias
//   (same value would be read back). Pure computations duplicate only
//   when their operands dominate each target block (same values the
//   single hoisted computation saw). Constants carry no use list
//   (ConstantData has none by design), so phase 2 finds the orphaned
//   uses with a forward operand scan instead of use iteration.
//
// Deliberate boundary: hoisted values whose only loop-body uses feed
// header phis (LICM's scalar promotion: preheader load plus entry-edge
// phi) are left in place. A body clone cannot feed a phi's entry edge
// (it would not dominate the incoming edge; the verifier rejects it),
// and a latch-placed reload would read stale memory instead of the
// body's carried value. Restoring per-iteration memory reads there
// means eliminating the phi, which is mem2reg reversal: SsaOut's
// territory (it demotes phi webs to locals with per-assignment
// stores), not this pass's.
//
// Every cloned instruction inherits the source instruction's
// !revenant.va provenance and debug location. !revenant.opaque calls
// are never moved, cloned, or deleted. The CFG is unchanged: clones
// land before existing instructions in existing blocks, so
// !revenant.region annotations survive.
//
// Self-contained by design: the pass queries no LLVM analyses and takes
// no analysis parameters, so it loads under stock `opt
// -load-pass-plugin` (a plugin DLL cannot query opt's analysis
// registry) and links statically into revenant-opt unchanged. Loops
// come from a local natural-loop scan, dominance from a local
// Cooper-Harvey-Kennedy dataflow, alias safety from pointer-equality
// walks. All three match the conservative shapes this pass rewrites
// and deliberately bail out anywhere fancier reasoning would be
// needed.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unhoist/Unhoist.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"

using namespace llvm;

namespace {

MDNode *getVA(const Instruction *I) {
  return I->getMetadata("revenant.va");
}

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *VA = getVA(Src))
    Dst->setMetadata("revenant.va", VA);
  Dst->setDebugLoc(Src->getDebugLoc());
}

bool hasOpaque(const Instruction &I) {
  return I.getMetadata("revenant.opaque") != nullptr;
}

// Local natural-loop description: header plus preheader (when the header
// has exactly one predecessor outside the loop) and the block set.
// Computed without LoopInfo so the pass loads under stock `opt
// -load-pass-plugin` (plugin DLLs cannot query opt's analysis registry).
struct NatLoop {
  BasicBlock *Header = nullptr;
  BasicBlock *Preheader = nullptr;
  SmallPtrSet<BasicBlock *, 16> Blocks;
};

// Immediate-dominator map over the function via Cooper-Harvey-Kennedy
// dataflow on the reverse postorder. Small enough for lifted functions
// and runs only when a candidate rewrite needs it.
struct DomTree {
  DenseMap<BasicBlock *, BasicBlock *> IDom;
  BasicBlock *Entry = nullptr;

  void build(Function &F) {
    Entry = &F.getEntryBlock();
    SmallVector<BasicBlock *, 32> RPO;
    SmallPtrSet<BasicBlock *, 32> Seen;
    SmallVector<BasicBlock *, 16> Stack{Entry};
    Seen.insert(Entry);
    while (!Stack.empty()) {
      BasicBlock *BB = Stack.pop_back_val();
      RPO.push_back(BB);
      for (BasicBlock *S : successors(BB)) {
        if (Seen.insert(S).second)
          Stack.push_back(S);
      }
    }
    DenseMap<BasicBlock *, unsigned> Order;
    for (unsigned K = 0; K < RPO.size(); ++K)
      Order[RPO[K]] = K;
    IDom[Entry] = Entry;
    bool ChangedT = true;
    while (ChangedT) {
      ChangedT = false;
      for (BasicBlock *BB : RPO) {
        if (BB == Entry)
          continue;
        BasicBlock *NewIDom = nullptr;
        for (BasicBlock *P : predecessors(BB)) {
          if (!IDom.count(P))
            continue;
          if (!NewIDom) {
            NewIDom = P;
            continue;
          }
          BasicBlock *A = NewIDom, *B = P;
          while (A != B) {
            while (Order[A] > Order[B])
              A = IDom[A];
            while (Order[B] > Order[A])
              B = IDom[B];
          }
          NewIDom = A;
        }
        if (NewIDom && IDom[BB] != NewIDom) {
          IDom[BB] = NewIDom;
          ChangedT = true;
        }
      }
    }
  }

  bool dominates(const BasicBlock *A, const BasicBlock *B) const {
    if (A == B)
      return true;
    auto It = IDom.find(const_cast<BasicBlock *>(B));
    if (It == IDom.end())
      return false;
    const BasicBlock *Cur = It->second;
    while (Cur && Cur != A) {
      auto It2 = IDom.find(const_cast<BasicBlock *>(Cur));
      if (It2 == IDom.end())
        return false;
      if (It2->second == Cur)
        break;
      Cur = It2->second;
    }
    return Cur == A;
  }

  bool dominates(const Instruction *A, const BasicBlock *B) const {
    const BasicBlock *AB = A->getParent();
    return dominates(AB, B);
  }
};

// Find natural loops via back edges (a successor that dominates its
// predecessor). Each back edge yields one loop body walked backwards
// from the latch to the header.
void findLoops(Function &F, const DomTree &DT, SmallVectorImpl<NatLoop> &Out) {
  for (BasicBlock &BB : F) {
    for (BasicBlock *S : successors(&BB)) {
      if (!DT.dominates(S, &BB))
        continue;
      NatLoop L;
      L.Header = S;
      SmallVector<BasicBlock *, 16> Stack{&BB};
      L.Blocks.insert(S);
      L.Blocks.insert(&BB);
      while (!Stack.empty()) {
        BasicBlock *Cur = Stack.pop_back_val();
        if (Cur == S)
          continue;
        for (BasicBlock *P : predecessors(Cur)) {
          if (L.Blocks.insert(P).second)
            Stack.push_back(P);
        }
      }
      BasicBlock *Outside = nullptr;
      bool Single = true;
      for (BasicBlock *P : predecessors(S)) {
        if (L.Blocks.count(P))
          continue;
        if (!Outside)
          Outside = P;
        else {
          Single = false;
          break;
        }
      }
      L.Preheader = Single ? Outside : nullptr;
      Out.push_back(std::move(L));
    }
  }
}

// Stripped pointer equality: same address when pointer casts are
// ignored. Covers the lifted -O0 idiom where one alloca or argument
// pointer is reloaded; anything fancier bails out, never an unsound
// alias answer.
bool sameAddr(const Value *A, const Value *B) {
  return A->stripPointerCasts() == B->stripPointerCasts();
}

// Provably-disjoint pointers via underlying objects: an alloca creates
// a fresh object that cannot alias any pre-existing object (argument,
// global, or a different alloca), and two different allocas never
// alias. Uses LLVM's own getUnderlyingObject (strips GEPs and casts)
// so GEPs into the same array still compare equal. Anything else
// bails out (false), never an unsound NoAlias.
bool disjointPtr(const Value *A, const Value *B) {
  const Value *U1 = getUnderlyingObject(A);
  const Value *U2 = getUnderlyingObject(B);
  if (U1 == U2)
    return false;
  return isa<AllocaInst>(U1) || isa<AllocaInst>(U2);
}

// True when no may-write instruction in [First, Last) touches Ptr. A
// same-address store means the value changes (false). A store to a
// provably disjoint pointer (underlying-object rule above) is safe;
// any call or unknown writer bails out (false = unsafe).
bool rangeClean(BasicBlock *BB, BasicBlock::iterator First,
                BasicBlock::iterator Last, const Value *Ptr) {
  for (auto It = First; It != Last; ++It) {
    Instruction &J = *It;
    if (!J.mayWriteToMemory())
      continue;
    if (auto *SI = dyn_cast<StoreInst>(&J)) {
      if (sameAddr(SI->getPointerOperand(), Ptr))
        return false;
      if (disjointPtr(SI->getPointerOperand(), Ptr))
        continue;
      return false;
    }
    return false;
  }
  return true;
}


bool isHoistablePure(const Instruction &I) {
  if (hasOpaque(I))
    return false;
  if (I.isTerminator() || isa<PHINode>(I) || I.mayHaveSideEffects())
    return false;
  if (isa<LoadInst>(I))
    return true;
  // Pure value computation: safe to recompute anywhere its operands are
  // available. Div/rem may trap and need the guarded-entry check at the
  // sink site, not here.
  return isa<BinaryOperator>(I) || isa<CmpInst>(I) || isa<CastInst>(I) ||
         isa<SelectInst>(I) || isa<GetElementPtrInst>(I);
}

// True when I and J compute the same value: same opcode/type and same
// operands. Flags are ignored (fast-math/nsw/nuw/exact differences do
// not change the -O0 shape we restore).
bool sameComputation(const Instruction *I, const Instruction *J) {
  if (I->getOpcode() != J->getOpcode() || I->getType() != J->getType())
    return false;
  if (I->getNumOperands() != J->getNumOperands())
    return false;
  if (const auto *LI = dyn_cast<LoadInst>(I)) {
    const auto *LJ = cast<LoadInst>(J);
    return LI->getPointerOperand() == LJ->getPointerOperand();
  }
  for (unsigned K = 0, E = I->getNumOperands(); K < E; ++K)
    if (I->getOperand(K) != J->getOperand(K))
      return false;
  return true;
}

// Every operand of I is defined outside the loop (or is a
// constant/argument): recomputing I inside the loop sees identical
// inputs each iteration.
bool operandsLoopInvariant(const Instruction &I, const NatLoop &L) {
  for (const Value *Op : I.operands()) {
    if (isa<Constant>(Op) || isa<Argument>(Op))
      continue;
    const auto *OI = dyn_cast<Instruction>(Op);
    if (!OI)
      return false;
    if (L.Blocks.count(const_cast<BasicBlock *>(OI->getParent())))
      return false;
  }
  return true;
}

// The loop body runs whenever the preheader does: preheader falls
// through to the header, and the header has no other outside
// predecessor (single-outside-predecessor scan already guarantees the
// latter; recheck cheaply here). Guards sinking possibly-trapping
// instructions.
bool loopAlwaysRuns(const NatLoop &L, const BasicBlock *Preheader) {
  if (!Preheader || !Preheader->getSingleSuccessor())
    return false;
  if (Preheader->getSingleSuccessor() != L.Header)
    return false;
  for (const BasicBlock *Pred : predecessors(L.Header)) {
    if (Pred == Preheader)
      continue;
    if (!L.Blocks.count(const_cast<BasicBlock *>(Pred)))
      return false;
  }
  return true;
}

bool mayTrap(const Instruction &I) {
  if (const auto *BO = dyn_cast<BinaryOperator>(&I))
    return BO->getOpcode() == Instruction::SDiv ||
           BO->getOpcode() == Instruction::UDiv ||
           BO->getOpcode() == Instruction::SRem ||
           BO->getOpcode() == Instruction::URem;
  return false;
}

// No store inside the loop body may touch the load's address: sinking
// the load back into the body reads the same value. Same stripped
// pointer is a clobber; a store to a provably disjoint pointer
// (underlying-object rule) is fine; anything else (calls, fences,
// unknown side effects) bails out unsafe.
bool loadSafeToSink(const LoadInst &LI, const NatLoop &L) {
  const Value *Ptr = LI.getPointerOperand();
  for (BasicBlock *BB : L.Blocks) {
    for (Instruction &J : *BB) {
      if (!J.mayWriteToMemory())
        continue;
      if (auto *SI = dyn_cast<StoreInst>(&J)) {
        if (sameAddr(SI->getPointerOperand(), Ptr))
          return false;
        if (disjointPtr(SI->getPointerOperand(), Ptr))
          continue;
        return false;
      }
      if (isa<FenceInst>(J))
        return false;
      if (J.mayHaveSideEffects())
        return false;
    }
  }
  return true;
}

// Insertion point for a sunk clone feeding a use U in block UseBB: just
// before the first non-phi instruction that is not the value itself, so
// the clone dominates U without disturbing phi placement.
Instruction *sinkPointFor(BasicBlock *UseBB) {
  return &*UseBB->getFirstNonPHIOrDbgOrLifetime();
}

// Clone Src into DstBB before InsertBefore, remapping operands through
// Map (used to chain multi-instruction defs in phase 3). Returns the
// clone, or nullptr when an operand is not available.
Instruction *cloneInto(const Instruction *Src, BasicBlock *DstBB,
                       Instruction *InsertBefore,
                       const DenseMap<const Value *, Value *> &Map) {
  Instruction *Clone = Src->clone();
  for (unsigned K = 0, E = Clone->getNumOperands(); K < E; ++K) {
    Value *Op = Clone->getOperand(K);
    auto It = Map.find(Op);
    if (It != Map.end())
      Clone->setOperand(K, It->second);
  }
  // Every operand must dominate the clone site: defined in another
  // block that dominates DstBB, a constant, an argument, or just
  // cloned above.
  for (Value *Op : Clone->operands()) {
    if (isa<Constant>(Op) || isa<Argument>(Op))
      continue;
    const auto *OI = dyn_cast<Instruction>(Op);
    if (!OI)
      return nullptr;
    if (OI->getParent() == DstBB) {
      // Defined in the target block: must come before the insert
      // point. Walk is linear but blocks here are small.
      bool Seen = false;
      for (const Instruction &J : *DstBB) {
        if (&J == OI) {
          Seen = true;
          break;
        }
        if (&J == InsertBefore)
          break;
      }
      if (!Seen) {
        Clone->deleteValue();
        return nullptr;
      }
      continue;
    }
    // Defined elsewhere: available as long as it is not inside the
    // same loop nest region being rewritten (caller checks).
  }
  Clone->insertBefore(InsertBefore->getIterator());
  copyVA(Src, Clone);
  return Clone;
}

// Phase 1: sink loop-invariant hoisted instructions from Preheader back
// into the loop body blocks that use them.
bool sinkLICM(Function &F, const SmallVectorImpl<NatLoop> &Loops,
              const DomTree &DT) {
  bool Changed = false;
  for (const NatLoop &Cur : Loops) {
    BasicBlock *Preheader = Cur.Preheader;
    if (!Preheader)
      continue;
    bool AlwaysRuns = loopAlwaysRuns(Cur, Preheader);
      // Snapshot: sinking inserts into body blocks, never Preheader.
      SmallVector<Instruction *, 16> Candidates;
      for (Instruction &I : *Preheader) {
        if (isHoistablePure(I) && I.getParent() == Preheader)
          Candidates.push_back(&I);
      }
      for (Instruction *I : Candidates) {
        if (I->getParent() != Preheader)
          continue;
        if (!operandsLoopInvariant(*I, Cur))
          continue;
        if (mayTrap(*I) && !AlwaysRuns)
          continue;
        LoadInst *LD = dyn_cast<LoadInst>(I);
        if (LD && !loadSafeToSink(*LD, Cur))
          continue;
        // Collect loop-body uses. Uses outside the loop keep the
        // preheader copy alive.
        SmallVector<Instruction *, 8> BodyUses;
        bool HasOutsideUse = false;
        for (Use &U : I->uses()) {
          auto *UI = dyn_cast<Instruction>(U.getUser());
          if (!UI) {
            HasOutsideUse = true;
            break;
          }
          if (Cur.Blocks.count(UI->getParent()))
            BodyUses.push_back(UI);
          else
            HasOutsideUse = true;
        }
        if (BodyUses.empty())
          continue;
        // Group uses by block so one clone feeds all uses in a block.
        // Placement per block: before the earliest use in that block
        // (after phis), so the clone dominates every use it feeds and
        // never lands above a phi it must dominate.
        DenseMap<BasicBlock *, SmallVector<Instruction *, 4>> ByBlock;
        for (Instruction *UI : BodyUses)
          ByBlock[UI->getParent()].push_back(UI);
        bool AllSunk = true;
        for (auto &KV : ByBlock) {
          BasicBlock *UseBB = KV.first;
          // Earliest use in block order is the insert point: the
          // clone dominates all uses in the block without crossing
          // phis. Skip when a use is itself a phi (header phis read
          // the preheader value on entry; a body clone cannot feed
          // them).
          Instruction *First = nullptr;
          bool HasPhiUse = false;
          for (Instruction *UI : KV.second) {
            if (isa<PHINode>(UI)) {
              HasPhiUse = true;
              break;
            }
            if (!First)
              First = UI;
            else {
              // Order by block position.
              bool UIBefore = false;
              for (Instruction &J : *UseBB) {
                if (&J == UI) {
                  UIBefore = true;
                  break;
                }
                if (&J == First)
                  break;
              }
              if (UIBefore)
                First = UI;
            }
          }
          if (HasPhiUse || !First) {
            AllSunk = false;
            continue;
          }
          Instruction *At = First;
          // Each operand must dominate UseBB or be defined in
          // Preheader/UseBB (the invariant/available cases), and
          // same-block operands must come before the insert point.
          bool Ok = true;
          for (Value *Op : I->operands()) {
            if (isa<Constant>(Op) || isa<Argument>(Op))
              continue;
            const auto *OI = dyn_cast<Instruction>(Op);
            if (!OI || OI->getParent() == Preheader)
              continue;
            if (OI->getParent() == UseBB) {
              bool Seen = false;
              for (const Instruction &J : *UseBB) {
                if (&J == OI) {
                  Seen = true;
                  break;
                }
                if (&J == At)
                  break;
              }
              if (!Seen) {
                Ok = false;
                break;
              }
              continue;
            }
            if (!DT.dominates(OI->getParent(), UseBB)) {
              Ok = false;
              break;
            }
          }
          if (!Ok) {
            AllSunk = false;
            continue;
          }
          DenseMap<const Value *, Value *> Empty;
          Instruction *Clone = cloneInto(I, UseBB, At, Empty);
          if (!Clone) {
            AllSunk = false;
            continue;
          }
          for (Instruction *UI : KV.second)
            UI->replaceUsesOfWith(I, Clone);
          Changed = true;
        }
        if (!HasOutsideUse && AllSunk && I->use_empty()) {
          // Every use sunk into the body: the preheader copy is the
          // optimizer's artifact, drop it.
          I->eraseFromParent();
          Changed = true;
        }
        (void)F;
    }
  }
  return Changed;
}

// Phase 2: undo GVN load forwarding. GVN deletes a reload after a
// store to the same address and rewrites its uses to the stored value
// (or, for load-after-load, to the earlier load). The -O0 shape the
// corpus shows performs the second read. This phase restores it in the
// narrow, provably-safe case: the surviving load LI in this block is
// now dead (no uses: GVN moved them all to the stored value Fwd), a
// store of Fwd to the same stripped pointer precedes LI in this block,
// and the range from that store to LI is clean (no other may-write
// touches the address, so LI would have read Fwd back). The dead load
// is then re-armed: uses of Fwd that sit after LI in this block move
// onto LI, giving back the reload-then-use idiom.
//
// Anything else (live loads, cross-block forwarding, calls between
// store and load) is left alone: re-arming there would invent reads
// the source may not perform.
bool undoLoadForwarding(Function &F) {
  bool Changed = false;
  SmallVector<BasicBlock *, 32> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);
  for (BasicBlock *BB : Blocks) {
    // Last store per stripped pointer: value stored + iterator.
    struct StoreRec {
      Value *Ptr;
      Value *Val;
      BasicBlock::iterator It;
    };
    SmallVector<StoreRec, 8> Stored;
    for (auto It = BB->begin(); It != BB->end(); ++It) {
      Instruction *I = &*It;
      if (auto *SI = dyn_cast<StoreInst>(I)) {
        Value *Ptr = SI->getPointerOperand()->stripPointerCasts();
        bool Found = false;
        for (auto &R : Stored) {
          if (R.Ptr == Ptr) {
            R.Val = SI->getValueOperand();
            R.It = It;
            Found = true;
            break;
          }
        }
        if (!Found)
          Stored.push_back({Ptr, SI->getValueOperand(), It});
        continue;
      }
      if (auto *LI = dyn_cast<LoadInst>(I)) {
        if (!LI->use_empty())
          continue; // Live load: not a GVN-forwarded reload.
        Value *Ptr = LI->getPointerOperand()->stripPointerCasts();
        for (auto &R : Stored) {
          if (R.Ptr != Ptr)
            continue;
          Value *Fwd = R.Val;
          // Store-to-load range must be clean apart from the store
          // itself: LI would have read Fwd back.
          if (!rangeClean(BB, std::next(R.It), LI->getIterator(),
                          LI->getPointerOperand()))
            break;
          // Collect post-load users of the stored value by scanning
          // operands forward: Fwd may be a constant (ConstantData has
          // no use list), so the use list cannot be queried. They are
          // what the reload would have fed.
          SmallVector<Instruction *, 4> LaterUses;
          for (auto Jt = std::next(LI->getIterator()); Jt != BB->end();
               ++Jt) {
            Instruction *UI = &*Jt;
            for (Value *Op : UI->operands()) {
              if (Op == Fwd) {
                LaterUses.push_back(UI);
                break;
              }
            }
          }
          if (LaterUses.empty())
            break;
          // Tail range must be clean too: the re-armed load reads
          // the same value the uses see today.
          if (!rangeClean(BB, std::next(LI->getIterator()), BB->end(),
                          LI->getPointerOperand()))
            break;
          for (Instruction *UI : LaterUses)
            UI->replaceUsesOfWith(Fwd, LI);
          Changed = true;
          break;
        }
        continue;
      }
      if (I->mayWriteToMemory()) {
        // A call or other writer clobbers everything except provably
        // disjoint pointers (underlying-object rule).
        SmallVector<StoreRec, 8> Kept;
        for (auto &R : Stored) {
          bool Disjoint = false;
          if (auto *SI2 = dyn_cast<StoreInst>(I))
            Disjoint = disjointPtr(SI2->getPointerOperand(), R.Ptr);
          if (Disjoint)
            Kept.push_back(R);
        }
        Stored.swap(Kept);
      }
    }
  }
  return Changed;
}

// Phase 3: undo cross-branch CSE. A pure instruction in a block P with
// exactly the successors that use it, where each use block's only other
// predecessor is P (or the use is the successor itself), is the shape
// GVN/PRE leaves when it merges identical branch computations into the
// common predecessor. Clone it into each use block.
bool undoCrossBranchCSE(Function &F, const DomTree &DT) {
  bool Changed = false;
  SmallVector<BasicBlock *, 32> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);
  for (BasicBlock *P : Blocks) {
    SmallVector<Instruction *, 16> Candidates;
    for (Instruction &I : *P) {
      if (I.isTerminator() || !isHoistablePure(I))
        continue;
      if (isa<LoadInst>(I))
        continue; // Loads need alias safety; phase 2 owns them.
      if (mayTrap(I))
        continue; // Cloning a trap into a branch changes behavior.
      Candidates.push_back(&I);
    }
    for (Instruction *I : Candidates) {
      if (I->getParent() != P || !I->hasNUsesOrMore(1))
        continue;
      // Every use must sit in a successor of P, and each use block
      // must be reachable from P on its own edge only (P dominates
      // the use block's other paths away): require P to dominate the
      // use block. Uses in P itself keep the original.
      DenseMap<BasicBlock *, SmallVector<Instruction *, 4>> ByBlock;
      bool Ok = true;
      for (Use &U : I->uses()) {
        auto *UI = dyn_cast<Instruction>(U.getUser());
        if (!UI || isa<PHINode>(UI)) {
          // Phi uses read the predecessor value on entry; a clone in
          // the successor cannot feed them. (Rewriting them would
          // only reorder phi edges, as seen on GVN'd struct_access.)
          Ok = false;
          break;
        }
        BasicBlock *UB = UI->getParent();
        if (UB == P)
          continue;
        bool IsSucc = false;
        for (BasicBlock *S : successors(P))
          if (S == UB) {
            IsSucc = true;
            break;
          }
        if (!IsSucc || !DT.dominates(P, UB)) {
          Ok = false;
          break;
        }
      }
      if (!Ok)
        continue;
      for (Use &U : I->uses()) {
        auto *UI = cast<Instruction>(U.getUser());
        if (UI->getParent() != P)
          ByBlock[UI->getParent()].push_back(UI);
      }
      if (ByBlock.empty())
        continue;
      // Operands must be available in every target block.
      for (auto &KV : ByBlock) {
        BasicBlock *UB = KV.first;
        for (Value *Op : I->operands()) {
          if (isa<Constant>(Op) || isa<Argument>(Op))
            continue;
          const auto *OI = dyn_cast<Instruction>(Op);
          if (!OI) {
            Ok = false;
            break;
          }
          if (OI->getParent() == P || OI->getParent() == UB)
            continue;
          if (!DT.dominates(OI, UB)) {
            Ok = false;
            break;
          }
        }
        if (!Ok)
          break;
      }
      if (!Ok)
        continue;
      // Clone per use block. When the use block already computes the
      // same value (partial CSE), reuse it instead of cloning.
      bool AnyCloned = false;
      for (auto &KV : ByBlock) {
        BasicBlock *UB = KV.first;
        Instruction *Existing = nullptr;
        for (Instruction &J : *UB) {
          if (&J != I && sameComputation(&J, I)) {
            Existing = &J;
            break;
          }
        }
        Instruction *Src = Existing ? Existing : nullptr;
        if (!Src) {
          DenseMap<const Value *, Value *> Empty;
          Src = cloneInto(I, UB, sinkPointFor(UB), Empty);
          if (!Src)
            continue;
        }
        for (Instruction *UI : KV.second)
          UI->replaceUsesOfWith(I, Src);
        AnyCloned = true;
      }
      if (AnyCloned) {
        Changed = true;
        if (I->use_empty())
          I->eraseFromParent();
      }
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UnhoistPass::run(Function &F,
                                             FunctionAnalysisManager &) {
  // Self-contained: local dominance + loop scan, no analysis queries,
  // so the pass loads under stock `opt -load-pass-plugin` and links
  // statically into revenant-opt unchanged.
  if (F.isDeclaration() || F.empty())
    return PreservedAnalyses::all();
  DomTree DT;
  DT.build(F);
  SmallVector<NatLoop, 8> Loops;
  findLoops(F, DT, Loops);
  bool Changed = false;
  Changed |= sinkLICM(F, Loops, DT);
  Changed |= undoLoadForwarding(F);
  Changed |= undoCrossBranchCSE(F, DT);
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: LunaTxPipeline, formerly MuseTxPipeline) links this library
// statically and registers the `revenant-unhoist` name in
// Pipeline/Register.cpp, so the entry point is compiled only for the
// standalone plugin DLL: a static link of every transform would otherwise
// export llvmGetPassPluginInfo more than once. Windows needs the explicit
// dllexport; without it opt fails with "Plugin entry point not found".
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
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnhoist", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unhoist") {
                    PM.addPass(revenant::UnhoistPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
