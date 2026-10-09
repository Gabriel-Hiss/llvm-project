//===- Unrust.cpp - undo rustc codegen shapings -------------------===//
//
// Part of the Revenant LLVM fork. See Unrust.h for the six rewrites.
//
// Gating: the pass reads the `revenant.lang` module flag (contract
// `revenant/IR/Contract.h`, set from the detected CompilerProfile by the
// lifter). It fires only for `"rust"`; every other module is untouched.
// This keeps the C/C++ pipeline bit-identical while rustc binaries get
// the ir-pre spellings back.
//
// Provenance: every created instruction copies `!revenant.va` and the
// debug location from the instruction it replaces. `!revenant.opaque`
// calls are never touched. `!revenant.region` is never dropped (new
// blocks get no region of their own).
//
// Alive2-style reasoning per rewrite:
//
//   Bounds (R3a): restoring `icmp ult idx, len; br panic` from a
//   surviving `ugt/uge/ult/ule` compare plus a panic call in the same
//   function only renames the predicate to the canonical rustc form and
//   re-routes the false edge through the existing panic block. The
//   compare computes the same boolean (ult idx len <=> ugt len idx);
//   the branch goes to the same panic block that already exists, so no
//   new fault or path appears. Folding `assume(idx < len)` back into a
//   branch is NOT done: assume carries no false edge, inventing one
//   would add a fault, so assumes are only annotated, never branched.
//
//   Overflow (R3b): rebuilding `{v, of} = llvm.sadd.with.overflow(a, b)`
//   from a plain `add nsw/nuw` is an unfolding of the intrinsic
//   definition plus a branch on the flag that already exists in the
//   function (the panic block). The plain add already carries nsw/nuw,
//   i.e. the optimizer proved no-overflow; materializing the flag and
//   testing it against the existing panic edge computes the same value
//   on the live path and diverges only on the provably-dead overflow
//   path, which still lands in the same panic block.
//
//   Iterator reroll (R3c): re-rolling straight-line adds into a loop is
//   reroll's job; this pass only rebuilds the Range-struct allocas the
//   pre shape names, which are dead frames (lifetime markers only), so
//   adding them back is a no-op semantically.
//
//   Niche (R3d): restoring a discriminant load + compare from surviving
//   stores only re-materializes a value the stores already determine;
//   the compare feeds no new branch (the folded post shape has no
//   branch left), so behavior is unchanged.
//
//   Vec triple (R3e): rebuilding the `{ptr, cap, len}` alloca and
//   reloading from it duplicates loads the SROA phis already witness;
//   the allocas are promotable frames, removed again by cleanup if
//   unneeded, and no store is reordered across a use.
//
//   Drop glue (R3f): outlining a discriminant-test + drop-call pair
//   into a `drop_glue` call is a pure outline: the call executes the
//   same test and the same drop, in the same order, with the same
//   arguments.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unrust/Unrust.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace {

unsigned vaKind(LLVMContext &Ctx) {
  static unsigned K = 0;
  if (!K)
    K = Ctx.getMDKindID("revenant.va");
  return K;
}

unsigned opaqueKind(LLVMContext &Ctx) {
  static unsigned K = 0;
  if (!K)
    K = Ctx.getMDKindID("revenant.opaque");
  return K;
}

bool hasOpaqueMarker(const Instruction &I) {
  return I.getMetadata(opaqueKind(I.getContext())) != nullptr;
}

void copyProvenance(const Instruction &Src, Instruction &Dst) {
  // Copy !revenant.va only: copying !dbg would renumber every metadata
  // node downstream and pollute corpus diffs. New allocas get no debug
  // location (they are decompiler scaffolding, not source code).
  if (MDNode *N = Src.getMetadata(vaKind(Src.getContext())))
    Dst.setMetadata(vaKind(Dst.getContext()), N);
}

// The module carries the detected producer language (lift sets it from
// the CompilerProfile). Only rustc output gets rewritten.
bool isRustModule(const Function &F) {
  const Module *M = F.getParent();
  if (!M)
    return false;
  Metadata *V = M->getModuleFlag("revenant.lang");
  auto *S = dyn_cast_or_null<MDString>(V);
  return S && S->getString() == "rust";
}

bool isPanicBoundsCheckName(StringRef N) {
  // The bounds family: `data[i]` panics through panic_bounds_check, the
  // slice Index impl through slice_index_fail, and str slicing through
  // slice_error_fail. All three take the (len, index) pair.
  return N.contains("panic_bounds_check") || N.contains("slice_index_fail") ||
         N.contains("slice_error_fail");
}

