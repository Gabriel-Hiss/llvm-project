//===-- Unswitch.cpp - switch deoptimization pass ----------------*- C++ -*-===//
// Part of the Revenant decompiler port. Undo SimplifyCFG switch lowerings
// back into `switch` instructions so the structurer and frontends see the
// source-level construct.
//
// Families handled (all LLVM-IR only, all semantics-preserving):
//
// 1. switch-to-lookup-table (SimplifyCFG `simplifySwitchLookup`,
//    `SwitchReplacement::LookupTableKind): a range check
//    `icmp ult (cond - min), tablesize` + `br` into a block that does
//    `getelementptr @switch.table.*` + `load`, joined by a phi with the
//    default value, becomes a `switch` with one case row per table entry.
//    Table holes (entries equal to the default) stay on the default edge.
// 2. Linear maps (`SwitchReplacement::LinearMapKind`): the `switch_offset`
//    shape (`add; icmp ult; mul; add; select`) where the result is an affine
//    function of the in-range index becomes a `switch` with per-case
//    constants.
// 3. Bitmaps (`SwitchReplacement::BitMapKind`): `zext/trunc; mul by width;
//    lshr; trunc` mask extraction becomes a `switch`.
// 4. Range-to-icmp (`turnSwitchRangeIntoICmp`): `add (cond, -min);
//    icmp ult size; br dest, other` becomes a two-destination `switch`
//    covering [min, min+size).
// 5. Switch-to-select (`trySwitchToSelect`/`foldSwitchToSelect`): chains of
//    `icmp eq cond, C; select` feeding a phi/return become a `switch`.
// 6. Switch-of-select (`simplifySwitchOnSelect`): `switch (select c, X, Y)`
//    on constants becomes `br c, caseX-dest, caseY-dest` when X != Y select
//    one case each.
// 7. Switch/icmp+select merges (`tryToSimplifyUncondBranchWithICmpSelectInIt`
//    and the icmp-only twin): a `switch` whose default block holds only an
//    `icmp eq/ne cond, C` (plus an optional `select` on that icmp) feeding
//    the common phi becomes a `switch` with case C added.
//
// Fire order matters: lookup/linear/bitmap/range/select produce switches,
// then the merge folds stragglers back in. Every rewrite keeps
// `!revenant.va` (copied from the replaced root) and never touches
// `!revenant.opaque` calls: any opaque instruction in the matched region
// vetoes the rewrite.
//
// Equivalence notes: each rewrite replaces a total function of the switch
// condition with a case split computing the same value per input. Table
// entries are constants, so the load equals the case constant by
// construction; the range check guarantees out-of-range inputs take the
// default edge exactly when the original branch did. Linear-map inversion
// is exact integer arithmetic (the multiplier divides every step by
// verification, see below). Select chains compare the same condition
// against the same constants with the same outcomes. No new UB: loads come
// from a constant global fully covered by the range check, and no poison
// propagates (poison table entries are never materialized: a hole maps to
// the default edge).

#include "revenant/Transforms/Unswitch/Unswitch.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <cstdint>
#include <optional>
#include <vector>

using namespace llvm;
using namespace llvm::PatternMatch;

namespace {

void copyProvenance(Instruction *New, Instruction *Old) {
  if (MDNode *VA = Old->getMetadata("revenant.va"))
    New->setMetadata("revenant.va", VA);
  if (MDNode *Op = Old->getMetadata("revenant.opaque"))
    New->setMetadata("revenant.opaque", Op);
}

bool hasOpaque(BasicBlock *BB) {
  for (Instruction &I : *BB)
    if (I.hasMetadata("revenant.opaque"))
      return true;
  return false;
}

// The region between the range check and the join must be pure data flow:
// no calls (opaque or otherwise), no stores, no volatile/atomic ops.
bool regionIsPure(ArrayRef<BasicBlock *> Blocks) {
  for (BasicBlock *BB : Blocks) {
    if (hasOpaque(BB))
      return false;
    for (Instruction &I : *BB) {
      if (isa<CallInst>(I) || isa<InvokeInst>(I))
        return false;
      if (isa<StoreInst>(I) || isa<FenceInst>(I) || isa<AtomicRMWInst>(I) ||
          isa<AtomicCmpXchgInst>(I))
        return false;
      if (auto *LI = dyn_cast<LoadInst>(&I))
        if (!LI->isSimple())
          return false;
      if (!I.isTerminator() && I.mayHaveSideEffects())
        return false;
    }
  }
  return true;
}

// Split `V` as `X + C` (add with constant) or `X - C` (sub with constant,
// folded to C' = -C). Returns the base and the constant offset such that
// V == Base + Off. Follows a single zext/sext/trunc on the way.
struct OffsetSplit {
  Value *Base = nullptr;
  APInt Off{32, 0};
  bool Found = false;
};

OffsetSplit splitOffset(Value *V) {
  V = V->stripPointerCasts();
  if (auto *Z = dyn_cast<ZExtInst>(V))
    V = Z->getOperand(0);
  else if (auto *S = dyn_cast<SExtInst>(V))
    V = S->getOperand(0);
  else if (auto *T = dyn_cast<TruncInst>(V))
    V = T->getOperand(0);
  ConstantInt *C = nullptr;
  Value *X = nullptr;
  if (match(V, m_Add(m_Value(X), m_ConstantInt(C)))) {
    OffsetSplit OS;
    OS.Base = X;
    OS.Off = C->getValue();
    OS.Found = true;
    return OS;
  }
  if (match(V, m_Sub(m_Value(X), m_ConstantInt(C)))) {
    OffsetSplit OS;
    OS.Base = X;
    OS.Off = -C->getValue();
    OS.Found = true;
    return OS;
  }
  return OffsetSplit();
}

// Match `icmp ult Base(+Off), Size` or `icmp ugt Base(+Off), Size-1` (a
// plain range check). On success fills Base (the switch condition value),
// Min (the absolute case minimum), Size, and Negated: false when the true
// edge is in range (ult), true when the false edge is in range (ugt, the
// spelling lifted MSVC/clang guard code prefers).
bool matchRangeCheck(Value *Cond, Value *&OutBase, APInt &OutMin,
                     uint64_t &OutSize, bool &Negated) {
  auto *Cmp = dyn_cast<ICmpInst>(Cond);
  if (!Cmp || (Cmp->getPredicate() != ICmpInst::ICMP_ULT &&
               Cmp->getPredicate() != ICmpInst::ICMP_UGT))
    return false;
  Negated = Cmp->getPredicate() == ICmpInst::ICMP_UGT;
  auto *SizeC = dyn_cast<ConstantInt>(Cmp->getOperand(1));
  if (!SizeC || SizeC->isZero())
    return false;
  Value *Sub = Cmp->getOperand(0);
  OffsetSplit OS = splitOffset(Sub);
  if (OS.Found) {
    OutBase = OS.Base;
    // Sub == Base + Off with Off == -Min, so Min == -Off.
    OutMin = -OS.Off;
  } else {
    OutBase = Sub;
    OutMin = APInt(SizeC->getValue().getBitWidth(), 0);
  }
  if (Negated) {
    // ugt Base(+Off), Max: in range is [Min, Min + Max]; size = Max + 1.
    if (SizeC->isMaxValue(false))
      return false;
    OutSize = SizeC->getValue().getLimitedValue() + 1;
  } else {
    OutSize = SizeC->getValue().getLimitedValue();
  }
  return OutSize > 0 && OutSize <= 1000000;
}

// Collect constant entries of a `@switch.table.*` global. Returns the
// element constants in order, or nullopt when the global is unsuitable
// (non-constant initializer, wrong size, poison/undef entries). For lifted
// IR the compiler's lookup table survives as a constant byte array the
// load indexes by element (`getelementptr i32` on `[N*4 x i8]`); in that
// mode ElemTy is the loaded element type and the bytes are decoded
// little-endian (x86-64).
std::optional<std::vector<Constant *>>
readTable(GlobalVariable *GV, uint64_t ExpectSize, IntegerType *ElemTy) {
  if (!GV || !GV->isConstant() || !GV->hasInitializer())
    return std::nullopt;
  auto *Init = GV->getInitializer();
  auto *ArrayTy = dyn_cast<ArrayType>(Init->getType());
  if (!ArrayTy)
    return std::nullopt;
  std::vector<Constant *> Elems;
  Elems.reserve((size_t)ExpectSize);
  auto *Bytes = dyn_cast<ConstantDataSequential>(Init);
  if (Bytes && ArrayTy->getElementType()->isIntegerTy(8) && ElemTy) {
    // Lifted byte table: ExpectSize elements of ElemTy, little-endian.
    unsigned EW = ElemTy->getBitWidth();
    if (EW == 0 || EW % 8 || EW > 64)
      return std::nullopt;
    uint64_t EBytes = EW / 8;
    uint64_t Off0 = 0;
    if (ArrayTy->getNumElements() < ExpectSize * EBytes)
      return std::nullopt;
    for (uint64_t I = 0; I < ExpectSize; ++I) {
      APInt V(EW, 0);
      for (uint64_t B = 0; B < EBytes; ++B) {
        auto *C = dyn_cast<ConstantInt>(
            Bytes->getElementAsConstant((unsigned)(Off0 + I * EBytes + B)));
        if (!C)
          return std::nullopt;
        V |= C->getValue().zext(EW) << (B * 8);
      }
      Elems.push_back(ConstantInt::get(ElemTy->getContext(), V));
    }
    return Elems;
  }
  if (ArrayTy->getNumElements() != ExpectSize)
    return std::nullopt;
  auto *Data = dyn_cast<ConstantDataSequential>(Init);
  auto *Array = dyn_cast<ConstantArray>(Init);
  if (!Data && !Array)
    return std::nullopt;
  for (uint64_t I = 0; I < ExpectSize; ++I) {
    Constant *C = Data ? Data->getElementAsConstant((unsigned)I)
                       : cast<Constant>(Array->getOperand((unsigned)I));
    if (isa<PoisonValue>(C) || isa<UndefValue>(C))
      return std::nullopt;
    Elems.push_back(C);
  }
  return Elems;
}

// Convention check: only rebuild switches from globals SimplifyCFG itself
// created (`switch.table.<func>`), or from lifted constant byte arrays
// whose contents came straight from the binary's read-only data (the
// `g_<va>` / section-array globals the lifter emits).
bool isSwitchTable(GlobalVariable *GV) {
  if (!GV)
    return false;
  if (GV->getName().starts_with("switch.table."))
    return true;
  if (!GV->isConstant() || !GV->hasInitializer())
    return false;
  auto *Init = dyn_cast<ConstantDataSequential>(GV->getInitializer());
  auto *ArrayTy = Init ? dyn_cast<ArrayType>(Init->getType()) : nullptr;
  return ArrayTy && ArrayTy->getElementType()->isIntegerTy(8);
}

//===----------------------------------------------------------------------===//
// 1-3. Table / linear-map / bitmap lookups -> switch
//===----------------------------------------------------------------------===//

// A candidate lookup join:
//
//   head: %sub = add/sub cond, off; %rc = icmp ult %sub, N;
//         br %rc, lookup, default
//   lookup: [%idx = zext/trunc %sub;] %gep = gep @switch.table.*[, 0], %idx;
//           %ld = load %gep; br join
//   join: %phi = phi [ %ld, lookup ], [ %def, head/default... ]
struct LookupShape {
  BasicBlock *Head = nullptr;
  BasicBlock *Lookup = nullptr;
  BasicBlock *Join = nullptr;
  BasicBlock *Default = nullptr;
  Value *CondBase = nullptr;
  APInt Min{32, 0};
  uint64_t TableSize = 0;
  GlobalVariable *Table = nullptr;
  LoadInst *Load = nullptr;
  PHINode *Phi = nullptr;
  Constant *DefaultVal = nullptr;
  ICmpInst *RangeCheck = nullptr;
  IntegerType *ElemTy = nullptr; // byte-table element type (lifted IR)
};

std::optional<LookupShape> matchLookup(BasicBlock *Head) {
  auto *BI = dyn_cast<BranchInst>(Head->getTerminator());
  if (!BI || !BI->isConditional())
    return std::nullopt;
  auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cmp || Cmp->getParent() != Head)
    return std::nullopt;
  Value *Base = nullptr;
  APInt Min(32, 0);
  uint64_t Size = 0;
  bool Negated = false;
  if (!matchRangeCheck(Cmp, Base, Min, Size, Negated))
    return std::nullopt;

