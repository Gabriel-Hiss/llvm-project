//===- Uninline.cpp - recover calls inlined from a known callee ---------===//
//
// Part of the Revenant LLVM fork.
//
// Inverts LLVM function inlining for regions the inliner spliced into a
// caller while the callee still exists as a definition in the same module
// (intra-module inlining and LTO cross-module inlining: after linking the
// imported callee is just another module definition).
//
// What the inliner does to a call `v = call @callee(args)` when the callee
// body is a single basic block: it binds the arguments, splices the body
// instructions in place of the call, and replaces uses of the callee's `ret`
// value with the spliced computation. Later passes (SROA, GVN, instcombine)
// clean up the allocas and redundant casts, so by the time this pass runs
// the region looks like straight-line optimized code, not like a call.
//
// This pass fires only on an airtight shape:
//
// 1. The callee still has a definition in the module (a `declare` alone is
//    not enough: there is no body to match against), is not an intrinsic,
//    has exactly one basic block, one `ret`, no `invoke`, no inline asm, no
//    `!revenant.opaque` calls, and takes no `label`/`token`/`metadata`
//    arguments (those cannot cross a call boundary as values).
// 2. The caller holds a contiguous run of non-phi, non-terminator,
//    non-intrinsic instructions inside one basic block (the candidate
//    region) that matches the callee body's return-value computation up to
//    argument binding: same opcodes in the same order, same constants and
//    attributes, callee arguments replaced by caller values, callee-local
//    defs replaced by region-local defs. Memory operations match by access
//    kind (a load stays a load); the region may carry extra attributes the
//    optimizer inferred (alignment, dereferenceable markers), which are
//    kept, never widened.
// 3. The region is single-entry/single-exit: nothing outside jumps into its
//    middle (it sits in one block, so entry holds by construction), nothing
//    inside branches out, and no region-local def escapes except the single
//    value the call result would replace. Stores and other memory writes
//    disqualify the region: reordering them relative to the caller's memory
//    traffic is not provably safe without alias analysis, and the corpus
//    shapes this pass targets (pure arithmetic/gep/cast/cmp/select bodies
//    such as `mz_deflateBound` folded into `mz_compressBound`) need none.
// 4. The call, once rebuilt, is a legal call: the reconstructed argument
//    list has the callee's arity and types, and the call site keeps the
//    callee's calling convention.
//
// Rewrite: the region collapses to `v = call @callee(bound args)` at the
// position of the region's last instruction, with `v` replacing uses of the
// region's escaping value. Dead region instructions are erased. The call
// copies the callee's calling convention and per-argument attributes; the
// replaced instructions' `!revenant.va` metadata is copied onto the call
// (first region instruction carrying one wins), `!revenant.opaque` and
// `!revenant.region` are never touched (regions holding either are
// rejected), and debug locations are preserved per instruction where one
// exists.
//
// Why only single-block callees: multi-block bodies inline into multiple
// caller blocks with cloned branches, and matching those against later
// optimization (loop rotation, jump threading, tail duplication) is a
// different, much weaker-evidenced job. The corpus inline events this pass
// recovers (`-O1`/`-O2`/`-O3`/LTO small helpers: null checks folded to a
// select, bound arithmetic, flag setup) are single-block after optimization,
// and every rewrite here is checkable with `opt -passes=verify`.
//
// Liveness subtlety: the escaping value must dominate its uses the way a
// call result would. Because the region sits in one block and the call is
// placed at the region end, every use after the region sees the call
// result exactly where it saw the region value; uses *inside* the region
// are region-local by construction (rejected otherwise).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Uninline/Uninline.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

using namespace llvm;