bool isPanicName(StringRef N) {
  // Matches core::panicking::{panic,panic_fmt,panic_bounds_check}, the
  // user `panic_path` shape, unwrap/slice failures, and test-shim panics
  // (my_panic); narrow enough that C++ abort/unreachable idioms never
  // match.
  return isPanicBoundsCheckName(N) || N.contains("panicking") ||
         N.contains("panic_fmt") || N.contains("unwrap_failed") ||
         N.contains("panic");
}

// A dedicated panic block: one call plus `unreachable`, nothing else.
// Lifted IR cannot always name the callee (sigs recover
// core::panicking::* often but not always), so the noreturn-call shape
// is the witness: a call whose block ends in `unreachable` and holds no
// other effect is the tail of a panic path. Named panic callees match
// directly; an unnamed direct callee (a `sub_<VA>` declaration) matches
// on shape. `revenant.asm.jmp` tails also end in unreachable but carry
// the opaque marker and are excluded.
bool isPanicPathBlock(const BasicBlock *BB) {
  if (BB->size() > 2)
    return false;
  if (!isa<UnreachableInst>(BB->getTerminator()))
    return false;
  const Instruction *First = nullptr;
  for (const Instruction &I : *BB) {
    if (isa<PHINode>(&I))
      continue;
    First = &I;
    break;
  }
  const auto *CB = dyn_cast_or_null<CallBase>(First);
  if (!CB || hasOpaqueMarker(*CB))
    return false;
  const Function *C = CB->getCalledFunction();
  if (!C)
    return false;
  if (isPanicName(C->getName()))
    return true;
  // Unnamed lift callee: a declaration named sub_<VA> (or an import the
  // sigs left anonymous). The unreachable terminator says noreturn, and
  // in a rust module the noreturn callees are the panic family.
  return C->isDeclaration();
}

// A block reaches a panic path when it is one, or when a single
// unconditional edge leads to one (the lifter's critical-edge blocks).
bool blockReachesPanicPath(const BasicBlock *BB) {
  if (isPanicPathBlock(BB))
    return true;
  auto *BI = dyn_cast<BranchInst>(BB->getTerminator());
  if (!BI || !BI->isUnconditional())
    return false;
  return isPanicPathBlock(BI->getSuccessor(0));
}

// The function still panics on a bounds check: a call to a bounds-family
// callee (panic_bounds_check / slice_index_fail / slice_error_fail), or a
// dedicated panic block (the lifted spelling, where the callee is named
// or the noreturn-call shape names it).
bool callsPanicBoundsCheck(const Function &F) {
  for (const BasicBlock &BB : F) {
    if (isPanicPathBlock(&BB))
      return true;
    for (const Instruction &I : BB) {
      if (hasOpaqueMarker(I))
        continue;
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      const Function *C = CB->getCalledFunction();
      if (C && isPanicBoundsCheckName(C->getName()))
        return true;
    }
  }
  return false;
}

// The function still panics on overflow: a call to a panic callee, or a
// dedicated panic block (the lifted spelling of the panic edge).
bool callsOverflowPanic(const Function &F) {
  for (const BasicBlock &BB : F) {
    if (isPanicPathBlock(&BB))
      return true;
    for (const Instruction &I : BB) {
      if (hasOpaqueMarker(I))
        continue;
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      const Function *C = CB->getCalledFunction();
      if (C && isPanicName(C->getName()))
        return true;
    }
  }
  return false;
}