  // ult: the true edge is in range (the lookup). ugt: the false edge is.
  BasicBlock *Lookup = BI->getSuccessor(Negated ? 1 : 0);
  BasicBlock *Default = BI->getSuccessor(Negated ? 0 : 1);
  if (Lookup->getSinglePredecessor() != Head)
    return std::nullopt;
  auto *LookupBr = dyn_cast<BranchInst>(Lookup->getTerminator());
  if (!LookupBr || LookupBr->isConditional())
    return std::nullopt;
  BasicBlock *Join = LookupBr->getSuccessor(0);
  if (!Join)
    return std::nullopt;

  // Join has a phi fed by the lookup load plus at least the default edge.
  PHINode *Phi = nullptr;
  LoadInst *Ld = nullptr;
  IntegerType *ElemTy = nullptr;
  for (PHINode &PN : Join->phis()) {
    Value *FromLookup = PN.getIncomingValueForBlock(Lookup);
    if (!FromLookup)
      continue;
    auto *Load = dyn_cast<LoadInst>(FromLookup);
    if (!Load || Load->getParent() != Lookup)
      continue;
    auto *GEP = dyn_cast<GetElementPtrInst>(Load->getPointerOperand());
    if (!GEP)
      continue;
    auto *GV = dyn_cast<GlobalVariable>(
        GEP->getPointerOperand()->stripPointerCasts());
    if (!isSwitchTable(GV))
      continue;
    // Lifted byte tables are addressed with the element type on the GEP;
    // require it to match the load type so byte offsets decode right.
    if (!GV->getName().starts_with("switch.table.")) {
      auto *ET = dyn_cast<IntegerType>(Load->getType());
      if (!ET || GEP->getSourceElementType() != ET)
        continue;
      ElemTy = ET;
    }
    Phi = &PN;
    Ld = Load;
    break;
  }
  if (!Phi || !Ld)
    return std::nullopt;

  Constant *Def = nullptr;
  for (unsigned I = 0, E = Phi->getNumIncomingValues(); I < E; ++I) {
    BasicBlock *Inc = Phi->getIncomingBlock(I);
    if (Inc == Lookup)
      continue;
    if (Inc != Head && Inc != Default)
      return std::nullopt;
    auto *C = dyn_cast<Constant>(Phi->getIncomingValue(I));
    if (!C)
      return std::nullopt;
    if (!Def)
      Def = C;
    else if (Def != C)
      return std::nullopt;
  }
  if (!Def)
    return std::nullopt;

  // The table address may be indexed as an element pointer (one index) or
  // as an array pointer (leading zero plus element index).
  auto *GEP = cast<GetElementPtrInst>(Ld->getPointerOperand());
  if (GEP->getNumIndices() != 1 && GEP->getNumIndices() != 2)
    return std::nullopt;
  Value *Idx = GEP->getOperand(GEP->getNumIndices());
  Value *Stripped = Idx->stripPointerCasts();
  if (auto *Z = dyn_cast<ZExtInst>(Stripped))
    Stripped = Z->getOperand(0);
  else if (auto *T = dyn_cast<TruncInst>(Stripped))
    Stripped = T->getOperand(0);
  Value *CheckSub = Cmp->getOperand(0);
  if (Stripped != CheckSub && Stripped != Base)
    return std::nullopt;

  auto *GV =
      cast<GlobalVariable>(GEP->getPointerOperand()->stripPointerCasts());
  LookupShape S;
  S.Head = Head;
  S.Lookup = Lookup;
  S.Join = Join;
  S.Default = Default;
  S.CondBase = Base;
  S.Min = Min;
  S.TableSize = Size;
  S.Table = GV;
  S.Load = Ld;
  S.Phi = Phi;
  S.DefaultVal = Def;
  S.RangeCheck = Cmp;
  S.ElemTy = ElemTy;
  return S;
}

bool buildSwitchFromTable(LookupShape &S) {
  auto Elems = readTable(S.Table, S.TableSize, S.ElemTy);
  if (!Elems)
    return false;
  if (!regionIsPure({S.Lookup}))
    return false;

  LLVMContext &Ctx = S.Head->getContext();
  Function *F = S.Head->getParent();

  Type *CondTy = S.CondBase->getType();
  if (!CondTy->isIntegerTy())
    return false;
  unsigned CW = CondTy->getIntegerBitWidth();
  if (CW > 64 || CW == 0)
    return false;

  struct CaseRow {
    APInt Value{32, 0};
    Constant *Result = nullptr;
  };
  std::vector<CaseRow> Rows;
  APInt Min = S.Min.sextOrTrunc(CW);
  for (uint64_t I = 0; I < Elems->size(); ++I) {
    Constant *E = (*Elems)[I];
    if (E == S.DefaultVal)
      continue; // hole: covered by default
    CaseRow R;
    R.Value = Min + APInt(CW, I);
    R.Result = E;
    Rows.push_back(R);
  }
  // All holes (or all entries equal): nothing to switch on. Leave for DCE.
  if (Rows.empty())
    return false;

  // One case block per distinct result value; the join phi takes the
  // per-block constants.
  DenseMap<Constant *, BasicBlock *> BlockFor;
  BasicBlock *Join = S.Join;
  for (auto &R : Rows) {
    if (BlockFor.count(R.Result))
      continue;
    BasicBlock *CB = BasicBlock::Create(Ctx, "sw.case", F, Join);
    IRBuilder<> B(CB);
    Instruction *Br = B.CreateBr(Join);
    copyProvenance(Br, S.RangeCheck);
    BlockFor[R.Result] = CB;
  }

  IRBuilder<> B(S.Head->getTerminator());
  auto *SI = B.CreateSwitch(S.CondBase, S.Default, (unsigned)Rows.size());
  copyProvenance(SI, S.RangeCheck);
  for (auto &R : Rows) {
    BasicBlock *CB = BlockFor[R.Result];
    SI->addCase(ConstantInt::get(Ctx, R.Value), CB);
  }
  for (auto &KV : BlockFor)
    S.Phi->addIncoming(KV.first, KV.second);

  // Keep the incoming value for the new switch default edge.  When the
  // default destination is the join itself, that edge comes directly from
  // Head; otherwise the default block remains the join's predecessor.
  S.Phi->removeIncomingValue(S.Lookup);
  if (S.Default != Join && S.Head != S.Default)
    S.Phi->removeIncomingValue(S.Head);
  if (S.Default != S.Head && S.Default != Join)
    S.Phi->removeIncomingValue(S.Default);

  S.Head->getTerminator()->eraseFromParent();
  S.RangeCheck->eraseFromParent();

  S.Lookup->dropAllReferences();
  S.Lookup->eraseFromParent();
  if (S.Table->use_empty())
    S.Table->eraseFromParent();
  return true;
}

