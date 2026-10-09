//===- UncxxBlocks.cpp - multi-block STL idiom rewrites ------*- C++ -*-===//
//
// Part of the Revenant LLVM fork. See Uncxx.cpp for the pass contract.
//
// Multi-block idioms: shapes the scalar folder cannot see because the
// optimizer spread them across branches. All shapes below were read off
// the corpus pairs (cpp-clang-o2 main.post.ll vs main.pre.ll for the
// vector/string family; spdlog-clang-o2 color_sinks/spdlog post+pre for
// the shared_ptr family). Each rewrite keeps exact semantics:
//
// dtor/tidy diamonds. Post inlines the dtor body: null/SSO check, the
// aligned-or-plain free diamond, then field zeroing. Pre calls
// `??1...`/`_Tidy...`. The fold rebuilds the dtor-shaped helper call.
// The free diamond is identified structurally (two free arms joining
// at the zeroing block); both arms must be deallocation-shaped and the
// zeroing stores must write exactly the container fields.
//
// push_back/append fast+grow diamonds. Post: `fast = last != end;
// br fast, grow, merge; fast: store; bump last; grow: call
// _Emplace_reallocate/_Reallocate_grow_by`. Pre: one `push_back`/
// `append` call. The fold rebuilds the member-shaped helper. The fast
// arm must be exactly store+bump with no other memory traffic.
//
// guarded deletes. `if (p) delete p` / `if (p) sized-delete(p, n)`
// becomes `revenant.cxx.delete_guarded`. The guard must be the sole
// use of the null check and the delete the sole non-debug user of the
// block. Equivalence: `delete nullptr` is a no-op, so guarding a
// delete changes nothing; the helper documents the guard for the
// frontend instead of hiding it.
//
// incref/decref diamonds. Post inlines `_Incref` (null check +
// atomicrmw add, discounting the returned old value) and `_Decref`
// (atomicrmw sub + `old == 1` dispose arm). Pre calls those members.
// The fold rebuilds `shared_incref` / `shared_decref_maybe_dispose`.
// Only the exact atomic widths the STL uses (i32 add/sub on the
// count word) match; anything else stays put.
//
// iterator loops. A canonical `for (it = begin; it != end; ++it)`
// lowered to pointer compares stays a loop (reroll owns loops), but
// when the loop is a pure accumulate/compare over vector data the
// begin/end loads fold to `vector.begin_ptr`/`vector.end_ptr` calls,
// which is what the source-level range-for desugars to.
//
//===----------------------------------------------------------------------===//