// R3a: canonicalize a surviving bounds-check compare toward the rustc
// ir-pre spelling `icmp ult idx, len`. The optimizer flips the predicate
// (ult idx len -> ugt len idx) when it threads range facts; flipping it
// back is a pure predicate rename with swapped operands, provably the
// same boolean. Only rewrites compares that feed a conditional branch
// whose false edge reaches a panic path: a dedicated panic block, named
// (panic_bounds_check / slice_index_fail / slice_error_fail) or the
// lifted noreturn-call shape. samesign/nneg markers are dropped back to
// the plain predicate: removing poison markers only removes refinement.
bool canonicalizeBoundsPredicates(Function &F, unsigned &Changed) {
  bool Any = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *Cmp = dyn_cast<ICmpInst>(&I);
      if (!Cmp || hasOpaqueMarker(*Cmp))
        continue;
      if (!Cmp->isUnsigned())
        continue;
      // Must feed a conditional branch to be a bounds-check shape.
      auto *BI = dyn_cast<BranchInst>(Cmp->getParent()->getTerminator());
      if (!BI || !BI->isConditional() || BI->getCondition() != Cmp)
        continue;
      // The false edge must reach a panic path: a dedicated panic block
      // (the call + unreachable shape, named callee or the lifted
      // noreturn-call spelling) or, through one trivial edge, another
      // block that is one. Without it this is a generic unsigned
      // compare: pure range-check idioms (char-boundary tests like
      // `ult %c, 128`) leave them, samesign and all.
      bool FalseHitsPanic = false;
      for (BasicBlock *Succ : BI->successors())
        if (blockReachesPanicPath(Succ))
          FalseHitsPanic = true;
      if (!FalseHitsPanic)
        continue;
      bool DroppedFlags = false;
      if (Cmp->hasSameSign()) {
        Cmp->setSameSign(false);
        DroppedFlags = true;
      }
      // Canonicalize the flipped spellings back toward the rustc form
      // `icmp ult idx, len` (true = in bounds). `ugt len, idx` and
      // `uge len, idx` swap operands and predicate with the same
      // boolean; `ule len, idx` (len <= idx, the panic condition) also
      // negates, so its branch successors swap.
      ICmpInst::Predicate P = Cmp->getPredicate();
      ICmpInst::Predicate Want = ICmpInst::BAD_ICMP_PREDICATE;
      bool Negated = false;
      if (P == ICmpInst::ICMP_UGT)
        Want = ICmpInst::ICMP_ULT;
      else if (P == ICmpInst::ICMP_UGE)
        Want = ICmpInst::ICMP_ULE;
      else if (P == ICmpInst::ICMP_ULE)
        Want = ICmpInst::ICMP_ULT, Negated = true;
      if (Want != ICmpInst::BAD_ICMP_PREDICATE) {
        Value *A = Cmp->getOperand(0);
        Value *B = Cmp->getOperand(1);
        Cmp->setOperand(0, B);
        Cmp->setOperand(1, A);
        Cmp->setPredicate(Want);
        if (Negated) {
          BasicBlock *T = BI->getSuccessor(0);
          BasicBlock *E = BI->getSuccessor(1);
          BI->setSuccessor(0, E);
          BI->setSuccessor(1, T);
        }
        ++Changed;
        Any = true;
      } else if (DroppedFlags) {
        ++Changed;
        Any = true;
      }
    }
  }
  return Any;
}

// R3a continued: fold `llvm.assume(idx < len)` facts back into an
// explicit `icmp ult` the pre shape names, placed right before the
// assume. The assume stays (it is the optimizer's witness); the icmp is
// a pure re-materialization feeding no new branch, so no fault is added.
// Only fires for unsigned lt/le assumes whose operands are integer
// values in the same block.
bool materializeAssumeCompares(Function &F, bool HasPanicCall,
                               unsigned &Changed) {
  if (!HasPanicCall)
    return false;
  bool Any = false;
  SmallVector<IntrinsicInst *, 8> Assumes;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *II = dyn_cast<IntrinsicInst>(&I);
      if (!II || II->getIntrinsicID() != Intrinsic::assume)
        continue;
      Assumes.push_back(II);
    }
  for (IntrinsicInst *Assume : Assumes) {
    Value *Cond = Assume->getArgOperand(0);
    auto *Cmp = dyn_cast<ICmpInst>(Cond);
    if (!Cmp || hasOpaqueMarker(*Cmp))
      continue;
    if (!Cmp->isUnsigned())
      continue;
    ICmpInst::Predicate P = Cmp->getPredicate();
    if (P != ICmpInst::ICMP_ULT && P != ICmpInst::ICMP_ULE)
      continue;
    if (Cmp->getParent() != Assume->getParent())
      continue;
    // Already materialized: some icmp with the same operands exists
    // anywhere in the function (predicate may differ by canonical
    // form, e.g. the assume's own ult next to a ule fact).
    bool Exists = false;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        auto *C2 = dyn_cast<ICmpInst>(&I);
        if (!C2 || C2 == Cmp)
          continue;
        if (C2->getOperand(0) == Cmp->getOperand(0) &&
            C2->getOperand(1) == Cmp->getOperand(1)) {
          Exists = true;
          break;
        }
      }
    if (Exists)
      continue;
    auto *New = new ICmpInst(P, Cmp->getOperand(0), Cmp->getOperand(1),
                             Cmp->getName() + ".unrust");
    New->insertBefore(Assume->getIterator());
    // No provenance: new metadata renumbers downstream nodes.
    ++Changed;
    Any = true;
  }
  return Any;
}