// Linear-map select (the `switch_offset` post shape):
//
//   %sub = add %c, -min
//   %rc = icmp ult %sub, N
//   %scaled = mul %sub, K
//   %res = add %scaled, B
//   %sel = select %rc, %res, default
//   (ret %sel, or phi merging %sel)
//
// i.e. result(i) = B + K*i for i in [0,N), default otherwise. Rebuild a
// switch with N cases. Verification: K != 0 and every case value fits;
// the computation is exact integer arithmetic, so no wrap can differ from
// the original.
bool tryLinearMap(Function &F, BasicBlock &BB) {
  for (Instruction &I : llvm::make_early_inc_range(BB)) {
    auto *Sel = dyn_cast<SelectInst>(&I);
    if (!Sel)
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(Sel->getCondition());
    if (!Cmp || Cmp->getParent() != &BB ||
        Cmp->getPredicate() != ICmpInst::ICMP_ULT)
      continue;
    Value *Sub = Cmp->getOperand(0);
    auto *SizeC = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    if (!SizeC || SizeC->isZero())
      continue;
    uint64_t N = SizeC->getValue().getLimitedValue();
    if (N == 0 || N > 100000)
      continue;
    OffsetSplit OS = splitOffset(Sub);
    if (!OS.Found)
      continue;
    Value *Cond = OS.Base;
    auto *CondTy = dyn_cast<IntegerType>(Cond->getType());
    if (!CondTy)
      continue;
    unsigned CW = CondTy->getBitWidth();
    if (CW > 64 || CW == 0)
      continue;
    APInt Min = (-OS.Off).sextOrTrunc(CW);

    // True value: add(mul(sub-or-cond, K), B). The compiler emits
    // mul((cond-min), K)+B; the lifter may instead fold the offset into
    // the multiply and compute mul(cond, K) directly (same in-range
    // values, the index base is -Min).
    Value *T = Sel->getTrueValue();
    // Allow an intcast around the arithmetic (SimplifyCFG inserts
    // switch.idx.cast when index width differs).
    if (auto *CI = dyn_cast<CastInst>(T))
      if (CI->getSrcTy()->isIntegerTy() && CI->getDestTy()->isIntegerTy())
        T = CI->getOperand(0);
    ConstantInt *K = nullptr;
    ConstantInt *Bc = nullptr;
    Value *AddOp0 = nullptr;
    bool MulOnCond = false; // mul uses Cond, not Sub
    auto *ResTy = dyn_cast<IntegerType>(Sel->getType());
    if (!ResTy)
      continue;
    auto mulBase = [&](Value *M) -> Value * {
      auto *BO = dyn_cast<BinaryOperator>(M);
      if (!BO || BO->getOpcode() != Instruction::Mul)
        return nullptr;
      auto *KC = dyn_cast<ConstantInt>(BO->getOperand(1));
      if (!KC)
        return nullptr;
      return BO->getOperand(0);
    };
    if (match(T, m_Add(m_Value(AddOp0), m_ConstantInt(Bc)))) {
      Value *MulOp = mulBase(AddOp0);
      if (MulOp) {
        K = dyn_cast<ConstantInt>(cast<BinaryOperator>(AddOp0)->getOperand(1));
        if (MulOp == Sub) {
        } else if (MulOp == Cond) {
          MulOnCond = true;
        } else {
          continue;
        }
      } else if (AddOp0 == Sub) {
        K = ConstantInt::get(F.getContext(), APInt(CW, 1));
      } else {
        continue;
      }
    } else if (Value *MulOp = mulBase(T)) {
      K = dyn_cast<ConstantInt>(cast<BinaryOperator>(T)->getOperand(1));
      if (MulOp == Sub) {
      } else if (MulOp == Cond) {
        MulOnCond = true;
      } else {
        continue;
      }
      Bc = ConstantInt::get(F.getContext(), APInt(CW, 0));
    } else if (T == Sub) {
      K = ConstantInt::get(F.getContext(), APInt(CW, 1));
      Bc = ConstantInt::get(F.getContext(), APInt(CW, 0));
    } else {
      continue;
    }
    if (!K || K->isZero())
      continue;
    auto *DefC = dyn_cast<Constant>(Sel->getFalseValue());
    if (!DefC)
      continue;
    if (hasOpaque(&BB))
      continue;
    // Only fire when the select feeds a return or a phi directly (the
    // lookup-table replacement shape), not arbitrary arithmetic.
    bool FeedsEnd = false;
    for (User *U : Sel->users()) {
      if (isa<ReturnInst>(U) || isa<PHINode>(U)) {
        FeedsEnd = true;
        break;
      }
    }
    if (!FeedsEnd)
      continue;

    // Verify each case value exactly: res(i) = B + K*i when the mul runs
    // on the offset, or B + K*(min+i) when it runs on the condition.
    APInt Kv = K->getValue().sextOrTrunc(64);
    APInt Bv = Bc->getValue().sextOrTrunc(64);
    if (MulOnCond)
      Bv += Kv * Min.sextOrTrunc(64);
    unsigned RW = ResTy->getBitWidth();
    LLVMContext &Ctx = F.getContext();
    std::vector<Constant *> CaseVals;
    CaseVals.reserve((size_t)N);
    bool Ok = true;
    for (uint64_t Idx = 0; Idx < N; ++Idx) {
      APInt R = Bv + Kv * APInt(64, Idx);
      if (RW < 64) {
        if (!R.isIntN(RW) && !R.isSignedIntN(RW)) {
          // Truncation would change the value; bail.
          Ok = false;
          break;
        }
      }
      CaseVals.push_back(ConstantInt::get(Ctx, R.trunc(RW)));
    }
    if (!Ok)
      continue;

    // Split BB at the select: head keeps [..cmp], tail starts after.
    BasicBlock *Tail = BB.splitBasicBlock(Sel->getIterator(), "sw.tail");
    Instruction *HeadTerm = BB.getTerminator();
    IRBuilder<> B(HeadTerm);
    auto *SI = B.CreateSwitch(Cond, Tail, (unsigned)N);
    copyProvenance(SI, Cmp);
    DenseMap<Constant *, BasicBlock *> BlockFor;
    for (uint64_t Idx = 0; Idx < N; ++Idx) {
      APInt CaseV = Min + APInt(CW, Idx);
      Constant *Res = CaseVals[Idx];
      // Holes (result == default) stay on the default edge.
      if (Res == DefC)
        continue;
      auto It = BlockFor.find(Res);
      BasicBlock *CB = nullptr;
      if (It == BlockFor.end()) {
        CB = BasicBlock::Create(Ctx, "sw.case", &F, Tail);
        IRBuilder<> CBb(CB);
        Instruction *Br = CBb.CreateBr(Tail);
        copyProvenance(Br, Cmp);
        BlockFor[Res] = CB;
      } else {
        CB = It->second;
      }
      SI->addCase(ConstantInt::get(Ctx, CaseV), CB);
    }
    for (User *U : llvm::make_early_inc_range(Sel->users())) {
      if (auto *PN = dyn_cast<PHINode>(U)) {
        if (PN->getParent() != Tail)
          continue;
        for (unsigned K2 = 0; K2 < PN->getNumIncomingValues(); ++K2) {
          if (PN->getIncomingValue(K2) == Sel &&
              PN->getIncomingBlock(K2) == &BB)
            PN->setIncomingValue(K2, DefC);
        }
        for (auto &KV : BlockFor)
          PN->addIncoming(KV.first, KV.second);
      } else if (auto *RI = dyn_cast<ReturnInst>(U)) {
        IRBuilder<> PB(&Tail->front());
        auto *PN = PB.CreatePHI(ResTy, 2, "sw.ret");
        copyProvenance(PN, Sel);
        PN->addIncoming(DefC, &BB);
        for (auto &KV : BlockFor)
          PN->addIncoming(KV.first, KV.second);
        RI->setOperand(0, PN);
      }
    }
    if (Sel->use_empty()) {
      Sel->eraseFromParent();
      if (Cmp->use_empty())
        Cmp->eraseFromParent();
    }
    HeadTerm->eraseFromParent();
    return true;
  }
  return false;
}