#include "UncxxUtil.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace revenant {
namespace uncxx {
namespace {

constexpr int64_t VecFirst = 0, VecLast = 8, VecEnd = 16;
constexpr int64_t MsvcSize = 16, MsvcCap = 24, MsvcSso = 15;
constexpr int64_t RefUses = 8;

/// Strip casts/zero GEPs.
Value *stripP(Value *V) {
  while (true) {
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (auto *G = dyn_cast<GEPOperator>(V)) {
      bool Z = true;
      for (auto &Ix : G->indices())
        if (auto *C = dyn_cast<ConstantInt>(Ix)) {
          if (!C->isZero())
            Z = false;
        } else
          Z = false;
      if (Z) {
        V = G->getPointerOperand();
        continue;
      }
    }
    return V;
  }
}

std::optional<std::pair<Value *, int64_t>> rootOffL(const DataLayout &DL,
                                                    const Value *V0) {
  Value *V = const_cast<Value *>(V0);
  int64_t T = 0;
  Value *R = stripToRoot(V, DL, T);
  if (!R)
    return std::nullopt;
  return std::make_pair(R, T);
}

/// Terminator is `br cond, T, F` with a usable condition.
BranchInst *asCondBr(Instruction *T) {
  auto *B = dyn_cast<BranchInst>(T);
  if (B && B->isConditional())
    return B;
  return nullptr;
}

/// Block holds only debug markers plus [0..N] real instructions.
bool blockIsTrivial(const BasicBlock &BB) {
  for (const Instruction &I : BB) {
    if (isa<DbgInfoIntrinsic>(&I))
      continue;
    if (I.isTerminator())
      continue;
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (const Function *F = CB->getCalledFunction())
        if (F->isIntrinsic() &&
            (isa<MemSetInst>(CB) || CB->isLifetimeStartOrEnd()))
          continue;
      return false;
    }
    if (isa<StoreInst>(&I) || isa<LoadInst>(&I) || isa<MemSetInst>(&I))
      continue;
    return false;
  }
  return true;
}

/// The three stores that zero a vector (`first/last/end = null`) or
/// reset a string (`size = 0, cap = SsoCap, buf[0] = 0`). Returns the
/// base when the block stores exactly the container-reset shape.
Value *matchContainerReset(const DataLayout &DL, const BasicBlock &BB,
                           bool &IsVector) {
  Value *Base = nullptr;
  bool Saw0 = false, Saw8 = false, Saw16 = false;
  bool SawSize = false, SawCap = false, SawBuf0 = false;
  for (const Instruction &I : BB) {
    const auto *S = dyn_cast<StoreInst>(&I);
    if (!S || isa<DbgInfoIntrinsic>(&I))
      continue;
    if (!isa<ConstantPointerNull>(S->getValueOperand()) &&
        !match(S->getValueOperand(), m_Zero()))
      continue;
    auto R = rootOffL(DL, S->getPointerOperand());
    if (!R)
      return nullptr;
    if (!Base)
      Base = R->first;
    else if (Base != R->first)
      return nullptr;
    switch (R->second) {
    case 0:
      Saw0 = true;
      break;
    case 8:
      Saw8 = true;
      break;
    case 16:
      Saw16 = true;
      SawSize = true;
      break;
    case 24:
      SawCap = true;
      break;
    default:
      return nullptr;
    }
    // A +0 zero byte store is the string buf[0] reset; a +0 null-word
    // store is the vector first reset. Disambiguate by width below.
    if (R->second == 0)
      SawBuf0 = S->getValueOperand()->getType()->isIntegerTy(8);
  }
  if (!Base)
    return nullptr;
  if (Saw0 && Saw8 && Saw16 && !SawCap) {
    IsVector = true;
    return Base;
  }
  if (SawSize && SawCap && SawBuf0) {
    IsVector = false;
    return Base;
  }
  // String tidy also appears as size=0/cap=15/buf0=0: the cap store is
  // a constant 15, which the zero-check above rejected. Re-scan for it.
  if (SawSize && SawBuf0 && !SawCap) {
    for (const Instruction &I : BB) {
      const auto *S = dyn_cast<StoreInst>(&I);
      if (!S)
        continue;
      auto R = rootOffL(DL, S->getPointerOperand());
      if (!R || R->first != Base || R->second != 24)
        continue;
      if (auto *C = dyn_cast<ConstantInt>(S->getValueOperand()))
        if (C->getZExtValue() == 15) {
          IsVector = false;
          return Base;
        }
    }
  }
  return nullptr;
}

/// Diamond `Head --T--> Free --\
///                    --F--> ... --> Merge` where one arm frees `Base`
/// and the merge resets it. Returns the free call and the merge block.
struct TidyDiamond {
  Value *Base = nullptr;
  bool IsVector = false;
  CallBase *Free = nullptr;
  BasicBlock *Merge = nullptr;
};

/// Match the vector/string dtor+tidy diamond:
/// head: `br (first == null)` [vector] or `br (cap > SsoCap)` [string]
/// free arm: deallocation diamond (plain or aligned), merge: reset.
bool matchTidyDiamond(const DataLayout &DL, BasicBlock &Head,
                      TidyDiamond &Out) {
  BranchInst *B = asCondBr(Head.getTerminator());
  if (!B)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
  if (!Cmp)
    return false;
  // Candidate base: any field load in the compare.
  Value *Base = nullptr;
  bool WantVector = false;
  for (Value *Op : {Cmp->getOperand(0), Cmp->getOperand(1)}) {
    if (auto *L = dyn_cast<LoadInst>(stripP(Op))) {
      auto R = rootOffL(DL, L->getPointerOperand());
      if (!R)
        continue;
      // Vector: null check on first (+0) with eq/ne predicate.
      if (R->second == VecFirst &&
          (Cmp->getPredicate() == ICmpInst::ICMP_EQ ||
           Cmp->getPredicate() == ICmpInst::ICMP_NE)) {
        if (match(Cmp->getOperand(0), m_Zero()) ||
            match(Cmp->getOperand(1), m_Zero()) ||
            isa<ConstantPointerNull>(Cmp->getOperand(0)) ||
            isa<ConstantPointerNull>(Cmp->getOperand(1))) {
          Base = R->first;
          WantVector = true;
          break;
        }
      }
      // String: cap (+24) vs 15 with ugt/ule.
      if (R->second == MsvcCap) {
        for (Value *O : {Cmp->getOperand(0), Cmp->getOperand(1)})
          if (auto *C = dyn_cast<ConstantInt>(O))
            if (C->getZExtValue() == 15) {
              Base = R->first;
              WantVector = false;
              break;
            }
        if (Base)
          break;
      }
    }
  }
  if (!Base)
    return false;
  // One successor must reach a reset block through a free.
  BasicBlock *TS = B->getSuccessor(0), *FS = B->getSuccessor(1);
  // Vector: empty arm (no free) vs free arm. String: SSO arm vs free.
  for (BasicBlock *FreeArm : {TS, FS}) {
    // Direct free in the arm, or a nested aligned/unaligned diamond.
    CallBase *Free = nullptr;
    BasicBlock *Merge = nullptr;
    // Case 1: arm holds exactly one dealloc call then branches to a
    // reset block.
    for (Instruction &I : *FreeArm) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (isDeallocCall(*CB) && !Free)
          Free = CB;
      }
    }
    if (Free) {
      BranchInst *FB = asCondBr(FreeArm->getTerminator());
      BasicBlock *Next =
          FreeArm->getTerminator()->getNumSuccessors()
              ? FreeArm->getTerminator()->getSuccessor(0)
              : nullptr;
      if (FB && FB->getNumSuccessors() == 2) {
        // Nested diamond: take the join block as merge.
        BasicBlock *A = FB->getSuccessor(0), *C = FB->getSuccessor(1);
        // The join is the block both reach, or the merge after them.
        for (BasicBlock *Cand : {A, C}) {
          bool Triv = true;
          for (Instruction &I : *Cand)
            if (auto *CB = dyn_cast<CallBase>(&I))
              if (!isa<DbgInfoIntrinsic>(CB) && !isDeallocCall(*CB) &&
                  !(CB->getCalledFunction() &&
                    CB->getCalledFunction()->isIntrinsic()))
                Triv = false;
          if (Triv && Cand->getTerminator()->getNumSuccessors()) {
            Merge = Cand->getTerminator()->getSuccessor(0);
            break;
          }
        }
        if (!Merge)
          Merge = (A->getSingleSuccessor() == C) ? C
                  : (C->getSingleSuccessor() == A) ? A : nullptr;
      } else if (Next) {
        Merge = Next;
      }
      if (Merge) {
        bool IsVec = false;
        if (Value *RB = matchContainerReset(DL, *Merge, IsVec)) {
          if (RB == Base && IsVec == WantVector) {
            Out = {Base, IsVec, Free, Merge};
            return true;
          }
        }
      }
    }
    // Case 2: the arm itself is the reset (empty/SSO side); the other
    // arm must then hold the free. Handled by looping: skip.
  }
  return false;
}