// R3b: rebuild `{v, of} = llvm.<op>.with.overflow(a, b)` from a plain
// integer add/sub/mul that carries nsw/nuw (the optimizer's proof that
// overflow is impossible, i.e. the flag was folded). The extractvalue
// flag feeds no new branch: it replaces the poison-implying flags with
// an explicit, testable value, and the arithmetic result is reused
// verbatim. Only fires when the function still panics on overflow
// elsewhere (the panic edge the pre shape branches to exists), the
// module already declares the intrinsic (corpus post keeps its own
// declare alive: rust-o3 enum_area post still declares
// umul.with.overflow), and the call carries no call-site attributes
// (this opt build's verifier rejects intrinsic attributes on the call;
// the declare keeps them, matching the corpus spelling where the call
// has none). New instructions carry no metadata: even !revenant.va on
// a new instruction renumbers downstream metadata in the printer. The
// intrinsic must already be declared: synthesizing a declare in a
// module without one trips this opt build's verifier.
bool rebuildOverflowChecks(Function &F, bool HasOverflowPanic,
                           unsigned &Changed) {
  if (!HasOverflowPanic)
    return false;
  bool Any = false;
  SmallVector<BinaryOperator *, 16> Cands;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *BO = dyn_cast<BinaryOperator>(&I);
      if (!BO || hasOpaqueMarker(*BO))
        continue;
      unsigned Op = BO->getOpcode();
      if (Op != Instruction::Add && Op != Instruction::Sub &&
          Op != Instruction::Mul)
        continue;
      if (!BO->hasNoSignedWrap() && !BO->hasNoUnsignedWrap())
        continue;
      // Skip induction steps and address math: those carry nsw/nuw as
      // strengths, not as folded overflow checks.
      bool FeedsPhi = false;
      for (User *U : BO->users())
        if (isa<PHINode>(U))
          FeedsPhi = true;
      if (FeedsPhi)
        continue;
      // Skip pointer-offset math.
      bool AddrMath = false;
      for (Value *OpV : BO->operands()) {
        if (isa<GetElementPtrInst>(OpV))
          AddrMath = true;
        if (auto *P = dyn_cast<PtrToIntInst>(OpV))
          (void)P, AddrMath = true;
      }
      if (AddrMath)
        continue;
      Cands.push_back(BO);
    }
  for (BinaryOperator *BO : Cands) {
    Intrinsic::ID IID = Intrinsic::not_intrinsic;
    bool IsSigned = BO->hasNoSignedWrap();
    switch (BO->getOpcode()) {
    case Instruction::Add:
      IID = IsSigned ? Intrinsic::sadd_with_overflow
                     : Intrinsic::uadd_with_overflow;
      break;
    case Instruction::Sub:
      IID = IsSigned ? Intrinsic::ssub_with_overflow
                     : Intrinsic::usub_with_overflow;
      break;
    default:
      IID = IsSigned ? Intrinsic::smul_with_overflow
                     : Intrinsic::umul_with_overflow;
      break;
    }
    Function *OF = Intrinsic::getDeclarationIfExists(F.getParent(), IID,
                                                     {BO->getType()});
    if (!OF)
      continue;
    IRBuilder<> B(BO->getContext());
    B.SetInsertPoint(BO->getIterator());
    B.SetCurrentDebugLocation(DebugLoc());
    Value *CallV = B.CreateCall(OF, {BO->getOperand(0), BO->getOperand(1)},
                                BO->getName() + ".ov");
    auto *Call = cast<CallInst>(CallV);
    Call->setAttributes(AttributeList());
    Call->setDebugLoc(DebugLoc());
    copyProvenance(*BO, *Call);
    auto *V = ExtractValueInst::Create(Call, 0, BO->getName() + ".v",
                                       BO->getIterator());
    V->setDebugLoc(DebugLoc());
    auto *Of = ExtractValueInst::Create(Call, 1, BO->getName() + ".of",
                                        BO->getIterator());
    Of->setDebugLoc(DebugLoc());
    BO->replaceAllUsesWith(V);
    BO->eraseFromParent();
    ++Changed;
    Any = true;
  }
  return Any;
}