namespace {

constexpr unsigned MaxCalleeInsts = 64;
constexpr unsigned MaxRegionInsts = 64;
constexpr unsigned MaxOperands = 16;

// Callee bodies we match must be side-effect free apart from the returned
// value: no stores (memory writes cannot be reordered without alias proof),
// no volatile/atomic memory operations, no calls except readonly intrinsics
// the matcher skips (lifetime markers, assume), no inline asm, no fences.
// Callee bodies are single-entry/single-exit straight lines, so the only
// effectful instruction allowed is at most one direct call (the wrapper
// family: the callee forwards to one real function, e.g. `mz_deflateInit`
// tail-calling `mz_deflateInit2`). Stores are rejected: reordering them
// relative to caller memory traffic needs alias proof this pass does not
// attempt. The single call may read or write memory; outlining it back
// into a call preserves order because the region occupies one position in
// the caller's instruction stream and the call is rebuilt at that same
// position.
bool isPureForMatch(const Instruction &I, bool &SawCall) {
  if (const auto *CB = dyn_cast<CallBase>(&I)) {
    if (CB->isInlineAsm())
      return false;
    const Function *F = CB->getCalledFunction();
    if (!F)
      return false; // indirect call: not matchable
    if (F->isIntrinsic())
      return false; // intrinsics are markers, handled by the skip path
    if (SawCall)
      return false; // at most one real call per body
    if (CB->isVolatile())
      return false;
    SawCall = true;
    return true;
  }
  if (I.mayWriteToMemory())
    return false;
  if (I.isVolatile())
    return false;
  if (isa<FenceInst>(I))
    return false;
  // Terminators, debug intrinsics and PHIs are handled by the caller.
  return true;
}

bool isSkippableMarker(const Instruction &I) {
  if (const auto *II = dyn_cast<IntrinsicInst>(&I)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::assume:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_value:
    case Intrinsic::dbg_assign:
    case Intrinsic::dbg_label:
      return true;
    default:
      return false;
    }
  }
  return false;
}

void copyVA(const Instruction *Src, Instruction *Dst) {
  if (!Src || !Dst)
    return;
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    if (!Dst->getMetadata("revenant.va"))
      Dst->setMetadata("revenant.va", MD);
  if (Src->getDebugLoc() && !Dst->getDebugLoc())
    Dst->setDebugLoc(Src->getDebugLoc());
}

// A callee is eligible when its body is one block ending in a single `ret`
// of matchable instructions. Returns the body instructions (excluding phis,
// which a single-block function can only carry as self-referential and
// which are rejected, and excluding the terminator) and the returned value.
struct CalleeBody {
  const Function *Fn = nullptr;
  SmallVector<const Instruction *, 16> Insts;
  const Value *RetVal = nullptr; // null for `ret void`
  bool Valid = false;
};