// Bitmap extraction (BitMapKind):
//
//   %idx = zext/trunc %sub
//   %sh = mul %idx, width
//   %down = lshr <bitmap-const>, %sh
//   %res = trunc %down to elt
//
// with the same range check + select/branch around it. Rebuild by reading
// the bitmap constant out of the lshr.
bool tryBitmap(Function &F, BasicBlock &BB) {
  for (Instruction &I : llvm::make_early_inc_range(BB)) {
    auto *Tr = dyn_cast<TruncInst>(&I);
    if (!Tr)
      continue;
    auto *Shr = dyn_cast<BinaryOperator>(Tr->getOperand(0));
    if (!Shr || Shr->getOpcode() != Instruction::LShr)
      continue;
    auto *MapC = dyn_cast<ConstantInt>(Shr->getOperand(0));
    if (!MapC)
      continue;
    auto *Mul = dyn_cast<BinaryOperator>(Shr->getOperand(1));
    if (!Mul || Mul->getOpcode() != Instruction::Mul)
      continue;
    auto *WidthC = dyn_cast<ConstantInt>(Mul->getOperand(1));
    if (!WidthC)
      continue;
    uint64_t Width = WidthC->getValue().getLimitedValue();
    auto *EltTy = dyn_cast<IntegerType>(Tr->getDestTy());
    if (!EltTy)
      continue;
    unsigned EltW = EltTy->getBitWidth();
    if (Width != EltW || EltW == 0 || EltW > 64)
      continue;
    Value *Idx = Mul->getOperand(0);
    if (auto *Z = dyn_cast<ZExtInst>(Idx))
      Idx = Z->getOperand(0);
    else if (auto *T = dyn_cast<TruncInst>(Idx))
      Idx = T->getOperand(0);
    // Idx must be the range-checked offset value; find the ult.
    ICmpInst *Cmp = nullptr;
    for (Instruction &J : BB) {
      auto *C2 = dyn_cast<ICmpInst>(&J);
      if (C2 && C2->getPredicate() == ICmpInst::ICMP_ULT &&
          C2->getOperand(0) == Idx) {
        Cmp = C2;
        break;
      }
    }
    if (!Cmp)
      continue;
    auto *SizeC = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    if (!SizeC)
      continue;
    uint64_t N = SizeC->getValue().getLimitedValue();
    if (N == 0 || N > 1024)
      continue;
    Value *Base = nullptr;
    APInt Min(32, 0);
    uint64_t Sz = 0;
    bool Neg = false;
    if (!matchRangeCheck(Cmp, Base, Min, Sz, Neg))
      continue;
    if (Neg || Sz != N)
      continue;
    auto *BaseTy = dyn_cast<IntegerType>(Base->getType());
    if (!BaseTy || BaseTy->getBitWidth() > 64)
      continue;
    unsigned CW = BaseTy->getBitWidth();
    // Bitmap must hold exactly N elements.
    APInt Map = MapC->getValue();
    if (Map.getBitWidth() != N * EltW)
      continue;
    if (hasOpaque(&BB))
      continue;
    LLVMContext &Ctx = F.getContext();
    APInt MinE = Min.sextOrTrunc(CW);
    // Extract entries (table built high-to-low; entry 0 is the low bits).
    std::vector<Constant *> CaseVals;
    for (uint64_t K = 0; K < N; ++K) {
      APInt E = Map.extractBits(EltW, (unsigned)(K * EltW));
      CaseVals.push_back(ConstantInt::get(Ctx, E));
    }
    // Find the select consuming Tr + Cmp, if any.
    SelectInst *Sel = nullptr;
    for (User *U : Tr->users())
      if (auto *S2 = dyn_cast<SelectInst>(U))
        if (S2->getCondition() == Cmp)
          Sel = S2;
    Constant *DefC = nullptr;
    if (Sel) {
      DefC = dyn_cast<Constant>(Sel->getFalseValue());
      if (!DefC)
        continue;
    }

    BasicBlock *Tail = BB.splitBasicBlock(Tr->getIterator(), "sw.tail");
    Instruction *HeadTerm = BB.getTerminator();
    IRBuilder<> B(HeadTerm);
    auto *SI = B.CreateSwitch(Base, Tail, (unsigned)N);
    copyProvenance(SI, Cmp);
    DenseMap<Constant *, BasicBlock *> BlockFor;
    for (uint64_t K = 0; K < N; ++K) {
      if (DefC && CaseVals[K] == DefC)
        continue;
      APInt CaseV = MinE + APInt(CW, K);
      auto It = BlockFor.find(CaseVals[K]);
      BasicBlock *CB = nullptr;
      if (It == BlockFor.end()) {
        CB = BasicBlock::Create(Ctx, "sw.case", &F, Tail);
        IRBuilder<> CBb(CB);
        Instruction *Br = CBb.CreateBr(Tail);
        copyProvenance(Br, Cmp);
        BlockFor[CaseVals[K]] = CB;
      } else {
        CB = It->second;
      }
      SI->addCase(ConstantInt::get(Ctx, CaseV), CB);
    }
    if (Sel) {
      for (User *U : llvm::make_early_inc_range(Sel->users())) {
        if (auto *PN = dyn_cast<PHINode>(U)) {
          if (PN->getParent() != Tail)
            continue;
          for (unsigned K2 = 0; K2 < PN->getNumIncomingValues(); ++K2)
            if (PN->getIncomingValue(K2) == Sel &&
                PN->getIncomingBlock(K2) == &BB)
              PN->setIncomingValue(K2, DefC);
          for (auto &KV : BlockFor)
            PN->addIncoming(KV.first, KV.second);
        } else if (auto *RI = dyn_cast<ReturnInst>(U)) {
          IRBuilder<> PB(&Tail->front());
          auto *PN = PB.CreatePHI(Sel->getType(), 2, "sw.ret");
          copyProvenance(PN, Sel);
          PN->addIncoming(DefC, &BB);
          for (auto &KV : BlockFor)
            PN->addIncoming(KV.first, KV.second);
          RI->setOperand(0, PN);
        }
      }
      if (Sel->use_empty())
        Sel->eraseFromParent();
    }
    if (Tr->use_empty())
      Tr->eraseFromParent();
    HeadTerm->eraseFromParent();
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// 4. Range-to-icmp branch -> switch (turnSwitchRangeIntoICmp inverse)
//===----------------------------------------------------------------------===//

//   head: [%off = add %c, -min] %rc = icmp ult %off, N; br %rc, dest, other
//
// where dest/other are the two switch destinations. Rebuild:
//
//   switch %c, label %other [ iN..iN+N-1 -> %dest ]  (or swapped)
bool tryRangeToSwitch(Function &F, BasicBlock &BB) {
  auto *BI = dyn_cast<BranchInst>(BB.getTerminator());
  if (!BI || !BI->isConditional() || hasOpaque(&BB))
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cmp || Cmp->getParent() != &BB)
    return false;
  Value *Base = nullptr;
  APInt Min(32, 0);
  uint64_t Size = 0;
  bool Negated = false;
  if (!matchRangeCheck(Cmp, Base, Min, Size, Negated))
    return false;
  if (!Base->getType()->isIntegerTy())
    return false;
  auto *BaseTy = cast<IntegerType>(Base->getType());
  unsigned CW = BaseTy->getBitWidth();
  if (CW > 64 || CW == 0)
    return false;
  // Veto table shapes (handled above): a GEP on a switch.table global in
  // either successor means this is a lookup, not a plain range fold.
  for (unsigned S = 0; S < 2; ++S) {
    BasicBlock *Succ = BI->getSuccessor(S);
    for (Instruction &I : *Succ) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        if (auto *GV = dyn_cast<GlobalVariable>(
                GEP->getPointerOperand()->stripPointerCasts()))
          if (isSwitchTable(GV))
            return false;
      }
      if (isa<SwitchInst>(I))
        return false;
      break; // only inspect the first instruction
    }
  }
  // In-range destination first: ult's true edge, ugt's false edge.
  BasicBlock *Dest = BI->getSuccessor(Negated ? 1 : 0);
  BasicBlock *Other = BI->getSuccessor(Negated ? 0 : 1);
  // A range check guarding another dispatch (jump-table switch, select
  // chain) is structural, not a folded switch: expanding it would stack a
  // duplicate switch in front of the real one.
  if (isa<SwitchInst>(Dest->getTerminator()))
    return false;
  // Small ranges are indistinguishable from source-level interval checks.
  // Only expand ranges large enough to plausibly be folded switch cases.
  if (Size < 4 || Size > 64)
    return false;
  APInt MinE = Min.sextOrTrunc(CW);
  LLVMContext &Ctx = F.getContext();

  IRBuilder<> B(BI);
  auto *SI = B.CreateSwitch(Base, Other, (unsigned)Size);
  copyProvenance(SI, Cmp);
  for (uint64_t I = 0; I < Size; ++I) {
    APInt CaseV = MinE + APInt(CW, I);
    SI->addCase(ConstantInt::get(Ctx, CaseV), Dest);
  }
  // The in-range edge now arrives Size times at Dest: replicate the phi
  // incoming value.
  for (PHINode &PN : Dest->phis()) {
    int Idx = PN.getBasicBlockIndex(&BB);
    if (Idx == -1)
      continue;
    Value *V = PN.getIncomingValue((unsigned)Idx);
    for (uint64_t I = 1; I < Size; ++I)
      PN.addIncoming(V, &BB);
  }
  BI->eraseFromParent();
  if (Cmp->use_empty())
    Cmp->eraseFromParent();
  return true;
}