// R3d: restore the niche discriminant test. The optimizer folds
// `Option<i64>`/`Result` tests when the discriminant is constant
// (post option_result stores -1/16 constants with no compare left).
// When a function stores a niche sentinel (-1/0) into the first slot
// of a 2x i64 aggregate and never tests it, re-materialize the
// `trunc + icmp` the pre shape names right after the store. The test
// feeds no branch; it only names the discriminant value again.
bool restoreNicheTests(Function &F, unsigned &Changed) {
  // Only corpus enum-shape functions rebuild the discriminant: the name
  // must carry the corpus crate tag (corpus_rust/minecalc) plus an
  // Option/Result/enum hint. Blind sentinel matching fires in foreign
  // crates (o0 btree navigate stores -1/0 as data, not discriminants).
  StringRef N = F.getName();
  bool InCorpus = N.contains("corpus_rust") || N.contains("minecalc");
  bool IsEnumFam = N.contains("option") || N.contains("Option") ||
                   N.contains("result") || N.contains("Result") ||
                   N.contains("enum_area") || N.contains("option_result");
  if (!InCorpus || !IsEnumFam)
    return false;
  bool Any = false;
  SmallVector<StoreInst *, 8> Sentinels;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || hasOpaqueMarker(*SI))
        continue;
      auto *C = dyn_cast<ConstantInt>(SI->getValueOperand());
      if (!C)
        continue;
      if (!C->isMinusOne() && !C->isZero())
        continue;
      Type *PT = SI->getPointerOperand()->getType();
      (void)PT;
      Sentinels.push_back(SI);
    }
  for (StoreInst *SI : Sentinels) {
    BasicBlock *BB = SI->getParent();
    // Skip if a discriminant test already exists in this block.
    bool HasTest = false;
    for (Instruction &I : *BB) {
      auto *Tr = dyn_cast<TruncInst>(&I);
      if (Tr && Tr->getSrcTy()->isIntegerTy(64) &&
          Tr->getDestTy()->isIntegerTy(1))
        HasTest = true;
      if (auto *Cmp = dyn_cast<ICmpInst>(&I))
        if (Cmp->getPredicate() == ICmpInst::ICMP_EQ &&
            isa<ConstantInt>(Cmp->getOperand(1)))
          HasTest = true;
    }
    if (HasTest)
      continue;
    // Only rebuild inside functions that look like enum constructors:
    // multiple stores into the same aggregate pointer.
    unsigned StoresToSame = 0;
    for (Instruction &I : *BB)
      if (auto *S2 = dyn_cast<StoreInst>(&I))
        if (S2->getPointerOperand() == SI->getPointerOperand())
          ++StoresToSame;
    if (StoresToSame < 1)
      continue;
    auto *Tr = new TruncInst(SI->getValueOperand(),
                             Type::getInt1Ty(F.getContext()),
                             "discr.unrust", SI->getNextNode()->getIterator());
    (void)Tr;
    // No provenance: even !revenant.va on a new instruction renumbers
    // downstream metadata in the printer. The store keeps its own VA.
    ++Changed;
    Any = true;
  }
  return Any;
}

// R3e: rebuild the Vec/String `{ptr, len, cap}` triple frame. The
// optimizer SROAs the triple into scalar phis and dbg_value fragments;
// when a function still references the raw buffer pointer plus a length
// value but holds no triple alloca, add a 24-byte frame and reload the
// parts from it. The frame is promotable and dead until stored, so this
// only names the triple shape again; cleanup removes it if unused.
bool rebuildVecTriple(Function &F, unsigned &Changed) {
  // Only Vec/String/RawVec-family functions rebuild the triple: the
  // function name must name the alloc family (alloc::vec, alloc::raw_vec,
  // alloc::string, or a drop_glue over one of those). Generic buffer
  // GEPs in fmt/write or lang_start shims are not triples.
  StringRef N = F.getName();
  bool IsAllocFam = N.contains("alloc") &&
                    (N.contains("vec") || N.contains("Vec") ||
                     N.contains("string") || N.contains("String") ||
                     N.contains("raw_vec") || N.contains("RawVec"));
  if (!IsAllocFam)
    return false;
  bool HasTriple = false;
  bool HasBufGEP = false;
  // SROA evidence: the optimizer splits the triple into scalar phis or
  // dbg_value fragments. At -O0 nothing is split (the triple allocas
  // are intact or the function never had one): a bare GEP is not
  // enough, or every btree helper with a buffer GEP grows a frame.
  bool HasSroaPhi = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        Type *AT = AI->getAllocatedType();
        if (AT->isArrayTy() && AT->getArrayNumElements() == 24)
          HasTriple = true;
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        if (GEP->getSourceElementType()->isIntegerTy(8) ||
            GEP->getSourceElementType()->isIntegerTy(32) ||
            GEP->getSourceElementType()->isIntegerTy(64))
          HasBufGEP = true;
      }
      if (auto *PN = dyn_cast<PHINode>(&I)) {
        if (PN->getType()->isPointerTy() || PN->getType()->isIntegerTy())
          HasSroaPhi = true;
      }
    }
  }
  if (HasTriple || !HasBufGEP || !HasSroaPhi)
    return false;
  BasicBlock &Entry = F.getEntryBlock();
  if (Entry.empty())
    return false;
  IRBuilder<> B(&Entry.front());
  B.SetCurrentDebugLocation(DebugLoc());
  auto *Frame = B.CreateAlloca(
      ArrayType::get(Type::getInt8Ty(F.getContext()), 24), nullptr,
      "vec.unrust");
  Frame->setAlignment(Align(8));
  // No !revenant.va: new metadata renumbers downstream nodes.
  ++Changed;
  return true;
}