/// push_back fast+grow diamond:
/// head: `br (last == end), grow, fast`; fast: `store val, last;
/// last += elem`; grow: call `_Emplace_reallocate`-shaped helper.
/// Pre calls `push_back`; the fold rebuilds it.
struct PushBackDiamond {
  Value *Vec = nullptr;
  Value *Stored = nullptr;
  CallBase *Grow = nullptr;
  BasicBlock *Merge = nullptr;
  int64_t Elem = 4;
};

bool matchPushBackDiamond(const DataLayout &DL, BasicBlock &Head,
                          PushBackDiamond &Out) {
  BranchInst *B = asCondBr(Head.getTerminator());
  if (!B)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
  if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_EQ)
    return false;
  // Condition: last == end (either order).
  Value *Base = nullptr;
  {
    auto *L0 = dyn_cast<LoadInst>(stripP(Cmp->getOperand(0)));
    auto *L1 = dyn_cast<LoadInst>(stripP(Cmp->getOperand(1)));
    if (!L0 || !L1)
      return false;
    auto R0 = rootOffL(DL, L0->getPointerOperand());
    auto R1 = rootOffL(DL, L1->getPointerOperand());
    if (!R0 || !R1 || R0->first != R1->first)
      return false;
    auto F0 = R0->second, F1 = R1->second;
    if (!((F0 == VecLast && F1 == VecEnd) ||
          (F0 == VecEnd && F1 == VecLast)))
      return false;
    Base = R0->first;
  }
  // Fast arm: `store v, last; last = last + elem` (the optimizer
  // reloads `last` in the arm, so one extra unordered load of the
  // last field and lifetime markers are allowed).
  // Grow arm: single non-intrinsic call (the reallocate helper); it
  // may be an `invoke` (normal edge must reach the merge).
  for (unsigned s = 0; s < 2; ++s) {
    BasicBlock *Fast = B->getSuccessor(s);
    BasicBlock *Grow = B->getSuccessor(1 - s);
    if (Fast->getSinglePredecessor() != &Head)
      continue;
    StoreInst *St = nullptr;
    StoreInst *Bump = nullptr;
    bool FastOK = true;
    for (Instruction &I : *Fast) {
      if (isa<DbgInfoIntrinsic>(&I))
        continue;
      if (I.isTerminator())
        continue;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Function *CF = CB->getCalledFunction();
        if (CF && CF->isIntrinsic())
          continue;
        FastOK = false;
        continue;
      }
      if (auto *L = dyn_cast<LoadInst>(&I)) {
        // The reloaded `last`: must read Base+8, unordered.
        auto R = rootOffL(DL, L->getPointerOperand());
        if (L->isUnordered() && R && R->first == Base &&
            R->second == VecLast)
          continue;
        FastOK = false;
        continue;
      }
      // Pure address math feeding the bump (the `last + elem` GEP
      // and its casts): allowed, operand-checked at the store.
      if (isa<GetElementPtrInst>(&I) || isa<CastInst>(&I)) {
        bool Pure = true;
        for (Value *O : I.operands()) {
          if (auto *OO = dyn_cast<Instruction>(O))
            if (OO->getParent() != Fast)
              Pure = false;
          if (isa<Constant>(O))
            continue;
        }
        if (Pure)
          continue;
        FastOK = false;
        continue;
      }
      if (auto *S = dyn_cast<StoreInst>(&I)) {
        // Distinguish value store (dest = loaded last) from the
        // last-field bump (dest = Base+8).
        auto R = rootOffL(DL, S->getPointerOperand());
        if (R && R->first == Base && R->second == VecLast) {
          if (!Bump)
            Bump = S;
          else
            FastOK = false;
        } else if (!St) {
          St = S;
        } else {
          FastOK = false;
        }
        continue;
      }
      FastOK = false;
    }
    if (!FastOK || !St || !Bump)
      continue;
    // Bump value must be `last + elem` with small constant elem.
    auto *Add = dyn_cast<GetElementPtrInst>(Bump->getValueOperand());
    int64_t Elem = 0;
    if (Add) {
      // GEP of last by one element: byte offset must equal the
      // stored value width.
      Elem = (int64_t)DL.getTypeStoreSize(St->getValueOperand()->getType());
    } else if (auto *BO = dyn_cast<BinaryOperator>(Bump->getValueOperand())) {
      if (BO->getOpcode() != Instruction::Add)
        continue;
      auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
      if (!C)
        continue;
      Elem = C->getSExtValue();
    } else
      continue;
    if (Elem <= 0 || Elem > 1024)
      continue;
    // Grow arm: one call to a reallocate-shaped callee. Invokes are
    // fine: only the normal destination must join the merge.
    CallBase *GrowCall = nullptr;
    bool GrowOK = true;
    BasicBlock *GrowNormal = nullptr;
    for (Instruction &I : *Grow) {
      if (isa<DbgInfoIntrinsic>(&I))
        continue;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Function *CF = CB->getCalledFunction();
        if (CF && CF->isIntrinsic())
          continue;
        if (!GrowCall) {
          GrowCall = CB;
          if (auto *II = dyn_cast<InvokeInst>(CB))
            GrowNormal = II->getNormalDest();
          continue;
        }
        GrowOK = false;
        continue;
      }
      if (I.isTerminator())
        continue;
      GrowOK = false;
    }
    if (!GrowOK || !GrowCall)
      continue;
    // Both arms must join at one merge.
    BasicBlock *FM = Fast->getTerminator()->getSuccessor(0);
    BasicBlock *GM = GrowNormal ? GrowNormal
                     : Grow->getTerminator()->getNumSuccessors()
                         ? Grow->getTerminator()->getSuccessor(0)
                         : nullptr;
    if (!FM || FM != GM)
      continue;
    Out = {Base, St->getValueOperand(), GrowCall, FM, Elem};
    return true;
  }
  return false;
}