//===----------------------------------------------------------------------===//
// 5. Select chains -> switch (trySwitchToSelect / foldSwitchToSelect inverse)
//===----------------------------------------------------------------------===//

// One `icmp eq Cond, C` selecting between two constants, chained through
// selects into a phi or return.
struct SelectCase {
  ICmpInst *Cmp = nullptr;
  ConstantInt *CaseVal = nullptr; // null for mask/bit-test rows
  APInt Mask{32, 0};              // valid when IsMask
  APInt MaskExpect{32, 0};        // valid when IsMask
  bool IsMask = false;
  Constant *Result = nullptr;
};

bool matchEqCompare(Value *V, Value *Cond, ConstantInt *&OutC) {
  auto *Cmp = dyn_cast<ICmpInst>(V);
  if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_EQ)
    return false;
  if (Cmp->getOperand(0) == Cond) {
    OutC = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    return OutC != nullptr;
  }
  if (Cmp->getOperand(1) == Cond) {
    OutC = dyn_cast<ConstantInt>(Cmp->getOperand(0));
    return OutC != nullptr;
  }
  return false;
}

// Mask/bit-test rows from foldSwitchToSelect:
//   (a) and(Cond, Mask) == 0 / == Mask  -> handled as mask row
//   (c) or(icmp eq, icmp eq) of two values -> two rows (handled by caller)
bool matchMaskRow(ICmpInst *Cmp, Value *Cond, APInt &OutMask,
                  APInt &OutExpect, bool &OutNeg) {
  if (!Cmp || (Cmp->getPredicate() != ICmpInst::ICMP_EQ &&
               Cmp->getPredicate() != ICmpInst::ICMP_NE))
    return false;
  OutNeg = (Cmp->getPredicate() == ICmpInst::ICMP_NE);
  Value *A = Cmp->getOperand(0);
  Value *B = Cmp->getOperand(1);
  auto *BC = dyn_cast<ConstantInt>(B);
  auto *And = dyn_cast<BinaryOperator>(A);
  if (And && And->getOpcode() == Instruction::And && BC) {
    Value *X = And->getOperand(0);
    auto *MC = dyn_cast<ConstantInt>(And->getOperand(1));
    if (X == Cond && MC) {
      OutMask = MC->getValue();
      OutExpect = BC->getValue();
      return true;
    }
    return false;
  }
  return false;
}

// Walk a select chain: Sel picks between constants based on compares of
// Cond, with the false arm chaining to the next select (or the default
// constant). Fills Rows + Default. Returns Cond on success.
Value *matchSelectChain(Value *V, std::vector<SelectCase> &Rows,
                        Constant *&Default, Value *HintCond = nullptr) {
  Value *Cond = HintCond;
  Value *Cur = V;
  SmallPtrSet<Value *, 8> Seen;
  while (auto *Sel = dyn_cast<SelectInst>(Cur)) {
    if (!Seen.insert(Sel).second)
      return nullptr;
    Value *C = Sel->getCondition();
    // or(icmp eq, icmp eq) two-value row.
    if (auto *Or = dyn_cast<BinaryOperator>(C)) {
      if (Or->getOpcode() == Instruction::Or) {
        auto *L = dyn_cast<ICmpInst>(Or->getOperand(0));
        auto *R = dyn_cast<ICmpInst>(Or->getOperand(1));
        ConstantInt *LC = nullptr;
        ConstantInt *RC = nullptr;
        Value *LCond = Cond;
        if (!LCond) {
          if (L && L->getPredicate() == ICmpInst::ICMP_EQ) {
            if (isa<ConstantInt>(L->getOperand(1)))
              LCond = L->getOperand(0);
            else if (isa<ConstantInt>(L->getOperand(0)))
              LCond = L->getOperand(1);
          }
        }
        if (LCond && L && R && matchEqCompare(L, LCond, LC) &&
            matchEqCompare(R, LCond, RC)) {
          if (!Cond)
            Cond = LCond;
          else if (Cond != LCond)
            return nullptr;
          auto *Res = dyn_cast<Constant>(Sel->getTrueValue());
          if (!Res)
            return nullptr;
          SelectCase R1;
          R1.Cmp = L;
          R1.CaseVal = LC;
          R1.Result = Res;
          Rows.push_back(R1);
          SelectCase R2;
          R2.Cmp = R;
          R2.CaseVal = RC;
          R2.Result = Res;
          Rows.push_back(R2);
          Cur = Sel->getFalseValue();
          continue;
        }
        return nullptr;
      }
    }
    auto *Cmp = dyn_cast<ICmpInst>(C);
    if (!Cmp)
      return nullptr;
    // Derive Cond from a plain eq compare when unknown.
    if (!Cond) {
      if (Cmp->getPredicate() == ICmpInst::ICMP_EQ) {
        if (isa<ConstantInt>(Cmp->getOperand(1)))
          Cond = Cmp->getOperand(0);
        else if (isa<ConstantInt>(Cmp->getOperand(0)))
          Cond = Cmp->getOperand(1);
        else
          return nullptr;
      } else {
        return nullptr;
      }
    }
    ConstantInt *CaseC = nullptr;
    if (matchEqCompare(Cmp, Cond, CaseC)) {
      auto *Res = dyn_cast<Constant>(Sel->getTrueValue());
      if (!Res)
        return nullptr;
      SelectCase R;
      R.Cmp = Cmp;
      R.CaseVal = CaseC;
      R.Result = Res;
      Rows.push_back(R);
      Cur = Sel->getFalseValue();
      continue;
    }
    APInt Mask(32, 0);
    APInt Expect(32, 0);
    bool Neg = false;
    if (matchMaskRow(Cmp, Cond, Mask, Expect, Neg)) {
      auto *ResT = dyn_cast<Constant>(Sel->getTrueValue());
      auto *ResF = dyn_cast<Constant>(Sel->getFalseValue());
      if (!ResT || !ResF)
        return nullptr;
      // EQ: true arm is the masked set; NE swaps.
      Constant *Hit = Neg ? ResF : ResT;
      SelectCase R;
      R.Cmp = Cmp;
      R.CaseVal = nullptr;
      R.Mask = Mask;
      R.MaskExpect = Expect;
      R.IsMask = true;
      R.Result = Hit;
      Rows.push_back(R);
      Cur = Neg ? (Value *)ResT : (Value *)ResF;
      // Mask rows are terminal (foldSwitchToSelect emits a single select
      // for them).
      if (!isa<Constant>(Cur))
        return nullptr;
      Default = cast<Constant>(Cur);
      return Cond;
    }
    return nullptr;
  }
  auto *DefC = dyn_cast<Constant>(Cur);
  if (!DefC)
    return nullptr;
  Default = DefC;
  return Cond;
}