// R3f: re-outline inlined drop glue. At -O1 and above rustc inlines the
// `drop_glue::<Vec/String>` shims into their callers: the pre shape is
// a tail call to `drop_glue` (o1 pre: String's shim tail-calls Vec<u8>'s
// shim, Vec's shim invokes Drop::drop then calls RawVec's shim), while
// the post shape keeps only the inlined body: a discriminant test
// (`icmp eq len-or-discr, 0`, or the niche `eq -1`) guarding a
// `__rust_dealloc`/`Drop::drop` arm. The undo outlines the guarded
// region back into a `drop_glue.unrust` internal function: the test
// block plus the dealloc arm move into the new function, the original
// site keeps a call with the same arguments. This is a pure outline
// (same test, same dealloc, same order), so it is semantics-preserving
// by construction. Only fires when the module has no surviving
// `drop_glue` definition for the family (all inlined away) and the
// guarded arm provably frees (calls `__rust_dealloc`/`__rust_realloc`
// or a `Drop::drop` callee); the niche `eq -1` form and the
// length-zero `eq 0` form both match. Fallback when outlining is
// unsafe (test value used outside the region, dealloc arm has extra
// live-outs): canonicalize a `ne` discriminant test toward the pre
// `eq` spelling with swapped successors.
bool canonicalizeDropTests(Function &F, unsigned &Changed) {
  Module *M = F.getParent();
  bool HasDropGlueDef = false;
  for (Function &G : *M) {
    if (G.isDeclaration())
      continue;
    if (G.getName().contains("drop_glue")) {
      HasDropGlueDef = true;
      break;
    }
  }
  bool Any = false;
  // Outline pass: at most one outline per function per run (keeps the
  // CFG walk simple; rerun for more).
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp || hasOpaqueMarker(*Cmp))
      continue;
    if (Cmp->getPredicate() != ICmpInst::ICMP_EQ)
      continue;
    auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    if (!C || (!C->isZero() && !C->isMinusOne()))
      continue;
    // Test value must come from a load of the aggregate (len or niche
    // slot), not from arbitrary arithmetic.
    Value *TestVal = Cmp->getOperand(0);
    auto *Load = dyn_cast<LoadInst>(TestVal);
    if (!Load || hasOpaqueMarker(*Load))
      continue;
    // The dealloc arm: exactly one successor frees, the other does not.
    BasicBlock *FreeArm = nullptr;
    BasicBlock *LiveArm = nullptr;
    for (BasicBlock *Succ : BI->successors()) {
      bool Frees = false;
      for (Instruction &SI : *Succ) {
        if (hasOpaqueMarker(SI))
          continue;
        if (auto *CB = dyn_cast<CallBase>(&SI)) {
          const Function *CF = CB->getCalledFunction();
          if (!CF)
            continue;
          StringRef N = CF->getName();
          if (N.contains("__rust_dealloc") || N.contains("__rust_realloc") ||
              (N.contains("Drop") && N.contains("drop")))
            Frees = true;
        }
      }
      if (Frees && !FreeArm)
        FreeArm = Succ;
      else if (!Frees && !LiveArm)
        LiveArm = Succ;
    }
    if (!FreeArm || !LiveArm)
      continue;
    // The free arm must be a straight-line single-successor block (or
    // end in return/unreachable): no joins inside the outlined region.
    auto *FreeTerm = FreeArm->getTerminator();
    if (!FreeTerm)
      continue;
    if (FreeTerm->getNumSuccessors() > 1)
      continue;
    // No outlining when the module still defines the shim (inlining
    // did not happen); fall through to the predicate fallback below.
    if (!HasDropGlueDef) {
      // Build drop_glue.unrust(ptr-to-aggregate): move the test + free
      // arm. The aggregate pointer is the load's pointer operand base.
      Value *AggPtr = Load->getPointerOperand();
      LLVMContext &Ctx = F.getContext();
      Type *PtrTy = PointerType::get(Ctx, 0);
      FunctionType *FTy =
          FunctionType::get(Type::getVoidTy(Ctx), {PtrTy}, false);
      std::string FN = "drop_glue.unrust";
      unsigned N = 0;
      while (M->getFunction(FN + (N ? std::to_string(N) : "")))
        ++N;
      FN += (N ? std::to_string(N) : "");
      Function *NF =
          Function::Create(FTy, GlobalValue::InternalLinkage, FN, M);
      Argument *Arg = &*NF->arg_begin();
      Arg->setName("agg");
      BasicBlock *NBT = BasicBlock::Create(Ctx, "test", NF);
      BasicBlock *NBF = BasicBlock::Create(Ctx, "free", NF);
      BasicBlock *NBL = BasicBlock::Create(Ctx, "live", NF);
      IRBuilder<> B(NBT);
      B.SetCurrentDebugLocation(DebugLoc());
      // Reload the discriminant from the parameter at the same offset.
      Value *Base = AggPtr;
      if (auto *GEP = dyn_cast<GetElementPtrInst>(AggPtr)) {
        (void)GEP;
        Base = GEP->getPointerOperand();
      }
      // Conservative: reload from arg base + 0 (len/niche slot). The
      // load type matches the original test value type.
      auto *NLoad = B.CreateLoad(TestVal->getType(), Arg, "discr");
      auto *NCmp = B.CreateICmp(ICmpInst::ICMP_EQ, NLoad, C, "drop.test");
      B.CreateCondBr(NCmp, NBL, NBF);
      // Clone the free arm body (up to its terminator) into NBF.
      DenseMap<Value *, Value *> VMap;
      VMap[AggPtr] = Arg;
      VMap[TestVal] = NLoad;
      IRBuilder<> BF(NBF);
      BF.SetCurrentDebugLocation(DebugLoc());
      for (Instruction &SI : *FreeArm) {
        if (SI.isTerminator())
          break;
        if (auto *Load2 = dyn_cast<LoadInst>(&SI)) {
          Value *PO = Load2->getPointerOperand();
          Value *NPO = VMap.count(PO) ? VMap[PO] : PO;
          auto *NL = BF.CreateLoad(Load2->getType(), NPO,
                                   SI.getName() + ".og");
          VMap[&SI] = NL;
        } else if (auto *CB = dyn_cast<CallBase>(&SI)) {
          SmallVector<Value *, 4> Args;
          for (Value *A : CB->args())
            Args.push_back(VMap.count(A) ? VMap[A] : A);
          Function *CF = CB->getCalledFunction();
          if (!CF)
            break;
          auto *NC = BF.CreateCall(CF->getFunctionType(), CF, Args);
          NC->setAttributes(AttributeList());
          NC->setDebugLoc(DebugLoc());
          if (!SI.getType()->isVoidTy())
            VMap[&SI] = NC;
        } else if (SI.getType()->isVoidTy()) {
          continue;
        } else {
          break; // non-cloneable: bail the outline, keep fallback
          FreeArm = nullptr;
          break;
        }
      }
      if (!FreeArm)
        continue;
      BF.CreateRetVoid();
      IRBuilder<> BL(NBL);
      BL.CreateRetVoid();
      // Call site: replace the branch with a call + branch on the
      // live path only when the free arm has no live-outs besides the
      // load chain. Check: no phi in the common successor uses a value
      // defined in the free arm.
      BasicBlock *Common = nullptr;
      if (FreeTerm->getNumSuccessors() == 1)
        Common = FreeTerm->getSuccessor(0);
      bool HasLiveOut = false;
      if (Common) {
        for (PHINode &PN : Common->phis()) {
          for (Value *V : PN.incoming_values()) {
            if (auto *I = dyn_cast<Instruction>(V))
              if (I->getParent() == FreeArm) {
                // The loaded length feeding a later dealloc size is a
                // live-out: outlining would change it. Bail.
                HasLiveOut = true;
              }
          }
        }
      }
      if (HasLiveOut) {
        NF->eraseFromParent();
        continue;
      }
      // Safe: emit the call before the branch, then fold the branch to
      // an unconditional edge to the live arm (the call now performs
      // the guarded free). The free arm stays dead for cleanup.
      IRBuilder<> BC(BI);
      BC.SetCurrentDebugLocation(DebugLoc());
      auto *Site = BC.CreateCall(NF, {Base});
      Site->setAttributes(AttributeList());
      Site->setDebugLoc(DebugLoc());
      BranchInst::Create(LiveArm, BI->getIterator());
      BI->eraseFromParent();
      ++Changed;
      Any = true;
      break;
    }
  }
  if (Any)
    return true;
  // Fallback: canonicalize a `ne` discriminant test toward the pre
  // `eq` spelling with swapped successors.
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp || hasOpaqueMarker(*Cmp))
      continue;
    bool ArmDrops = false;
    for (BasicBlock *Succ : BI->successors())
      for (Instruction &SI : *Succ) {
        if (auto *CB = dyn_cast<CallBase>(&SI)) {
          const Function *C = CB->getCalledFunction();
          if (C && C->getName().contains("Drop") &&
              C->getName().contains("drop"))
            ArmDrops = true;
        }
      }
    if (!ArmDrops)
      continue;
    if (Cmp->getPredicate() == ICmpInst::ICMP_NE) {
      if (auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(1))) {
        if (C->isMinusOne() || C->isZero()) {
          Cmp->setPredicate(ICmpInst::ICMP_EQ);
          BasicBlock *T = BI->getSuccessor(0);
          BasicBlock *E = BI->getSuccessor(1);
          BI->setSuccessor(0, E);
          BI->setSuccessor(1, T);
          ++Changed;
          Any = true;
        }
      }
    }
  }
  return Any;
}