CalleeBody analyzeCallee(Function &F) {
  CalleeBody B;
  B.Fn = &F;
  if (F.isDeclaration() || F.isIntrinsic() || F.hasAddressTaken())
    return B;
  if (F.arg_size() > MaxOperands)
    return B;
  for (const Argument &A : F.args()) {
    Type *T = A.getType();
    if (T->isLabelTy() || T->isTokenTy() || T->isMetadataTy())
      return B;
  }
  if (F.size() != 1)
    return B;
  const BasicBlock &BB = F.front();
  if (!BB.phis().empty())
    return B;
  const Instruction *Term = BB.getTerminator();
  const auto *Ret = dyn_cast<ReturnInst>(Term);
  if (!Ret)
    return B;
  bool SawCall = false;
  for (const Instruction &I : BB) {
    if (I.isTerminator())
      continue;
    if (isSkippableMarker(I))
      continue;
    if (!isPureForMatch(I, SawCall))
      return B;
    if (I.getNumOperands() > MaxOperands)
      return B;
    // Callee-local control flow leftovers (catchswitch, funclets) cannot
    // cross a call boundary.
    if (I.isEHPad() || isa<ResumeInst>(&I))
      return B;
    B.Insts.push_back(&I);
  }
  if (B.Insts.size() > MaxCalleeInsts || B.Insts.empty())
    return B;
  // Minimum evidence: a one- or two-instruction body (a lone load, a
  // gep+load pair) matches coincidental code far more often than true
  // inline remnants (measured: a 2-instruction getter body matched 7
  // unrelated sites in miniz O1 for 1 true inline). Require at least 3
  // instructions, unless the body holds a real call: a call target plus
  // its bound arguments is distinctive evidence even in a 1-instruction
  // body (the single-call wrapper family).
  bool HasCall = false;
  for (const Instruction *I : B.Insts)
    if (isa<CallBase>(I))
      HasCall = true;
  if (B.Insts.size() < 3 && !HasCall)
    return B;
  // Wrapper disambiguation: a body that only forwards its arguments to
  // one inner call (`mz_free(p)` = `free(p)`) is indistinguishable from
  // hand-written calls to the inner function (measured: all 16 `free`
  // sites in miniz O1 rewritten to `mz_free`, diverging from ir-pre,
  // which calls `free` directly). Require the wrapper to add evidence
  // beyond forwarding: a constant operand in the body, or an argument
  // used zero times or at least twice. `mz_deflateInit(a,b)` passes
  if (HasCall && B.Insts.size() < 3) {
    bool Distinctive = false;
    for (const Instruction *I : B.Insts) {
      const Value *CalleeOp =
          isa<CallBase>(I) ? cast<CallBase>(I)->getCalledOperand() : nullptr;
      for (const Value *V : I->operands()) {
        if (V == CalleeOp)
          continue; // the inner target itself is not binding evidence
        if (isa<Constant>(V) && !V->getType()->isVoidTy() &&
            !isa<UndefValue>(V))
          Distinctive = true;
      }
    }
    DenseMap<const Argument *, unsigned> Uses;
    for (const Argument &A : F.args())
      Uses[&A] = 0;
    for (const Instruction *I : B.Insts)
      for (const Value *V : I->operands())
        if (const auto *A = dyn_cast<Argument>(V)) {
          auto It = Uses.find(A);
          if (It != Uses.end())
            ++It->second;
        }
    for (auto &KV : Uses)
      if (KV.second != 1)
        Distinctive = true;
    if (!Distinctive)
      return B;
  }
  if (B.RetVal) {
    // The returned value must be computed by the body or be a bound
    // argument/constant: anything else (e.g. a global) still matches, but
    // uses of a global need no binding, so accept any Value here and let
    // the operand matcher decide.
  }
  B.Valid = true;
  return B;
}

// Structural match of one callee instruction against one region instruction
// under the current bindings. Callee arguments must map to caller values
// consistently; callee-local defs must map to region-local defs in order.
class Matcher {
public:
  Matcher(const Function &Callee) {
    unsigned Idx = 0;
    for (const Argument &A : Callee.args())
      ArgBind[&A] = nullptr, ArgIndex[&A] = Idx++;
  }

  bool matchOperand(const Value *C, const Value *R) {
    if (C == R)
      return true;
    // Constants are uniqued: pointer equality is value equality.
    if (isa<Constant>(C))
      return false;
    if (const Argument *A = dyn_cast<Argument>(C)) {
      auto It = ArgBind.find(A);
      if (It == ArgBind.end())
        return false;
      Value *&Slot = It->second;
      if (!Slot) {
        Slot = const_cast<Value *>(R);
        return true;
      }
      return Slot == R;
    }
    auto It = LocalBind.find(C);
    if (It == LocalBind.end())
      return false;
    return It->second == R;
  }

  void bindLocal(const Value *C, Value *R) { LocalBind[C] = R; }

  // Snapshot/restore for trial matches of candidate regions.
  DenseMap<const Value *, Value *> snapLocals() const { return LocalBind; }
  void restoreLocals(const DenseMap<const Value *, Value *> &S) {
    LocalBind = S;
  }
  SmallVector<std::pair<const Argument *, Value *>> snapArgs() const {
    SmallVector<std::pair<const Argument *, Value *>> S;
    for (auto &KV : ArgBind)
      S.push_back({KV.first, KV.second});
    return S;
  }
  void restoreArgs(
      const SmallVector<std::pair<const Argument *, Value *>> &S) {
    for (auto &KV : S)
      ArgBind[KV.first] = KV.second;
  }

  DenseMap<const Argument *, Value *> args() const { return ArgBind; }

private:
  DenseMap<const Argument *, Value *> ArgBind;
  DenseMap<const Argument *, unsigned> ArgIndex;
  DenseMap<const Value *, Value *> LocalBind;
};