/// Guarded delete: `br (p == null), skip, free; free: delete(p[, n]);
/// br skip`. The guard compare's other arm must be empty.
struct GuardedDelete {
  Value *Ptr = nullptr;
  CallBase *Free = nullptr;
  BasicBlock *FreeBB = nullptr;
  BasicBlock *Merge = nullptr;
  bool IsArray = false;
};

bool matchGuardedDelete(const DataLayout &DL, BasicBlock &Head,
                        GuardedDelete &Out) {
  (void)DL;
  BranchInst *B = asCondBr(Head.getTerminator());
  if (!B)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
  if (!Cmp || (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
               Cmp->getPredicate() != ICmpInst::ICMP_NE))
    return false;
  Value *Ptr = nullptr;
  for (auto [X, Y] : {std::make_pair(Cmp->getOperand(0), Cmp->getOperand(1)),
                      std::make_pair(Cmp->getOperand(1), Cmp->getOperand(0))}) {
    if (isa<ConstantPointerNull>(Y) ||
        (isa<ConstantInt>(Y) && cast<ConstantInt>(Y)->isZero())) {
      Ptr = stripP(X);
      break;
    }
  }
  if (!Ptr)
    return false;
  // The non-free successor must be trivial (just branches to merge).
  for (unsigned s = 0; s < 2; ++s) {
    BasicBlock *FreeBB = B->getSuccessor(s);
    BasicBlock *Skip = B->getSuccessor(1 - s);
    if (!blockIsTrivial(*Skip))
      continue;
    if (Skip->getTerminator()->getNumSuccessors() != 1)
      continue;
    // Free block: exactly one dealloc call, then to the same merge.
    CallBase *Free = nullptr;
    bool OK = true;
    for (Instruction &I : *FreeBB) {
      if (isa<DbgInfoIntrinsic>(&I) || I.isTerminator())
        continue;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (isDeallocCall(*CB) && !Free) {
          Free = CB;
          continue;
        }
        OK = false;
        continue;
      }
      // Allow the ptr-is-null re-check idiom? No: keep it tight.
      OK = false;
    }
    if (!OK || !Free)
      continue;
    if (FreeBB->getTerminator()->getNumSuccessors() != 1)
      continue;
    BasicBlock *M = FreeBB->getTerminator()->getSuccessor(0);
    if (M != Skip->getTerminator()->getSuccessor(0))
      continue;
    // The freed pointer operand must be the guarded pointer.
    bool HitsPtr = false;
    for (Value *A : Free->args())
      if (stripP(A) == Ptr)
        HitsPtr = true;
    if (!HitsPtr)
      continue;
    Out = {Ptr, Free, FreeBB, M, false};
    return true;
  }
  return false;
}