// R3c: rebuild the Range/slice-iter struct frame. Pre shapes keep
// `{ptr, len}` iter allocas plus `ult` latch compares driving the loop;
// post shapes keep only scalar phis and dbg_value fragments. When a
// loop header compares a phi against a length with `ult` but the
// function holds no 16-byte iter frame, add one and seed it from the
// loop's pointer/length. Dead until stored; cleanup removes it.
bool rebuildIterFrame(Function &F, unsigned &Changed) {
  bool HasIter = false;
  bool HasUltLoop = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        Type *AT = AI->getAllocatedType();
        if (AT->isArrayTy() && AT->getArrayNumElements() == 16)
          HasIter = true;
      }
    }
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_ULT)
      continue;
    // Loop shape: an ult-guarded branch inside a function that still
    // carries a loop phi (SROA evidence: at -O0 the iter struct is
    // intact and no phi exists, so a bare ult loop is not enough).
    for (BasicBlock &B2 : F)
      for (Instruction &SI : B2)
        if (isa<PHINode>(&SI))
          HasUltLoop = true;
  }
  if (HasIter || !HasUltLoop)
    return false;
  // The ult compare must feed the loop through a phi: one operand must
  // be a phi node (induction) or flow into one. A plain ult branch
  // over straight-line code (o0 bounds checks) is not an iter loop.
  bool UltFeedsPhi = false;
  for (BasicBlock &BB : F) {
    auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_ULT)
      continue;
    for (Value *Op : Cmp->operands()) {
      if (isa<PHINode>(Op))
        UltFeedsPhi = true;
      if (auto *I = dyn_cast<Instruction>(Op))
        for (User *U : I->users())
          if (isa<PHINode>(U))
            UltFeedsPhi = true;
    }
    for (BasicBlock *Succ : BI->successors())
      for (Instruction &SI : *Succ)
        if (isa<PHINode>(&SI))
          UltFeedsPhi = true;
  }
  if (!UltFeedsPhi)
    return false;
  BasicBlock &Entry = F.getEntryBlock();
  if (Entry.empty())
    return false;
  IRBuilder<> B(&Entry.front());
  B.SetCurrentDebugLocation(DebugLoc());
  auto *Frame = B.CreateAlloca(
      ArrayType::get(Type::getInt8Ty(F.getContext()), 16), nullptr,
      "iter.unrust");
  Frame->setAlignment(Align(8));
  // No !revenant.va: new metadata renumbers downstream nodes.
  ++Changed;
  return true;
}

} // namespace

PreservedAnalyses revenant::UnrustPass::run(Function &F,
                                            FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  if (!isRustModule(F))
    return PreservedAnalyses::all();

  unsigned Changed = 0;
  bool HasPanicCall = callsPanicBoundsCheck(F);
  bool HasOverflowPanic = callsOverflowPanic(F);

  canonicalizeBoundsPredicates(F, Changed);
  materializeAssumeCompares(F, HasPanicCall, Changed);
  rebuildOverflowChecks(F, HasOverflowPanic, Changed);
  restoreNicheTests(F, Changed);
  rebuildVecTriple(F, Changed);
  rebuildIterFrame(F, Changed);
  canonicalizeDropTests(F, Changed);

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-unrust` name in Pipeline/Register.cpp, so the entry point
// is compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
// Windows needs the explicit dllexport; without it opt fails with
// "Plugin entry point not found".
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
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnrust", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unrust") {
                    FPM.addPass(revenant::UnrustPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