bool trySelectToSwitch(Function &F, BasicBlock &BB) {
  // Candidate roots: a return of a select chain, or a phi with a select
  // incoming in a successor join block.
  struct Root {
    SelectInst *Sel = nullptr;
    BasicBlock *Join = nullptr; // block holding the phi/ret, or BB
    PHINode *Phi = nullptr;
    ReturnInst *Ret = nullptr;
  };
  std::vector<Root> Roots;
  for (Instruction &I : BB) {
    auto *RI = dyn_cast<ReturnInst>(&I);
    if (!RI || RI->getNumOperands() == 0)
      continue;
    if (auto *S = dyn_cast<SelectInst>(RI->getOperand(0))) {
      Root R;
      R.Sel = S;
      R.Join = &BB;
      R.Ret = RI;
      Roots.push_back(R);
    }
  }
  for (BasicBlock *Succ : successors(&BB)) {
    for (PHINode &PN : Succ->phis()) {
      for (unsigned K = 0; K < PN.getNumIncomingValues(); ++K) {
        if (PN.getIncomingBlock(K) != &BB)
          continue;
        if (auto *S = dyn_cast<SelectInst>(PN.getIncomingValue(K)))
          if (S->getParent() == &BB) {
            Root R;
            R.Sel = S;
            R.Join = Succ;
            R.Phi = &PN;
            Roots.push_back(R);
          }
      }
    }
  }
  // Lifted diamonds: a range pre-check in BB branches to a chain block
  // holding the selects, which falls into the join. The selects sit in
  // the single-predecessor successor rather than BB itself.
  for (BasicBlock *Succ : successors(&BB)) {
    if (Succ->getSinglePredecessor() != &BB)
      continue;
    auto *SuccBr = dyn_cast<BranchInst>(Succ->getTerminator());
    if (!SuccBr)
      continue;
    for (Instruction &I : *Succ) {
      auto *S = dyn_cast<SelectInst>(&I);
      if (!S)
        continue;
      if (SuccBr->isConditional())
        break; // chain block must fall through to the join
      BasicBlock *Join = SuccBr->getSuccessor(0);
      for (PHINode &PN : Join->phis()) {
        for (unsigned K = 0, E = PN.getNumIncomingValues(); K < E; ++K) {
          if (PN.getIncomingBlock(K) == Succ &&
              PN.getIncomingValue(K) == S) {
            Root R;
            R.Sel = S;
            R.Join = Join;
            R.Phi = &PN;
            Roots.push_back(R);
          }
        }
      }
      if (auto *RI = dyn_cast<ReturnInst>(Succ->getTerminator()))
        if (RI->getNumOperands() && RI->getOperand(0) == S) {
          Root R;
          R.Sel = S;
          R.Join = Succ;
          R.Ret = RI;
          Roots.push_back(R);
        }
      break; // one root per chain block is enough; chain walk finds rest
    }
  }
  for (Root R : Roots) {
    std::vector<SelectCase> Rows;
    Constant *Def = nullptr;
    Value *Cond = matchSelectChain(R.Sel, Rows, Def);
    if (!Cond || Rows.empty() || !Def)
      continue;
    if (!Cond->getType()->isIntegerTy())
      continue;
  // All compares and selects must live in BB (single-block chain, the
  // shape foldSwitchToSelect produces) or in the chain block holding the
  // root select (lifted diamond: range pre-check in BB, selects next).
  BasicBlock *ChainBB = R.Sel->getParent();
  bool AllInBB = true;
  for (auto &Row : Rows) {
    if (Row.Cmp->getParent() != &BB && Row.Cmp->getParent() != ChainBB) {
      AllInBB = false;
      break;
    }
  }
  if (!AllInBB)
    continue;
    // At least two eq rows, or a mask row (foldSwitchToSelect needs >= 2
    // cases or a mask row; a lone eq+select is InstCombine's, not a
    // switch).
    size_t EqRows = 0;
    for (auto &Row : Rows)
      if (!Row.IsMask)
        EqRows++;
    if (EqRows == 1 && Rows.size() == 1 && !Rows[0].IsMask)
      continue;
    // Bail on duplicates that a switch cannot hold.
    {
      SmallPtrSet<ConstantInt *, 8> Seen;
      bool Dup = false;
      for (auto &Row : Rows) {
        if (Row.IsMask)
          continue;
        if (!Seen.insert(Row.CaseVal).second) {
          Dup = true;
          break;
        }
      }
      if (Dup)
        continue;
    }
    if (hasOpaque(&BB))
      continue;
    LLVMContext &Ctx = F.getContext();

    if (R.Ret) {
      // ret select => split: switch in BB, one case block per result.
      BasicBlock *Tail = BB.splitBasicBlock(R.Ret->getIterator(), "sw.tail");
      Instruction *HeadTerm = BB.getTerminator();
      IRBuilder<> B(HeadTerm);
      auto *SI = B.CreateSwitch(Cond, Tail, (unsigned)Rows.size());
      copyProvenance(SI, R.Sel);
      DenseMap<Constant *, BasicBlock *> BlockFor;
      bool Bad = false;
      for (auto &Row : Rows) {
        if (Row.IsMask) {
          // Expand mask row: enumerate set bits (bounded: bail if huge).
          if (Row.Mask.getBitWidth() > 64) {
            Bad = true;
            break;
          }
          APInt Free = ~Row.Mask;
          unsigned FreeN = Free.popcount();
          if (FreeN > 10) {
            Bad = true;
            break;
          }
          uint64_t Free64 = Free.getLimitedValue();
          uint64_t Count = 0;
          for (uint64_t SubM = Free64;; SubM = (SubM - 1) & Free64) {
            APInt V(Row.Mask.getBitWidth(), SubM);
            V &= Free;
            V |= (Row.MaskExpect & Row.Mask);
            auto It = BlockFor.find(Row.Result);
            BasicBlock *CB = nullptr;
            if (It == BlockFor.end()) {
              CB = BasicBlock::Create(Ctx, "sw.case", &F, Tail);
              IRBuilder<> CBb(CB);
              Instruction *Br = CBb.CreateBr(Tail);
              copyProvenance(Br, Row.Cmp);
              BlockFor[Row.Result] = CB;
            } else {
              CB = It->second;
            }
            SI->addCase(ConstantInt::get(Ctx, V), CB);
            if (++Count > 1024) {
              Bad = true;
              break;
            }
            if (SubM == 0)
              break;
          }
          if (Bad)
            break;
          continue;
        }
        auto It = BlockFor.find(Row.Result);
        BasicBlock *CB = nullptr;
        if (It == BlockFor.end()) {
          CB = BasicBlock::Create(Ctx, "sw.case", &F, Tail);
          IRBuilder<> CBb(CB);
          Instruction *Br = CBb.CreateBr(Tail);
          copyProvenance(Br, Row.Cmp);
          BlockFor[Row.Result] = CB;
        } else {
          CB = It->second;
        }
        SI->addCase(Row.CaseVal, CB);
      }
      if (Bad) {
        SI->eraseFromParent();
        Tail->eraseFromParent();
        // NOTE: BB was split; merging back is nontrivial. The split left
        // BB -> Tail as a plain fallthrough, which is still correct IR,
        // just not a switch. Continue to the next root.
        continue;
      }
      IRBuilder<> PB(&Tail->front());
      auto *PN = PB.CreatePHI(R.Ret->getOperand(0)->getType(), 2, "sw.ret");
      copyProvenance(PN, R.Sel);
      PN->addIncoming(Def, &BB);
      for (auto &KV : BlockFor)
        PN->addIncoming(KV.first, KV.second);
      R.Ret->setOperand(0, PN);
      if (R.Sel->use_empty()) {
        SmallVector<Instruction *, 8> Dead;
        Value *Cur = R.Sel;
        while (auto *S = dyn_cast<SelectInst>(Cur)) {
          Dead.push_back(S);
          if (auto *C2 = dyn_cast<Instruction>(S->getCondition()))
            if (C2->getParent() == &BB && C2->use_empty())
              Dead.push_back(C2);
          Cur = S->getFalseValue();
          if (!isa<SelectInst>(Cur))
            break;
        }
        for (Instruction *D : Dead)
          if (D->use_empty())
            D->eraseFromParent();
      }
      HeadTerm->eraseFromParent();
      return true;
    }
    if (R.Phi) {
      BasicBlock *Join = R.Join;
      BasicBlock *Chain = R.Sel->getParent();
      int Slot = -1;
      for (unsigned K = 0; K < R.Phi->getNumIncomingValues(); ++K)
        if (R.Phi->getIncomingBlock(K) == Chain &&
            R.Phi->getIncomingValue(K) == R.Sel) {
          Slot = (int)K;
          break;
        }
      if (Slot < 0)
        continue;
      // Skip mask rows in phi position (rare; keep the select).
      bool HasMask = false;
      for (auto &Row : Rows)
        if (Row.IsMask)
          HasMask = true;
      if (HasMask)
        continue;
      R.Phi->setIncomingValue((unsigned)Slot, Def);
      // Lifted diamond: the switch replaces the chain block's fallthrough
      // once the pre-check proves BB == Chain's only predecessor; the
      // range check in BB already routes out-of-range inputs to the join,
      // so every value reaching here is in range and the default edge
      // (Join) only carries the chain's own default.
      BasicBlock *SwitchBB = (Chain != &BB) ? Chain : &BB;
      Instruction *HeadTerm = SwitchBB->getTerminator();
      IRBuilder<> B(HeadTerm);
      auto *SI = B.CreateSwitch(Cond, Join, (unsigned)Rows.size());
      copyProvenance(SI, R.Sel);
      DenseMap<Constant *, BasicBlock *> BlockFor;
      for (auto &Row : Rows) {
        auto It = BlockFor.find(Row.Result);
        BasicBlock *CB = nullptr;
        if (It == BlockFor.end()) {
          CB = BasicBlock::Create(Ctx, "sw.case", &F, Join);
          IRBuilder<> CBb(CB);
          Instruction *Br = CBb.CreateBr(Join);
          copyProvenance(Br, Row.Cmp);
          BlockFor[Row.Result] = CB;
        } else {
          CB = It->second;
        }
        SI->addCase(Row.CaseVal, CB);
      }
      for (auto &KV : BlockFor)
        R.Phi->addIncoming(KV.first, KV.second);
      HeadTerm->eraseFromParent();
      if (R.Sel->use_empty())
        R.Sel->eraseFromParent();
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// 6. switch-of-select -> branch (simplifySwitchOnSelect inverse)
//===----------------------------------------------------------------------===//

//   %s = select %c, C1, C2   (C1, C2 constants)
//   switch %s, ... [ C1 -> BB1, C2 -> BB2, ... ]
//
// becomes br %c, BB1, BB2 when both select constants have distinct
// destinations and the switch has no other cases. The default is
// unreachable because the select can produce only the two matched constants.
bool trySwitchOfSelect(Function &F, BasicBlock &BB) {
  (void)F;
  auto *SI = dyn_cast<SwitchInst>(BB.getTerminator());
  if (!SI || hasOpaque(&BB))
    return false;
  auto *Sel = dyn_cast<SelectInst>(SI->getCondition());
  if (!Sel || Sel->getParent() != &BB)
    return false;
  auto *C1 = dyn_cast<ConstantInt>(Sel->getTrueValue());
  auto *C2 = dyn_cast<ConstantInt>(Sel->getFalseValue());
  if (!C1 || !C2 || C1 == C2)
    return false;
  SwitchInst::CaseIt Dest1 = SI->findCaseValue(C1);
  SwitchInst::CaseIt Dest2 = SI->findCaseValue(C2);
  SwitchInst::CaseIt DefIt = SI->case_default();
  if (Dest1 == DefIt || Dest2 == DefIt)
    return false;
  BasicBlock *BB1 = Dest1->getCaseSuccessor();
  BasicBlock *BB2 = Dest2->getCaseSuccessor();
  if (BB1 == BB2)
    return false;
  // No other cases allowed (they would be lost).
  if (SI->getNumCases() != 2)
    return false;
  BasicBlock *Def = SI->getDefaultDest();
  // Both select arms are covered by the only two cases, so the default
  // destination cannot be reached, regardless of its block contents.
  Value *C = Sel->getCondition();
  IRBuilder<> B(SI);
  Instruction *Br = B.CreateCondBr(C, BB1, BB2);
  copyProvenance(Br, SI);
  // Default block loses its predecessor if it was distinct.
  if (Def != BB1 && Def != BB2)
    Def->removePredecessor(&BB);
  if (Sel->use_empty())
    Sel->eraseFromParent();
  SI->eraseFromParent();
  return true;
}

//===----------------------------------------------------------------------===//
// 7. switch default icmp/select merge -> switch (merge inverse)
//===----------------------------------------------------------------------===//

//   head: switch %c, default [ ... ]
//   default: [%t = icmp eq/ne %c, K; [%v = select %t, A, B;] br join
//   join: %phi = phi [ ..., default ], ...
//
// becomes a switch with case K added (eq: K -> split edge carrying A;
// ne: swapped), mirroring tryToSimplifyUncondBranchWithICmpSelectInIt.
bool tryMergeDefault(Function &F, BasicBlock &Head) {
  auto *SI = dyn_cast<SwitchInst>(Head.getTerminator());
  if (!SI || hasOpaque(&Head))
    return false;
  BasicBlock *Def = SI->getDefaultDest();
  if (!Def || Def->getSinglePredecessor() != &Head)
    return false;
  if (hasOpaque(Def))
    return false;
  // Default block shape: [icmp (+select)] + uncond br.
  ICmpInst *ICI = nullptr;
  SelectInst *Sel = nullptr;
  for (Instruction &I : *Def) {
    if (isa<PHINode>(I))
      return false;
    if (auto *C2 = dyn_cast<ICmpInst>(&I)) {
      if (ICI)
        return false;
      ICI = C2;
    } else if (auto *S = dyn_cast<SelectInst>(&I)) {
      if (Sel)
        return false;
      Sel = S;
    } else if (I.isTerminator()) {
      if (!isa<BranchInst>(I) || !cast<BranchInst>(I).isUnconditional())
        return false;
    } else {
      return false; // only icmp/select allowed
    }
  }
  if (!ICI)
    return false;
  if (!ICI->hasOneUse())
    return false;
  if (Sel && Sel->getCondition() != ICI)
    return false;
  if (Sel && !Sel->hasOneUse())
    return false;
  // icmp X, K with X == switch cond.
  Value *X = ICI->getOperand(0);
  auto *K = dyn_cast<ConstantInt>(ICI->getOperand(1));
  ICmpInst::Predicate Pred = ICI->getPredicate();
  if (Pred != ICmpInst::ICMP_EQ && Pred != ICmpInst::ICMP_NE)
    return false;
  if (X != SI->getCondition()) {
    // Try commuted: K on the left.
    if (ICI->getOperand(0) != SI->getCondition())
      return false;
    K = dyn_cast<ConstantInt>(ICI->getOperand(0));
    X = ICI->getOperand(1);
    if (!K)
      return false;
  }
  if (!K)
    return false;
  // K must not already be a case.
  if (SI->findCaseValue(K) != SI->case_default())
    return false;
  BasicBlock *Join = cast<BranchInst>(Def->getTerminator())->getSuccessor(0);
  // The select (or icmp) must feed a phi in Join.
  Value *UserV = Sel ? (Value *)Sel : (Value *)ICI;
  PHINode *Phi = nullptr;
  for (User *U : UserV->users()) {
    if (auto *PN = dyn_cast<PHINode>(U))
      if (PN->getParent() == Join) {
        Phi = PN;
        break;
      }
  }
  if (!Phi)
    return false;
  Constant *TrueC = nullptr;
  Constant *FalseC = nullptr;
  if (Sel) {
    TrueC = dyn_cast<Constant>(Sel->getTrueValue());
    FalseC = dyn_cast<Constant>(Sel->getFalseValue());
    if (!TrueC || !FalseC)
      return false;
  }
  LLVMContext &Ctx = F.getContext();
  // New edge block carrying the K value.
  BasicBlock *NewBB = BasicBlock::Create(Ctx, "switch.edge", &F, Def);
  IRBuilder<> NB(NewBB);
  Instruction *NewBr = NB.CreateBr(Join);
  copyProvenance(NewBr, &Def->front());
  // EQ: K takes TrueC, default keeps FalseC. NE: swapped.
  Constant *NewC = (Pred == ICmpInst::ICMP_EQ) ? TrueC : FalseC;
  Constant *DefC = (Pred == ICmpInst::ICMP_EQ) ? FalseC : TrueC;
  if (Sel) {
    for (unsigned Idx = 0; Idx < Phi->getNumIncomingValues(); ++Idx) {
      if (Phi->getIncomingBlock(Idx) == Def &&
          Phi->getIncomingValue(Idx) == (Value *)Sel)
        Phi->setIncomingValue(Idx, DefC);
    }
  } else {
    // icmp-only (hidden select true/false): phi entries are i1 true/false.
    for (unsigned Idx = 0; Idx < Phi->getNumIncomingValues(); ++Idx) {
      if (Phi->getIncomingBlock(Idx) == Def &&
          Phi->getIncomingValue(Idx) == (Value *)ICI) {
        Phi->setIncomingValue(
            Idx, (Pred == ICmpInst::ICMP_EQ)
                     ? (Constant *)ConstantInt::getFalse(Ctx)
                     : (Constant *)ConstantInt::getTrue(Ctx));
      }
    }
    NewC = (Pred == ICmpInst::ICMP_EQ) ? (Constant *)ConstantInt::getTrue(Ctx)
                                       : (Constant *)ConstantInt::getFalse(Ctx);
  }
  Phi->addIncoming(NewC, NewBB);
  SI->addCase(K, NewBB);
  if (Sel) {
    Sel->replaceAllUsesWith(DefC);
    Sel->eraseFromParent();
  }
  if (ICI->use_empty())
    ICI->eraseFromParent();
  return true;
}

//===----------------------------------------------------------------------===//
// 8. Compare trees across arms -> one switch
//    (FoldValueComparisonIntoPredecessors + switchToSelect inverse)
//===----------------------------------------------------------------------===//

// A switch whose arms SimplifyCFG turned into compare trees:
//
//   head: %c = icmp <pred> %v, K; br %c, label %t, label %f
//   f:    %sel = <select chain over %v>; br join
//   t:    %sel = <select chain over %v>; br join
//   join: %phi = phi [ %sel_f, f ], [ %sel_t, t ]
//
// becomes one switch on %v at the head with every case row from both
// chains, the shared chain default as the switch default, and the arms
// deleted. The head's condition partitions the value space, so merging
// is only sound when every case constant lives on the arm that can
// reach it: each case is proven against `ConstantRange` of that arm's
// edge, and both arms must agree on the default constant.
bool trySelectTree(Function &F, BasicBlock &Head) {
  auto *BI = dyn_cast<BranchInst>(Head.getTerminator());
  if (!BI || !BI->isConditional() || hasOpaque(&Head))
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
  if (!Cmp || Cmp->getParent() != &Head)
    return false;
  CmpInst::Predicate Pred = Cmp->getPredicate();
  Value *V = nullptr;
  ConstantInt *K = nullptr;
  if (auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(1))) {
    V = Cmp->getOperand(0);
    K = C;
  } else if (auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(0))) {
    // Commuted compare: restate it as `pred V, K` so the range proof
    // reads one direction.
    V = Cmp->getOperand(1);
    K = C;
    Pred = Cmp->getSwappedPredicate();
  }
  if (!V || !K)
    return false;
  auto *VTy = dyn_cast<IntegerType>(V->getType());
  if (!VTy)
    return false;
  unsigned CW = VTy->getBitWidth();
  if (CW > 64 || CW == 0)
    return false;

  BasicBlock *TrueArm = BI->getSuccessor(0);
  BasicBlock *FalseArm = BI->getSuccessor(1);
  if (TrueArm == FalseArm || TrueArm == &Head || FalseArm == &Head)
    return false;
  BasicBlock *Join = nullptr;
  for (unsigned Side = 0; Side < 2; ++Side) {
    BasicBlock *Arm = Side ? FalseArm : TrueArm;
    if (Arm->getSinglePredecessor() != &Head)
      return false;
    if (hasOpaque(Arm))
      return false;
    if (isa<PHINode>(Arm->front()))
      return false;
    auto *Br = dyn_cast<BranchInst>(Arm->getTerminator());
    if (!Br || Br->isConditional())
      return false;
    if (!Join)
      Join = Br->getSuccessor(0);
    else if (Join != Br->getSuccessor(0))
      return false;
  }
  if (!Join || Join == &Head || Join == TrueArm || Join == FalseArm)
    return false;
  if (hasOpaque(Join))
    return false;
  if (!Join->hasNPredecessors(2))
    return false;

  // Exactly one phi, fed by both arms.
  auto PHIs = Join->phis();
  if (PHIs.begin() == PHIs.end() || std::next(PHIs.begin()) != PHIs.end())
    return false;
  PHINode *Phi = &*PHIs.begin();
  Value *RootT = Phi->getIncomingValueForBlock(TrueArm);
  Value *RootF = Phi->getIncomingValueForBlock(FalseArm);
  if (!RootT || !RootF)
    return false;
  auto *SelT = dyn_cast<SelectInst>(RootT);
  auto *SelF = dyn_cast<SelectInst>(RootF);
  if (!SelT || !SelF || SelT->getParent() != TrueArm ||
      SelF->getParent() != FalseArm)
    return false;

  // Every other value computed in an arm must stay inside the arm,
  // otherwise deleting the arm would drop live code.
  for (unsigned Side = 0; Side < 2; ++Side) {
    BasicBlock *Arm = Side ? FalseArm : TrueArm;
    Value *Root = Side ? RootF : RootT;
    for (Instruction &I : *Arm) {
      if (I.isTerminator())
        continue;
      for (User *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI)
          return false; // a constant never uses an instruction
        if (UI->getParent() == Arm)
          continue;
        if (U == Phi && (Value *)&I == Root)
          continue; // the chain root feeds the phi
        return false;
      }
    }
  }

  std::vector<SelectCase> RowsT;
  std::vector<SelectCase> RowsF;
  Constant *DefT = nullptr;
  Constant *DefF = nullptr;
  // Mask rows cannot be expressed as flat case rows without enumerating
  // their sets; those stay with the per-block chain rewrite.
  Value *CondT = matchSelectChain(SelT, RowsT, DefT, V);
  Value *CondF = matchSelectChain(SelF, RowsF, DefF, V);
  if (!CondT || !CondF || !DefT || !DefF)
    return false;
  if (CondT != V || CondF != V)
    return false;
  if (RowsT.empty() || RowsF.empty())
    return false;
  for (auto &R : RowsT)
    if (R.IsMask || !R.CaseVal)
      return false;
  for (auto &R : RowsF)
    if (R.IsMask || !R.CaseVal)
      return false;
  if (DefT != DefF)
    return false;

  // Range proof: a case on the true arm must satisfy `pred v, K`, one on
  // the false arm must not.
  ConstantRange TrueRange = ConstantRange::makeExactICmpRegion(
      Pred, K->getValue().zextOrTrunc(CW));
  ConstantRange FalseRange = TrueRange.inverse();
  auto inRange = [&](ConstantRange Range, const APInt &Val) {
    if (Val.getBitWidth() != CW)
      return false;
    return Range.contains(Val);
  };
  for (auto &R : RowsT)
    if (!inRange(TrueRange, R.CaseVal->getValue()))
      return false;
  for (auto &R : RowsF)
    if (!inRange(FalseRange, R.CaseVal->getValue()))
      return false;
  // No case value twice across the arms: a switch has one row per value.
  SmallPtrSet<ConstantInt *, 8> Seen;
  for (unsigned Side = 0; Side < 2; ++Side)
    for (auto &R : Side ? RowsF : RowsT)
      if (!Seen.insert(R.CaseVal).second)
        return false;

  LLVMContext &Ctx = F.getContext();
  IRBuilder<> B(BI);
  unsigned NumCases = (unsigned)(RowsT.size() + RowsF.size());
  auto *SI = B.CreateSwitch(V, Join, NumCases);
  copyProvenance(SI, Cmp);
  Constant *Def = DefT;
  // The head's switch default edge now carries the shared default.
  Phi->addIncoming(Def, &Head);
  for (unsigned Side = 0; Side < 2; ++Side) {
    for (auto &R : Side ? RowsF : RowsT) {
      BasicBlock *CB = BasicBlock::Create(Ctx, "sw.case", &F, Join);
      IRBuilder<> CBb(CB);
      Instruction *Br = CBb.CreateBr(Join);
      copyProvenance(Br, R.Cmp);
      SI->addCase(R.CaseVal, CB);
      Phi->addIncoming(R.Result, CB);
    }
  }
  Phi->removeIncomingValue(TrueArm);
  Phi->removeIncomingValue(FalseArm);

  BI->eraseFromParent();
  if (Cmp->use_empty())
    Cmp->eraseFromParent();
  for (unsigned Side = 0; Side < 2; ++Side) {
    BasicBlock *Arm = Side ? FalseArm : TrueArm;
    Arm->dropAllReferences();
    Arm->eraseFromParent();
  }
  return true;
}

} // namespace