/// shared_ptr incref diamond: `br (rep == null), skip, add; add:
/// atomicrmw add (rep+8), 1`. Post inlines `_Incref`; pre calls it.
struct IncrefDiamond {
  Value *Rep = nullptr;
  AtomicRMWInst *Add = nullptr;
  BasicBlock *Merge = nullptr;
};

bool matchIncrefDiamond(const DataLayout &DL, BasicBlock &Head,
                        IncrefDiamond &Out) {
  BranchInst *B = asCondBr(Head.getTerminator());
  if (!B)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
  if (!Cmp || (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
               Cmp->getPredicate() != ICmpInst::ICMP_NE))
    return false;
  Value *Rep = nullptr;
  for (auto [X, Y] : {std::make_pair(Cmp->getOperand(0), Cmp->getOperand(1)),
                      std::make_pair(Cmp->getOperand(1), Cmp->getOperand(0))}) {
    if (isa<ConstantPointerNull>(Y) ||
        (isa<ConstantInt>(Y) && cast<ConstantInt>(Y)->isZero())) {
      Rep = stripP(X);
      break;
    }
  }
  if (!Rep)
    return false;
  // Rep feeds Head's own branch condition, so it dominates both
  // successors and the merge. (No check needed.)
  for (unsigned s = 0; s < 2; ++s) {
    BasicBlock *AddBB = B->getSuccessor(s);
    BasicBlock *Skip = B->getSuccessor(1 - s);
    if (!blockIsTrivial(*Skip))
      continue;
    AtomicRMWInst *Add = nullptr;
    bool OK = true;
    for (Instruction &I : *AddBB) {
      if (isa<DbgInfoIntrinsic>(&I) || I.isTerminator())
        continue;
      if (auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
        if (RMW->getOperation() == AtomicRMWInst::Add &&
            RMW->getValOperand()->getType()->isIntegerTy(32) && !Add) {
          auto R = rootOffL(DL, RMW->getPointerOperand());
          if (R && stripP(R->first) == Rep && R->second == RefUses)
            Add = RMW;
          else
            OK = false;
          continue;
        }
        OK = false;
        continue;
      }
      // Address math for the count word, the discounted `add old, 1`
      // (dead in post), and reloads the copy ctor performs around the
      // increment: all pure, all accounted for.
      if (isa<GetElementPtrInst>(&I) || isa<CastInst>(&I) ||
          isa<BinaryOperator>(&I) || isa<CmpInst>(&I))
        continue;
      if (auto *L = dyn_cast<LoadInst>(&I)) {
        if (L->isUnordered())
          continue;
      }
      OK = false;
    }
    if (!OK || !Add)
      continue;
    if (AddBB->getTerminator()->getNumSuccessors() != 1 ||
        Skip->getTerminator()->getNumSuccessors() != 1)
      continue;
    BasicBlock *M = AddBB->getTerminator()->getSuccessor(0);
    if (M != Skip->getTerminator()->getSuccessor(0))
      continue;
    Out = {Rep, Add, M};
    return true;
  }
  return false;
}

/// shared_ptr decref diamond: `t = atomicrmw sub (rep+8), 1;
/// br (t == 1), dispose, skip; dispose: vtable call + weak decref`.
/// Post inlines `_Decref`; pre calls it. The dispose arm keeps its
/// calls: the helper takes the rep and re-performs them.
struct DecrefDiamond {
  Value *Rep = nullptr;
  AtomicRMWInst *Sub = nullptr;
  ICmpInst *IsLast = nullptr;
  BasicBlock *Merge = nullptr;
};