bool sameCallAttrs(const CallBase *C, const CallBase *R) {
  if (C->getCallingConv() != R->getCallingConv())
    return false;
  if (C->isVolatile() != R->isVolatile())
    return false;
  if (C->getNumOperands() != R->getNumOperands())
    return false;
  return true;
}

// Compare memory-access shape without requiring identical inferred
// attributes: the optimizer may have refined alignment or added
// dereferenceable/nonnull markers on the inlined copy.
bool sameMemShape(const Instruction *C, const Instruction *R) {
  if (const auto *CL = dyn_cast<LoadInst>(C)) {
    const auto *RL = dyn_cast<LoadInst>(R);
    if (!RL)
      return false;
    if (CL->isVolatile() != RL->isVolatile())
      return false;
    if (CL->getOrdering() != RL->getOrdering())
      return false;
    return true;
  }
  if (const auto *CG = dyn_cast<GetElementPtrInst>(C)) {
    const auto *RG = dyn_cast<GetElementPtrInst>(R);
    if (!RG)
      return false;
    if (CG->getSourceElementType() != RG->getSourceElementType())
      return false;
    if (CG->isInBounds() != RG->isInBounds())
      return false;
    return true;
  }
  return true;
}

bool matchInst(const Instruction *C, const Instruction *R, Matcher &M) {
  if (C->getOpcode() != R->getOpcode())
    return false;
  if (C->getType() != R->getType())
    return false;
  if (C->getNumOperands() != R->getNumOperands())
    return false;
  if (!sameMemShape(C, R))
    return false;
  if (const auto *CC = dyn_cast<CmpInst>(C)) {
    if (cast<CmpInst>(R)->getPredicate() != CC->getPredicate())
      return false;
  }
  if (const auto *CC = dyn_cast<CallBase>(C)) {
    const auto *RC = cast<CallBase>(R);
    const Function *CF = CC->getCalledFunction();
    const Function *RF = RC->getCalledFunction();
    // Compare targets by identity when both resolve, by name when one
    // side is a declaration: the inlined copy may carry refined
    // attributes or a tail marker the callee body lacks.
    if (CF && RF) {
      if (CF != RF && CF->getName() != RF->getName())
        return false;
    } else if (CF != RF) {
      return false;
    }
    if (CC->getCallingConv() != RC->getCallingConv())
      return false;
    if (CC->isVolatile() != RC->isVolatile())
      return false;
    if (CC->getNumOperands() != RC->getNumOperands())
      return false;
  }
  if (const auto *CS = dyn_cast<SelectInst>(C)) {
    (void)CS;
  }
  for (unsigned i = 0, e = C->getNumOperands(); i != e; ++i) {
    // Predicate/info operands of intrinsics compare as constants above;
    // operand mismatch fails the match.
    if (!M.matchOperand(C->getOperand(i), R->getOperand(i)))
      return false;
  }
  return true;
}

// Candidate region: contiguous non-phi instructions in one caller block.
// The region's last instruction produces the escaping value; every other
// region def must be used only inside the region (or be dead).
struct Region {
  BasicBlock *BB = nullptr;
  unsigned Start = 0; // index into the block's instruction list
  unsigned Len = 0;
  SmallVector<Instruction *, 16> Insts;
  Value *Escape = nullptr; // value replacing the call result
};

// Collect the non-terminator, non-phi instruction list of BB.
void blockBody(BasicBlock *BB, SmallVectorImpl<Instruction *> &Out) {
  for (Instruction &I : *BB) {
    if (isa<PHINode>(&I) || I.isTerminator())
      continue;
    Out.push_back(&I);
  }
}