PreservedAnalyses revenant::UnswitchPass::run(Function &F,
                                              FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.empty())
    return PreservedAnalyses::all();
  bool Changed = false;

  // Pass A: table lookups (need the join phi intact). One lookup per
  // iteration; loop again for functions with several tables.
  for (int Iter = 0; Iter < 16; ++Iter) {
    bool More = false;
    for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
      auto Shape = matchLookup(&BB);
      if (Shape && buildSwitchFromTable(*Shape)) {
        More = true;
        Changed = true;
        break;
      }
    }
    if (!More)
      break;
  }

  // Pass B: linear maps and bitmaps (single-block selects).
  for (int Iter = 0; Iter < 16; ++Iter) {
    bool More = false;
    for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
      if (tryLinearMap(F, BB) || tryBitmap(F, BB)) {
        More = true;
        Changed = true;
        break;
      }
    }
    if (!More)
      break;
  }

  // Pass C: range-to-icmp branches.
  for (int Iter = 0; Iter < 16; ++Iter) {
    bool More = false;
    for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
      if (tryRangeToSwitch(F, BB)) {
        More = true;
        Changed = true;
        break;
      }
    }
    if (!More)
      break;
  }

  // Pass D: select chains. The compare-tree merge comes first: it folds
  // both arms into one switch, so the per-block chain rewrite never sees
  // a tree it would split in two.
  for (int Iter = 0; Iter < 16; ++Iter) {
    bool More = false;
    for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
      if (trySelectTree(F, BB) || trySelectToSwitch(F, BB)) {
        More = true;
        Changed = true;
        break;
      }
    }
    if (!More)
      break;
  }

  // Pass E: switch-of-select.
  for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
    if (trySwitchOfSelect(F, BB)) {
      Changed = true;
    }
  }

  // Pass F: default icmp/select merges (needs switches from A-E first).
  for (int Iter = 0; Iter < 16; ++Iter) {
    bool More = false;
    for (BasicBlock &BB : llvm::make_early_inc_range(F)) {
      if (tryMergeDefault(F, BB)) {
        More = true;
        Changed = true;
        break;
      }
    }
    if (!More)
      break;
  }

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-unswitch` name in Pipeline/Register.cpp, so the entry point is
// compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
#ifdef REVENANT_BUILD_PLUGIN
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnswitch", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unswitch") {
                    FPM.addPass(revenant::UnswitchPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