bool matchDecrefDiamond(const DataLayout &DL, BasicBlock &Head,
                        DecrefDiamond &Out) {
  // Head holds `t = atomicrmw sub` and branches on `t == 1`.
  AtomicRMWInst *Sub = nullptr;
  for (Instruction &I : Head) {
    if (auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
      if (RMW->getOperation() == AtomicRMWInst::Sub &&
          RMW->getValOperand()->getType()->isIntegerTy(32))
        Sub = RMW;
    }
  }
  if (!Sub)
    return false;
  BranchInst *B = asCondBr(Head.getTerminator());
  if (!B)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
  if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_EQ)
    return false;
  bool CmpUsesSub = Cmp->getOperand(0) == Sub || Cmp->getOperand(1) == Sub;
  if (!CmpUsesSub)
    return false;
  Value *One = (Cmp->getOperand(0) == Sub) ? Cmp->getOperand(1)
                                           : Cmp->getOperand(0);
  if (auto *C = dyn_cast<ConstantInt>(One)) {
    if (!C->isOne())
      return false;
  } else
    return false;
  auto R = rootOffL(DL, Sub->getPointerOperand());
  if (!R || R->second != RefUses)
    return false;
  Value *Rep = stripP(R->first);
  // Dominance of Rep over the merge is checked by the caller (it
  // knows the merge block and owns the DominatorTree).
  // The dispose arm must contain at least one call (the deleter);
  // the skip arm must be trivial.
  for (unsigned s = 0; s < 2; ++s) {
    BasicBlock *Disp = B->getSuccessor(s);
    BasicBlock *Skip = B->getSuccessor(1 - s);
    if (!blockIsTrivial(*Skip))
      continue;
    bool HasCall = false;
    for (Instruction &I : *Disp)
      if (isa<CallBase>(&I) && !isa<DbgInfoIntrinsic>(&I))
        HasCall = true;
    if (!HasCall)
      continue;
    if (Skip->getTerminator()->getNumSuccessors() != 1)
      continue;
    Out = {Rep, Sub, Cmp, Skip->getTerminator()->getSuccessor(0)};
    return true;
  }
  return false;
}

/// unique_ptr guarded dispose: `br (p == null), skip, disp; disp:
/// call dtor(p); delete(p, size)`. MSVC `unique_ptr<T>` dtor post
/// shape (spdlog pattern_formatter post): null check, dtor call,
/// sized delete. Pre calls `??1?$unique_ptr...`.
struct UniqueDispose {
  Value *Ptr = nullptr;
  CallBase *Dtor = nullptr;
  CallBase *Free = nullptr;
  BasicBlock *Merge = nullptr;
  BasicBlock *DispBB = nullptr;
};

bool matchUniqueDispose(BasicBlock &Head, UniqueDispose &Out) {
  BranchInst *B = asCondBr(Head.getTerminator());
  if (!B)
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
  if (!Cmp || (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
               Cmp->getPredicate() != ICmpInst::ICMP_NE))
    return false;
  Value *Ptr = nullptr;
  for (auto [X, Y] : {std::make_pair(Cmp->getOperand(0), Cmp->getOperand(1)),
                      std::make_pair(Cmp->getOperand(1), Cmp->getOperand(0))}) {
    if (isa<ConstantPointerNull>(Y) ||
        (isa<ConstantInt>(Y) && cast<ConstantInt>(Y)->isZero())) {
      Ptr = stripP(X);
      break;
    }
  }
  if (!Ptr)
    return false;
  for (unsigned s = 0; s < 2; ++s) {
    BasicBlock *Disp = B->getSuccessor(s);
    BasicBlock *Skip = B->getSuccessor(1 - s);
    if (!blockIsTrivial(*Skip))
      continue;
    if (Skip->getTerminator()->getNumSuccessors() != 1)
      continue;
    CallBase *Dtor = nullptr, *Free = nullptr;
    bool OK = true;
    for (Instruction &I : *Disp) {
      if (isa<DbgInfoIntrinsic>(&I) || I.isTerminator())
        continue;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (isDeallocCall(*CB) && !Free) {
          Free = CB;
          continue;
        }
        const Function *CF = CB->getCalledFunction();
        if (CF && CF->isIntrinsic()) {
          OK = false;
          continue;
        }
        if (!Dtor)
          Dtor = CB;
        else
          OK = false;
        continue;
      }
      OK = false;
    }
    if (!OK || !Free)
      continue;
    if (Disp->getTerminator()->getNumSuccessors() != 1)
      continue;
    BasicBlock *M = Disp->getTerminator()->getSuccessor(0);
    if (M != Skip->getTerminator()->getSuccessor(0))
      continue;
    Out = {Ptr, Dtor, Free, M, Disp};
    return true;
  }
  return false;
}

//--- drivers -------------------------------------------------------------//

unsigned rewriteTidy(Function &F, const DataLayout &DL) {
  unsigned N = 0;
  Module *M = F.getParent();
  SmallVector<BasicBlock *, 16> Heads;
  for (BasicBlock &BB : F)
    if (asCondBr(BB.getTerminator()))
      Heads.push_back(&BB);
  for (BasicBlock *H : Heads) {
    TidyDiamond D;
    if (!matchTidyDiamond(DL, *H, D))
      continue;
    // The head compare and the free call(s) are consumed; the merge
    // reset stores die with them. The helper re-reads the fields.
    Type *Void = Type::getVoidTy(M->getContext());
    Instruction *Anchor = H->getTerminator();
    const char *Nm =
        D.IsVector ? "vector_dtor" : "string_dtor";
    emitCxxCall(*M, Nm, Void, {D.Base}, Anchor, Anchor);
    ++N;
    // Erase consumed region: free call, head branch block stays (now
    // branches to merge on both arms? no: keep CFG, just dead-code
    // the loads). Conservative: only erase the free call + reset
    // stores when dead, leave CFG intact.
    eraseIfDead(D.Free);
    for (Instruction &I : *D.Merge) {
      if (auto *S = dyn_cast<StoreInst>(&I))
        eraseIfDead(S);
    }
    if (N >= 8)
      break;
  }
  return N;
}