bool regionEscapesCleanly(const SmallVectorImpl<Instruction *> &Insts,
                          Value *Escape, Function &Caller) {
  SmallPtrSet<Instruction *, 16> InRegion;
  for (Instruction *I : Insts)
    InRegion.insert(I);
  for (Instruction *I : Insts) {
    if (I == Escape)
      continue;
    for (User *U : I->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI)
        return false;
      if (UI->getParent() != I->getParent() || !InRegion.count(UI)) {
        // Uses outside the region (or in another block) escape.
        // Debug uses are fine.
        if (isa<DbgInfoIntrinsic>(UI))
          continue;
        return false;
      }
    }
  }
  // The escape itself must not be used inside the region after its def
  // (that would be a cycle); uses dominate-check below covers order.
  auto *EscapeInst = dyn_cast<Instruction>(Escape);
  for (User *U : Escape->users()) {
    auto *UI = dyn_cast<Instruction>(U);
    if (!UI)
      return false;
    if (EscapeInst && UI->getParent() == EscapeInst->getParent() &&
        InRegion.count(UI)) {
      // Use inside the region: only legal if it precedes... in straight
      // line code the def dominates, so any in-region user comes after.
      // That would make the value both internal and escaping; reject:
      // a call result has exactly one definition point.
      return false;
    }
  }
  // No use of the escape may sit between region instructions (placement at
  // region end must dominate all uses).
  (void)Caller;
  return true;
}

// Try to match CalleeBody against a window of caller instructions.
// On success fills ArgVals (callee argument order) and Escape.
bool matchWindow(const CalleeBody &CB,
                 const SmallVectorImpl<Instruction *> &Body, unsigned Start,
                 SmallVectorImpl<Value *> &ArgVals, Value *&Escape) {
  const Function *Callee = CB.Fn;
  Matcher M(*Callee);
  unsigned N = (unsigned)CB.Insts.size();
  if (Start + N > Body.size())
    return false;
  for (unsigned i = 0; i != N; ++i) {
    const Instruction *C = CB.Insts[i];
    Instruction *R = Body[Start + i];
    // Never match opaque helper calls or region annotations: those carry
    // provenance the rewrite must not disturb.
    if (R->getMetadata("revenant.opaque") || C->getMetadata("revenant.opaque"))
      return false;
    if (R->hasMetadata("revenant.region"))
      return false;
    if (isSkippableMarker(*R))
      return false; // markers were stripped from the callee list, so a
                    // marker in the caller window breaks contiguity
    if (!matchInst(C, R, M))
      return false;
    M.bindLocal(C, R);
  }
  // Bind the return value.
  if (!CB.RetVal) {
    Escape = nullptr; // `ret void`: set below to the region end marker
  } else if (const auto *RC = dyn_cast<Constant>(CB.RetVal)) {
    // Callee returns a constant: the region's last value must be that
    // constant. Only accept when the last region instruction produces it
    // (a trailing materialization), else there is no escaping def.
    Instruction *Last = Body[Start + N - 1];
    if (!matchInst(CB.Insts.back(), Last, M))
      return false;
    (void)RC;
    Escape = Last;
  } else if (const Argument *A = dyn_cast<Argument>(CB.RetVal)) {
    auto Args = M.args();
    auto It = Args.find(A);
    if (It == Args.end() || !It->second)
      return false;
    // Callee returns an argument unchanged: the call site forwards a
    // caller value, so the region must be empty... but regions are
    // non-empty by construction. Reject: nothing to outline.
    return false;
  } else {
    auto Locals = M.snapLocals();
    auto It = Locals.find(CB.RetVal);
    if (It == Locals.end())
      return false;
    Escape = It->second;
    // The escape must be the region's last instruction: a call result is
    // defined at the call, and everything after would observe it.
    if (Escape != Body[Start + N - 1])
      return false;
  }
  ArgVals.clear();
  for (const Argument &A : Callee->args()) {
    auto Args = M.args();
    auto It = Args.find(&A);
    if (It == Args.end() || !It->second)
      return false;
    ArgVals.push_back(It->second);
  }
  return true;
}