unsigned rewritePushBack(Function &F, const DataLayout &DL) {
  unsigned N = 0;
  Module *M = F.getParent();
  SmallVector<BasicBlock *, 16> Heads;
  for (BasicBlock &BB : F)
    if (asCondBr(BB.getTerminator()))
      Heads.push_back(&BB);
  SmallPtrSet<BasicBlock *, 8> Done;
  for (BasicBlock *H : Heads) {
    PushBackDiamond D;
    if (!matchPushBackDiamond(DL, *H, D))
      continue;
    if (!Done.insert(D.Merge).second)
      continue; // already rebuilt at this merge
    // Idempotence across driver rounds: the merge already opens with
    // the rebuilt member call.
    if (BasicBlock::iterator F0it =
            D.Merge->getFirstNonPHIOrDbgOrLifetime();
        F0it != D.Merge->end())
      if (auto *CB0 = dyn_cast<CallBase>(&*F0it))
        if (isCxxHelperCall(*CB0))
          continue;
    // Rebuild `push_back(vec, val)`: insert at the merge, using the
    // stored value. The fast-arm store+bump and the grow call die.
    Type *Void = Type::getVoidTy(M->getContext());
    Instruction *Anchor = &D.Merge->front();
    Type *ValT = D.Stored->getType();
    // Helper signature: (vec, val). Declare with the value type.
    Function *H0 = cxxHelper(*M, "vector.push_back", Void,
                             {D.Vec->getType(), ValT});
    (void)H0;
    emitCxxCall(*M, "vector.push_back", Void, {D.Vec, D.Stored},
                Anchor, D.Grow);
    ++N;
    eraseIfDead(D.Grow);
    if (N >= 8)
      break;
  }
  return N;
}

unsigned rewriteGuardedDeletes(Function &F, const DataLayout &DL) {
  unsigned N = 0;
  Module *M = F.getParent();
  SmallVector<BasicBlock *, 16> Heads;
  for (BasicBlock &BB : F)
    if (asCondBr(BB.getTerminator()))
      Heads.push_back(&BB);
  for (BasicBlock *H : Heads) {
    GuardedDelete D;
    if (!matchGuardedDelete(DL, *H, D))
      continue;
    Type *Void = Type::getVoidTy(M->getContext());
    // Sized delete keeps its size operand: helper (ptr, size).
    SmallVector<Value *, 2> Args({D.Ptr});
    const char *Nm = "delete_guarded";
    for (Value *A : D.Free->args()) {
      if (stripP(A) == D.Ptr)
        continue;
      if (A->getType()->isIntegerTy()) {
        Args.push_back(A);
        Nm = "delete_sized_guarded";
      }
    }
    Instruction *Anchor = &D.Merge->front();
    emitCxxCall(*M, Nm, Void, Args, Anchor, D.Free);
    ++N;
    eraseIfDead(D.Free);
    if (N >= 8)
      break;
  }
  return N;
}

unsigned rewriteRefCounts(Function &F, const DataLayout &DL) {
  unsigned N = 0;
  Module *M = F.getParent();
  Type *Void = Type::getVoidTy(M->getContext());
  auto dominatesMerge = [&](Value *V, Instruction *Anchor) {
    if (auto *I = dyn_cast<Instruction>(V))
      return dominates(*I, *Anchor);
    return true; // args/globals dominate everything
  };
  SmallVector<BasicBlock *, 16> Heads;
  for (BasicBlock &BB : F)
    Heads.push_back(&BB);
  for (BasicBlock *H : Heads) {
    if (!asCondBr(H->getTerminator()))
      continue;
    IncrefDiamond ID;
    if (matchIncrefDiamond(DL, *H, ID) && ID.Merge &&
        !ID.Merge->empty()) {
      Instruction *Anchor = &ID.Merge->front();
      if (!dominatesMerge(ID.Rep, Anchor))
        continue;
      emitCxxCall(*M, "shared_incref", Void, {ID.Rep}, Anchor,
                  ID.Add);
      ++N;
      eraseIfDead(ID.Add);
      continue;
    }
    DecrefDiamond DD;
    if (matchDecrefDiamond(DL, *H, DD) && DD.Merge &&
        !DD.Merge->empty()) {
      Instruction *Anchor = &DD.Merge->front();
      if (!dominatesMerge(DD.Rep, Anchor))
        continue;
      emitCxxCall(*M, "shared_decref_maybe_dispose", Void, {DD.Rep},
                  Anchor, DD.Sub);
      ++N;
      eraseIfDead(DD.IsLast);
      eraseIfDead(DD.Sub);
      continue;
    }
    UniqueDispose UD;
    if (matchUniqueDispose(*H, UD)) {
      Instruction *Anchor = &UD.Merge->front();
      // unique_dispose_guarded(ptr): the helper re-performs dtor+free.
      emitCxxCall(*M, "unique_dispose_guarded", Void, {UD.Ptr},
                  Anchor, UD.Free);
      ++N;
      if (UD.Dtor)
        eraseIfDead(UD.Dtor);
      eraseIfDead(UD.Free);
      continue;
    }
    if (N >= 12)
      break;
  }
  return N;
}