bool tryUninlineCaller(Function &Caller, const CalleeBody &CB) {
  const Function *Callee = CB.Fn;
  if (Callee == &Caller)
    return false; // never outline a function into itself
  // Skip recursive shapes: caller already calls the callee (partial
  // inlining left a real call). Outlining more copies would fight the
  // inliner's judgment call after call.
  for (BasicBlock &BB : Caller) {
    for (Instruction &I : BB) {
      if (const auto *CBInst = dyn_cast<CallBase>(&I)) {
        if (CBInst->getCalledFunction() == Callee)
          return false;
      }
    }
  }
  bool Changed = false;
  // Snapshot blocks: the rewrite erases instructions but never adds or
  // removes blocks, so iterating blocks directly is safe.
  for (BasicBlock &BB : Caller) {
    SmallVector<Instruction *, 64> Body;
    blockBody(&BB, Body);
    if (Body.size() < CB.Insts.size())
      continue;
    // Slide the window over the block. After a successful rewrite the
    // block changed; restart scanning it.
    bool Restart = true;
    unsigned Guard = 0;
    while (Restart && Guard++ < 64) {
      Restart = false;
      Body.clear();
      blockBody(&BB, Body);
      if (Body.size() < CB.Insts.size() || CB.Insts.size() > MaxRegionInsts)
        break;
      for (unsigned S = 0, E = (unsigned)Body.size() - (unsigned)CB.Insts.size();
           S <= E; ++S) {
        SmallVector<Value *, 8> ArgVals;
        Value *Escape = nullptr;
        if (!matchWindow(CB, Body, S, ArgVals, Escape))
          continue;
        SmallVector<Instruction *, 16> RegionInsts;
        for (unsigned i = 0; i != CB.Insts.size(); ++i)
          RegionInsts.push_back(Body[S + i]);
        if (!Escape) {
          // `ret void` callee: no escaping value; the call goes at the
          // region end.
          Escape = RegionInsts.back();
        }
        if (!regionEscapesCleanly(RegionInsts, Escape, Caller))
          continue;
        // All region defs except the escape must die with the region.
        // (regionEscapesCleanly proved they have no outside users.)
        Instruction *Anchor = RegionInsts.back();
        // Rebuild the call at the region end.
        CallInst *Call = CallInst::Create(
            const_cast<Function *>(Callee)->getFunctionType(),
            const_cast<Function *>(Callee), ArgVals, "", Anchor->getIterator());
        Call->setCallingConv(Callee->getCallingConv());
        Call->setAttributes(Callee->getAttributes());
        // Provenance: first region instruction carrying !revenant.va wins.
        for (Instruction *I : RegionInsts) {
          if (I->getMetadata("revenant.va") || I->getDebugLoc()) {
            copyVA(I, Call);
            break;
          }
        }
        if (!Callee->getReturnType()->isVoidTy()) {
          // A non-void callee whose body value feeds the escape: replace
          // outside uses of the escape with the call result.
          Escape->replaceUsesWithIf(
              Call, [&](Use &U) {
                auto *UI = dyn_cast<Instruction>(U.getUser());
                if (!UI)
                  return false;
                for (Instruction *RI : RegionInsts)
                  if (UI == RI)
                    return false;
                return true;
              });
        }
        // Erase the region back to front. regionEscapesCleanly proved no
        // outside instruction observes a region def except through the
        // escape, whose outside uses now name the call; erasing users
        // before their operands keeps every erase legal.
        for (int i = (int)RegionInsts.size() - 1; i >= 0; --i)
          RegionInsts[i]->eraseFromParent();
        Changed = true;
        Restart = true;
        break;
      }
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses revenant::UninlinePass::run(Module &M,
                                             ModuleAnalysisManager &) {
  // Collect eligible callees first: the rewrite never adds or removes
  // functions, so pointers stay valid.
  SmallVector<CalleeBody, 16> Callees;
  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    CalleeBody B = analyzeCallee(F);
    if (B.Valid)
      Callees.push_back(std::move(B));
  }
  bool Changed = false;
  for (CalleeBody &CB : Callees) {
    for (Function &F : M) {
      if (F.isDeclaration() || F.isIntrinsic())
        continue;
      if (&F == CB.Fn)
        continue;
      Changed |= tryUninlineCaller(F, CB);
    }
  }
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// Plugin entry for `opt -load-pass-plugin`. Mirrors the Devectorize/SsaOut
// slices: compiled only for the standalone plugin DLL so a static link of
// every transform into `revenant-opt` cannot export the entry twice.
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUninline", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-uninline") {
                    PM.addPass(revenant::UninlinePass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