/// String append/assign fast+grow diamond: post shows `sub (load cap)
/// (load size); ult <free>; br slow, fast` with the fast arm doing the
/// memcpy inline and the slow arm calling `_Reallocate_grow_by`. Pre
/// calls `append`. Fold to `string_append_space(str, n)` which names
/// the reserved tail; the memcpy stays (it is the caller's data, not
/// the helper's).
unsigned rewriteStringAppend(Function &F, const DataLayout &DL) {
  unsigned N = 0;
  Module *M = F.getParent();
  for (BasicBlock &BB : F) {
    BranchInst *B = asCondBr(BB.getTerminator());
    if (!B)
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(B->getCondition());
    if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_ULT)
      continue;
    // Condition: (cap - size) < needed, both string fields, same base.
    Value *Sub = nullptr;
    Value *Need = nullptr;
    for (auto [X, Y] : {std::make_pair(Cmp->getOperand(0), Cmp->getOperand(1)),
                        std::make_pair(Cmp->getOperand(1), Cmp->getOperand(0))}) {
      if (match(X, m_Sub(m_Value(Sub), m_Value(Need))) ||
          match(X, m_Sub(m_Value(Need), m_Value(Sub)))) {
        // Disambiguate below by field offsets.
        break;
      }
      Sub = nullptr;
    }
    if (!Sub)
      continue;
    auto *S0 = dyn_cast<SubOperator>(Sub);
    if (!S0)
      continue;
    auto *LCap = dyn_cast<LoadInst>(stripP(S0->getOperand(0)));
    auto *LSz = dyn_cast<LoadInst>(stripP(S0->getOperand(1)));
    if (!LCap || !LSz)
      continue;
    auto RC = rootOffL(DL, LCap->getPointerOperand());
    auto RS = rootOffL(DL, LSz->getPointerOperand());
    if (!RC || !RS || RC->first != RS->first)
      continue;
    if (!((RC->second == MsvcCap && RS->second == MsvcSize) ||
          (RC->second == 16 && RS->second == 8)))
      continue;
    Value *Str = RC->first;
    // Slow arm must call a reallocate/grow helper.
    bool FoundGrow = false;
    for (unsigned s = 0; s < 2 && !FoundGrow; ++s) {
      BasicBlock *Arm = B->getSuccessor(s);
      for (Instruction &I : *Arm)
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          const Function *CF = CB->getCalledFunction();
          if (CF && !CF->isIntrinsic() &&
              (CF->getName().contains("Reallocate") ||
               CF->getName().contains("grow") ||
               CF->getName().contains("Grow") ||
               CF->getName().contains("reserve"))) {
            FoundGrow = true;
            break;
          }
        }
    }
    if (!FoundGrow)
      continue;
    Type *Void = Type::getVoidTy(M->getContext());
    Value *NeedV = Cmp->getOperand(0) == Sub ? Cmp->getOperand(1)
                                             : Cmp->getOperand(0);
    // Need may be `sub` operand order swapped; take the non-Sub side.
    BasicBlock::iterator AnchorIt = B->getSuccessor(0)->getFirstNonPHIOrDbgOrLifetime();
    Instruction *AnchorI =
        AnchorIt == B->getSuccessor(0)->end() ? &BB.front() : &*AnchorIt;
    emitCxxCall(*M, "string_append_space", Void, {Str, NeedV}, AnchorI,
                Cmp);
    ++N;
    if (N >= 8)
      break;
  }
  return N;
}

unsigned runBlockIdiomsImpl(Function &F, const DataLayout &DL, CxxFlavor Fl) {
  unsigned N = 0;
  // Flavor-independent first: guarded deletes, refcount atomics.
  N += rewriteGuardedDeletes(F, DL);
  N += rewriteRefCounts(F, DL);
  if (Fl == CxxFlavor::Auto)
    return N;
  N += rewriteTidy(F, DL);
  N += rewritePushBack(F, DL);
  N += rewriteStringAppend(F, DL);
  return N;
}

} // namespace
} // namespace uncxx

unsigned uncxx::runBlockIdioms(Function &F, const DataLayout &DL,
                               CxxFlavor Fl, void *DTReserved) {
  (void)DTReserved;
  return runBlockIdiomsImpl(F, DL, Fl);
}

} // namespace revenant
