//===- RerollFull.cpp - Full/partial unroll re-rolling ---------*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See RerollFull.h for the contract. Shape details:
//
// A window is M consecutive non-terminator instructions. Candidate windows
// repeat with identical opcodes and operand counts (the same structural
// check `trySynthesize` uses). Within a window exactly one instruction is
// the *anchor*: a store whose address operand is a GEP on one shared base
// with a constant byte offset. The anchor addresses across windows must be
// `base + k*Stride` for a uniform `Stride` (4 bytes for the i32 `arrays`
// case). The anchor's stored value must be an affine `c*k + d` over the
// window index through one shared leaf:
//
//   - `mul leaf, C` / `shl leaf, S` (strength-reduced `k * idx`), where the
//     per-window constant `C`/`(1<<S)` itself is affine in `k` with the
//     uniform step (`mul idx,3`, `mul idx,5`, ... => step 1; the constant
//     `0` window is `c*0 + d`);
//   - `add/shl/mul` chains of the above with constant folding through;
//   - a bare leaf read (the `k = 0` lane stores the unscaled factor).
//
// Non-anchor instructions in the window must be identical across windows
// (same opcode, same non-constant operands) or pure address math consumed
// within the window; any call, load of the array base, or cross-window use
// other than the anchor value vetoes the run.
//
// Evidence: every window must carry `!revenant.va` provenance from one
// shared VA range with a uniform per-window VA stride (see the header).
// Windows without any VA metadata, or with VAs outside one range, veto.
//
// The rewrite builds the same counted loop `trySynthesize` builds (counter
// phi, latch, exit), but the window-0 value operand is rewired to
// `Step*i + Base` computed from the counter instead of `Delta*i + c0`.
// Live-out handling, provenance copying, and block splitting mirror the
// existing synth.
//
//===----------------------------------------------------------------------===//
#include "revenant/Transforms/Reroll/RerollFull.h"

// Metadata evidence (MuseDeoptEvidenceR): when the Evidence header has
// landed, its presence check confirms short runs; the structural matcher
// proves N either way. Shape wins disagreements; evidence only lowers the
// worth-it threshold. Included live (never vendored) once staging carries
// it; until then the VA-range fallback below is today's behavior.
#if __has_include("revenant/Evidence/Evidence.h")
#include "revenant/Evidence/Evidence.h"
#endif
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

#include <cstdint>
#include <optional>
#include <vector>
using namespace llvm;

namespace revenant {
namespace {

// Same provenance kind spelling as Reroll.cpp (RvIRContract owns it).
constexpr char VA_MD[] = "revenant.va";

// Minimum windows before a loop is worth synthesizing.
constexpr unsigned MinWindows = 3;

// Copy !revenant.va from Src to Dst so provenance survives the rewrite.
void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
}

// First !revenant.va value on I, if any.
std::optional<uint64_t> vaOf(const Instruction &I) {
  MDNode *N = I.getMetadata(VA_MD);
  if (!N || N->getNumOperands() != 1)
    return std::nullopt;
  auto *CAM = dyn_cast<ConstantAsMetadata>(N->getOperand(0));
  auto *C = CAM ? dyn_cast<ConstantInt>(CAM->getValue()) : nullptr;
  if (!C)
    return std::nullopt;
  return C->getZExtValue();
}

// Signed i64 extraction for affine constants (same rule as Reroll.cpp:
// i128 SIMD masks outside signed 64 veto rather than narrow).
std::optional<int64_t> signedConst(const ConstantInt *C) {
  if (!C->getValue().isSignedIntN(64))
    return std::nullopt;
  return C->getSExtValue();
}

// Shape key: opcode only. Type/operand checks happen per window pair.
unsigned shapeOf(const Instruction &I) { return I.getOpcode(); }

// Unfold V through pure in-block defs into (leaf, scale) with
// V = leaf * scale + 0, or (nullptr, 0, c) for a pure constant c.
// Strength-reduced unroll lanes are each `leaf * laneScale` (mul/shl by a
// lane constant, chains of those, casts through); the lane scales differ
// per lane (0, 1, 2, 3, ...) so the per-lane scale IS the affine evidence.
// Returns nullopt when V is not leaf-times-constant: more than one leaf,
// a non-affine op (div/rem/cmp/select), an add of a nonzero constant, or
// a load/call/phi/gep def.
struct LaneValue {
  Value *Leaf = nullptr; // nullptr => pure constant
  int64_t Scale = 0;     // leaf multiplier (1 for a bare leaf read)
  int64_t Const = 0;     // constant value when Leaf == nullptr
};

std::optional<LaneValue> laneValueOf(Value *V,
                                     const DenseMap<Value *, Instruction *> &Def,
                                     unsigned Depth = 0) {
  if (Depth > 8)
    return std::nullopt;
  if (auto *C = dyn_cast<ConstantInt>(V)) {
    auto S = signedConst(C);
    if (!S)
      return std::nullopt;
    return LaneValue{nullptr, 0, *S};
  }
  if (isa<Constant>(V))
    return std::nullopt;
  auto It = Def.find(V);
  if (It == Def.end())
    return LaneValue{V, 1, 0}; // leaf: function arg, factor, base pointer
  Instruction *DI = It->second;
  auto Op = [&](unsigned K) { return laneValueOf(DI->getOperand(K), Def, Depth + 1); };
  switch (DI->getOpcode()) {
  case Instruction::Mul: {
    if (DI->getNumOperands() != 2)
      return std::nullopt;
    auto A = Op(0), B = Op(1);
    if (!A || !B)
      return std::nullopt;
    if (!A->Leaf && B->Leaf)
      return LaneValue{B->Leaf, B->Scale * A->Const, 0};
    if (A->Leaf && !B->Leaf)
      return LaneValue{A->Leaf, A->Scale * B->Const, 0};
    if (!A->Leaf && !B->Leaf)
      return LaneValue{nullptr, 0, A->Const * B->Const};
    return std::nullopt; // leaf * leaf: not affine
  }
  case Instruction::Shl: {
    if (DI->getNumOperands() != 2)
      return std::nullopt;
    auto *C = dyn_cast<ConstantInt>(DI->getOperand(1));
    if (!C)
      return std::nullopt;
    auto S = signedConst(C);
    if (!S || *S < 0 || *S > 60)
      return std::nullopt;
    auto A = laneValueOf(DI->getOperand(0), Def, Depth + 1);
    if (!A || !A->Leaf)
      return std::nullopt;
    return LaneValue{A->Leaf, A->Scale * (int64_t(1) << *S), 0};
  }
  case Instruction::Add:
  case Instruction::Sub: {
    // Lane values are pure scaled leaves; an add/sub of two scaled lanes
    // of the same leaf stays a scaled lane only when one side is zero.
    if (DI->getNumOperands() != 2)
      return std::nullopt;
    auto A = Op(0), B = Op(1);
    if (!A || !B)
      return std::nullopt;
    bool IsAdd = DI->getOpcode() == Instruction::Add;
    if (A->Leaf && B->Leaf) {
      if (A->Leaf != B->Leaf)
        return std::nullopt;
      return LaneValue{A->Leaf,
                       IsAdd ? A->Scale + B->Scale : A->Scale - B->Scale, 0};
    }
    if (A->Leaf && !B->Leaf) {
      if (B->Const != 0)
        return std::nullopt;
      return LaneValue{A->Leaf, A->Scale, 0};
    }
    if (!A->Leaf && B->Leaf) {
      if (!IsAdd || A->Const != 0)
        return std::nullopt;
      return LaneValue{B->Leaf, B->Scale, 0};
    }
    return LaneValue{nullptr, 0,
                     IsAdd ? A->Const + B->Const : A->Const - B->Const};
  }
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::BitCast:
    if (DI->getNumOperands() != 1)
      return std::nullopt;
    return Op(0);
  default:
    return std::nullopt;
  }
}

// The stored value of an anchor store as a lane value.
std::optional<LaneValue> anchorValue(StoreInst *S,
                                     const DenseMap<Value *, Instruction *> &Def) {
  Value *V = S->getValueOperand();
  if (isa<Constant>(V)) {
    auto *C = dyn_cast<ConstantInt>(V);
    if (!C)
      return std::nullopt;
    auto Sv = signedConst(C);
    if (!Sv)
      return std::nullopt;
    return LaneValue{nullptr, 0, *Sv};
  }
  if (!Def.count(V))
    return LaneValue{V, 1, 0}; // bare leaf (k = 0 lane)
  return laneValueOf(V, Def);
}

struct IndexedScalarLoad {
  LoadInst *Load;
  Value *Base;
  int64_t Index;
};

struct ScalarPointerInfo {
  Value *Base = nullptr;
  int64_t Index = 0;
  SmallVector<GetElementPtrInst *, 8> GEPs;
};

// Reduce a scalar load address to a byte offset from one in-block GEP base.
// The byte GEP chains emitted by StraightDevec and ordinary typed GEPs both
// use this form; struct and dynamic-index GEPs conservatively veto the match.
bool scalarPointerInfo(LoadInst *L, BasicBlock &B, ScalarPointerInfo &Info) {
  Type *LoadTy = L->getType();
  if (!LoadTy->isIntegerTy())
    return false;
  unsigned LoadBits = LoadTy->getIntegerBitWidth();
  if (LoadBits < 8 || LoadBits % 8 != 0)
    return false;
  uint64_t ElemBytes = LoadBits / 8;

  int64_t ByteOffset = 0;
  Value *Ptr = L->getPointerOperand();
  while (auto *G = dyn_cast<GetElementPtrInst>(Ptr)) {
    if (G->getParent() != &B || G->getNumIndices() != 1)
      return false;
    Type *SourceTy = G->getSourceElementType();
    if (!SourceTy->isIntegerTy() && !isa<FixedVectorType>(SourceTy))
      return false;
    TypeSize Bits = SourceTy->getPrimitiveSizeInBits();
    if (Bits.isScalable() || Bits.getFixedValue() % 8 != 0)
      return false;
    uint64_t Scale = Bits.getFixedValue() / 8;
    if (Scale == 0 || Scale > static_cast<uint64_t>(INT64_MAX))
      return false;
    auto *C = dyn_cast<ConstantInt>(G->getOperand(1));
    auto Index = C ? signedConst(C) : std::nullopt;
    if (!Index || *Index < 0 ||
        static_cast<uint64_t>(*Index) >
            static_cast<uint64_t>(INT64_MAX) / Scale)
      return false;
    int64_t Delta = *Index * static_cast<int64_t>(Scale);
    if (Delta > INT64_MAX - ByteOffset)
      return false;
    ByteOffset += Delta;
    Info.GEPs.push_back(G);
    Ptr = G->getPointerOperand();
  }
  if (!Ptr->getType()->isPointerTy() ||
      ByteOffset % static_cast<int64_t>(ElemBytes) != 0)
    return false;
  Info.Base = Ptr;
  Info.Index = ByteOffset / static_cast<int64_t>(ElemBytes);
  return true;
}

// Rebuild a fixed-trip scalar sum as one source-style loop. The proof is
// deliberately narrow: one block, contiguous nonvolatile scalar loads,
// a single-use add tree, and monotonic !revenant.va evidence. No operation
// outside the load/address/add tree may be present.
unsigned tryRerollScalarSum(Function &F, BasicBlock &B,
                            ArrayRef<Instruction *> Insns) {
  if (F.size() != 1 || &B != &F.getEntryBlock())
    return 0;
  auto *Ret = dyn_cast<ReturnInst>(B.getTerminator());
  if (!Ret || !Ret->getReturnValue())
    return 0;

  SmallVector<Value *, 4> CandidateBases;
  for (Instruction *I : Insns) {
    auto *L = dyn_cast<LoadInst>(I);
    if (!L || L->isVolatile() || L->isAtomic())
      continue;
    ScalarPointerInfo PI;
    if (!scalarPointerInfo(L, B, PI) ||
        llvm::is_contained(CandidateBases, PI.Base))
      continue;
    CandidateBases.push_back(PI.Base);
  }

  for (Value *CandidateBase : CandidateBases) {
    SmallVector<IndexedScalarLoad, 32> Loads;
    SmallPtrSet<GetElementPtrInst *, 32> GEPs;
    for (Instruction *I : Insns) {
      auto *L = dyn_cast<LoadInst>(I);
      if (!L || L->isVolatile() || L->isAtomic())
        continue;
      ScalarPointerInfo PI;
      if (!scalarPointerInfo(L, B, PI) || PI.Base != CandidateBase)
        continue;
      Loads.push_back({L, PI.Base, PI.Index});
      for (GetElementPtrInst *G : PI.GEPs)
        GEPs.insert(G);
    }
    if (Loads.size() < 4 || Loads.size() > 64)
      continue;
    Type *LoadTy = Loads.front().Load->getType();
    Align LoadAlign = Loads.front().Load->getAlign();
    if (!llvm::all_of(Loads, [&](const auto &L) {
          return L.Load->getType() == LoadTy;
        }))
      continue;
    for (const IndexedScalarLoad &L : Loads)
      if (L.Load->getAlign().value() < LoadAlign.value())
        LoadAlign = L.Load->getAlign();
    llvm::sort(Loads, [](const auto &A, const auto &C) {
      return A.Index < C.Index;
    });
    bool Contiguous = true;
    for (size_t K = 0; K < Loads.size(); ++K)
      Contiguous &= Loads[K].Index == static_cast<int64_t>(K);
    if (!Contiguous)
      continue;

    SmallVector<BinaryOperator *, 32> Adds;
    bool InvalidAdd = false;
    for (Instruction *I : Insns) {
      auto *BO = dyn_cast<BinaryOperator>(I);
      if (!BO || BO->getOpcode() != Instruction::Add)
        continue;
      if (BO->getType() != LoadTy || BO->hasNoSignedWrap() ||
          BO->hasNoUnsignedWrap()) {
        InvalidAdd = true;
        break;
      }
      Adds.push_back(BO);
    }
    if (InvalidAdd || Adds.size() != Loads.size() - 1)
      continue;
    auto *Root = dyn_cast<BinaryOperator>(Ret->getReturnValue());
    if (!Root || !llvm::is_contained(Adds, Root))
      continue;

    SmallPtrSet<LoadInst *, 32> LoadSet;
    SmallPtrSet<BinaryOperator *, 32> AddSet;
    for (const IndexedScalarLoad &L : Loads)
      LoadSet.insert(L.Load);
    for (BinaryOperator *BO : Adds)
      AddSet.insert(BO);

    SmallPtrSet<Instruction *, 32> Allowed;
    for (const IndexedScalarLoad &L : Loads)
      Allowed.insert(L.Load);
    for (GetElementPtrInst *G : GEPs)
      Allowed.insert(G);
    for (BinaryOperator *BO : Adds)
      Allowed.insert(BO);
    if (llvm::any_of(Insns, [&](Instruction *I) {
          return !Allowed.count(I);
        }))
      continue;

    bool UsesOK = true;
    for (const IndexedScalarLoad &L : Loads) {
      if (!L.Load->hasOneUse()) {
        UsesOK = false;
        break;
      }
      auto *U = dyn_cast<BinaryOperator>(*L.Load->user_begin());
      if (!U || !AddSet.count(U))
        UsesOK = false;
    }
    for (GetElementPtrInst *G : GEPs) {
      for (User *U : G->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || (!LoadSet.count(dyn_cast<LoadInst>(UI)) &&
                    !GEPs.count(dyn_cast<GetElementPtrInst>(UI))))
          UsesOK = false;
      }
    }
    for (BinaryOperator *BO : Adds) {
      if (!BO->hasOneUse()) {
        UsesOK = false;
        break;
      }
      User *U = *BO->user_begin();
      if (BO == Root ? U != Ret : !AddSet.count(dyn_cast<BinaryOperator>(U)))
        UsesOK = false;
    }
    if (!UsesOK)
      continue;

    SmallPtrSet<LoadInst *, 32> SeenLoads;
    SmallPtrSet<BinaryOperator *, 32> SeenAdds;
    SmallVector<Value *, 64> Pending{Root};
    bool TreeOK = true;
    while (!Pending.empty() && TreeOK) {
      Value *V = Pending.pop_back_val();
      if (auto *L = dyn_cast<LoadInst>(V)) {
        if (!LoadSet.count(L) || SeenLoads.count(L)) {
          TreeOK = false;
          break;
        }
        SeenLoads.insert(L);
        continue;
      }
      auto *BO = dyn_cast<BinaryOperator>(V);
      if (!BO || !AddSet.count(BO) || SeenAdds.count(BO)) {
        TreeOK = false;
        break;
      }
      SeenAdds.insert(BO);
      Pending.push_back(BO->getOperand(0));
      Pending.push_back(BO->getOperand(1));
    }
    if (!TreeOK || SeenLoads.size() != Loads.size() ||
        SeenAdds.size() != Adds.size())
      continue;

    Type *IndexTy = nullptr;
    bool IndexTypesOK = true;
    for (GetElementPtrInst *G : GEPs) {
      Type *Ty = G->getOperand(1)->getType();
      if (!Ty->isIntegerTy() || (IndexTy && IndexTy != Ty)) {
        IndexTypesOK = false;
        break;
      }
      IndexTy = Ty;
    }
    if (!IndexTypesOK || !IndexTy)
      continue;

    std::optional<uint64_t> FirstVA, PreviousVA;
    Instruction *VAAnchor = nullptr;
    bool EvidenceOK = true;
    for (BinaryOperator *BO : Adds) {
      auto VA = vaOf(*BO);
      if (!VA || (PreviousVA && *VA < *PreviousVA) ||
          (FirstVA && *VA - *FirstVA > 512)) {
        EvidenceOK = false;
        break;
      }
      if (!FirstVA)
        FirstVA = VA;
      if (!VAAnchor)
        VAAnchor = BO;
      PreviousVA = VA;
    }
    if (!EvidenceOK || !VAAnchor)
      continue;

    BasicBlock *Preheader = &B;
    BasicBlock *Exit =
        Preheader->splitBasicBlock(Preheader->getTerminator(), "unroll.exit");
    auto *PreheaderBranch = cast<BranchInst>(Preheader->getTerminator());
    BasicBlock *Loop =
        BasicBlock::Create(F.getContext(), "unroll", &F, Exit);
    PreheaderBranch->setSuccessor(0, Loop);

    IRBuilder<> PHBuilder(Loop, Loop->begin());
    auto *Index = PHBuilder.CreatePHI(IndexTy, 2, "u0i");
    Index->addIncoming(ConstantInt::get(IndexTy, 0), Preheader);
    auto *Acc = PHBuilder.CreatePHI(LoadTy, 2, "u0acc");
    Acc->addIncoming(ConstantInt::get(LoadTy, 0), Preheader);
    IRBuilder<> BodyBuilder(Loop);
    Value *Addr =
        BodyBuilder.CreateGEP(LoadTy, CandidateBase, Index, "u0addr");
    auto *Load = BodyBuilder.CreateLoad(LoadTy, Addr, "u0v");
    Load->setAlignment(LoadAlign);
    Value *Sum = BodyBuilder.CreateAdd(Acc, Load, "u0next");
    Value *Next = BodyBuilder.CreateAdd(
        Index, ConstantInt::get(IndexTy, 1), "u0inext");
    Value *Cond = BodyBuilder.CreateICmpULT(
        Next, ConstantInt::get(IndexTy, Loads.size()), "u0c");
    BranchInst *Latch = BodyBuilder.CreateCondBr(Cond, Loop, Exit);
    Index->addIncoming(Next, Loop);
    Acc->addIncoming(Sum, Loop);

    copyVA(VAAnchor, Index);
    copyVA(VAAnchor, Acc);
    copyVA(VAAnchor, cast<Instruction>(Addr));
    copyVA(VAAnchor, Load);
    copyVA(VAAnchor, cast<Instruction>(Sum));
    copyVA(VAAnchor, cast<Instruction>(Next));
    copyVA(VAAnchor, cast<Instruction>(Cond));
    copyVA(VAAnchor, Latch);
    Ret->setOperand(0, Sum);

    for (auto It = Insns.rbegin(); It != Insns.rend(); ++It)
      if ((*It)->getParent() == Preheader)
        (*It)->eraseFromParent();
    return 1;
  }
  return 0;
}

} // namespace

unsigned tryRerollFull(Function &F, BasicBlock &B,
                       SmallVectorImpl<StringRef> &Notes) {
  SmallVector<Instruction *, 64> Insns;
  for (Instruction &I : B) {
    if (isa<PHINode>(&I))
      return 0; // PHIs cannot move into the synthesized body
    if (I.isTerminator())
      continue;
    Insns.push_back(&I);
  }
  size_t N = Insns.size();
  // Four constant stores need only three address GEPs; other patterns
  // are filtered by their anchor and lane checks below.
  if (tryRerollScalarSum(F, B, Insns)) {
    Notes.push_back("fixed-trip scalar sum");
    return 1;
  }
  if (N < 7)
    return 0;

  // In-window def lookup.
  DenseMap<Value *, Instruction *> Def;
  for (Instruction *I : Insns)
    if (!I->getType()->isVoidTy())
      Def[I] = I;

  // Anchor candidates: stores whose address is a GEP on a shared base
  // with a constant byte offset, or a direct base+offset GEP chain of
  // depth 1. Record (store pos, base, byte offset).
  struct Anchor {
    size_t Pos;
    Value *Base;
    int64_t Off;
  };
  SmallVector<Anchor, 16> Anchors;
  for (size_t p = 0; p < N; ++p) {
    auto *S = dyn_cast<StoreInst>(Insns[p]);
    if (!S)
      continue;
    Value *Addr = S->getPointerOperand();
    auto *G = dyn_cast<GetElementPtrInst>(Addr);
    if (!G) {
      // A direct store through the shared base is the zero-offset lane.
      if (Addr->getType()->isPointerTy() &&
          !isa<Instruction>(Addr) && isa<Constant>(S->getValueOperand()))
        Anchors.push_back({p, Addr, 0});
      continue;
    }
    if (G->getParent() != &B || G->getNumIndices() != 1)
      continue;
    auto *C = dyn_cast<ConstantInt>(G->getOperand(1));
    if (!C)
      continue;
    auto Off = signedConst(C);
    if (!Off)
      continue;
    // Byte scale of the GEP element type.
    Type *Elem = G->getResultElementType();
    if (!Elem->isSized())
      continue;
    int64_t Scale = (int64_t)Elem->getPrimitiveSizeInBits() / 8;
    if (Scale <= 0)
      Scale = 1;
    Anchors.push_back({p, G->getPointerOperand(), *Off * Scale});
  }
  if (Anchors.size() < MinWindows)
    return 0;

  // Try each base with >= MinWindows anchors as one run. Lanes may have
  // different widths (the k = 0 lane of `arrays` is a bare store, later
  // lanes carry one value def: mul/shl): lane j owns [laneStart(j),
  // anchorPos(j)], where laneStart(0) is the first anchor's run Start and
  // laneStart(j) is anchorPos(j-1) + 1. The anchor value defs of a lane
  // are the pure (mul/shl/add/sub/cast) instructions in its range that
  // feed the stored value; everything else in the range must be the
  // address GEP, the anchor store, or dead prologue shared by lane 0.
  DenseMap<Value *, SmallVector<size_t, 8>> ByBase;
  for (size_t i = 0; i < Anchors.size(); ++i)
    ByBase[Anchors[i].Base].push_back(i);
  auto TryConstantFill = [&](const SmallVector<size_t, 8> &Idx, Value *Base,
                             int64_t Stride) {
    const size_t K = Idx.size();
    if (K < 4 || !Base->getType()->isPointerTy())
      return false;
    auto *FirstStore =
        dyn_cast<StoreInst>(Insns[Anchors[Idx.front()].Pos]);
    auto *Fill = FirstStore
                     ? dyn_cast<Constant>(FirstStore->getValueOperand())
                     : nullptr;
    if (!FirstStore || !Fill || Fill->getType()->isVectorTy() ||
        !Fill->getType()->isSized() || FirstStore->isVolatile() ||
        FirstStore->isAtomic())
      return false;
    int64_t StoreBytes =
        (int64_t)Fill->getType()->getPrimitiveSizeInBits() / 8;
    if (StoreBytes <= 0 || Stride < StoreBytes)
      return false;

    Align StoreAlign = FirstStore->getAlign();
    SmallPtrSet<Instruction *, 16> Allowed;
    SmallVector<StoreInst *, 8> Stores;
    SmallVector<GetElementPtrInst *, 8> AddressGEPs;
    uint64_t FirstVA = 0, PrevVA = 0;
    for (size_t J = 0; J < K; ++J) {
      auto *S = dyn_cast<StoreInst>(Insns[Anchors[Idx[J]].Pos]);
      if (!S || S->getValueOperand() != Fill || S->isVolatile() ||
          S->isAtomic() || S->getAlign() != StoreAlign)
        return false;
      auto VA = vaOf(*S);
      if (!VA || (J && *VA <= PrevVA))
        return false;
      if (!J)
        FirstVA = *VA;
      PrevVA = *VA;

      Stores.push_back(S);
      Allowed.insert(S);
      Value *Addr = S->getPointerOperand();
      if (Addr == Base) {
        if (Anchors[Idx[J]].Off != 0)
          return false;
        continue;
      }
      auto *G = dyn_cast<GetElementPtrInst>(Addr);
      if (!G || G->getPointerOperand() != Base || G->getNumIndices() != 1 ||
          !G->hasOneUse() || *G->user_begin() != S)
        return false;
      AddressGEPs.push_back(G);
      Allowed.insert(G);
    }
    if (PrevVA - FirstVA > 512)
      return false;

    size_t Start = Anchors[Idx.front()].Pos;
    size_t RunEnd = Anchors[Idx.back()].Pos + 1;
    for (size_t I = Start; I < RunEnd; ++I)
      if (!Allowed.count(Insns[I]))
        return false;

    BasicBlock *Pre = &B;
    BasicBlock *Exit = RunEnd < N
                           ? Pre->splitBasicBlock(Insns[RunEnd], "unroll.exit")
                           : Pre->splitBasicBlock(Pre->getTerminator(),
                                                  "unroll.exit");
    BasicBlock *Loop = Pre->splitBasicBlock(Insns[Start], "unroll");
    Instruction *OldTerm = Loop->getTerminator();
    Type *I64 = Type::getInt64Ty(F.getContext());
    IRBuilder<> PB(Loop, Loop->begin());
    PHINode *Index = PB.CreatePHI(I64, 2, "u0i");
    Index->addIncoming(ConstantInt::get(I64, 0), Pre);
    copyVA(FirstStore, Index);

    IRBuilder<> LB(OldTerm);
    Value *Offset = LB.CreateMul(Index, ConstantInt::get(I64, Stride), "u0a");
    if (Anchors[Idx.front()].Off != 0)
      Offset = LB.CreateAdd(
          Offset, ConstantInt::get(I64, Anchors[Idx.front()].Off), "u0a");
    Value *Address =
        LB.CreateGEP(Type::getInt8Ty(F.getContext()), Base, Offset, "u0addr");
    StoreInst *LoopStore = LB.CreateStore(Fill, Address);
    LoopStore->setAlignment(StoreAlign);
    copyVA(FirstStore, LoopStore);
    Value *Next = LB.CreateAdd(Index, ConstantInt::get(I64, 1), "u0inext");
    Value *Cond =
        LB.CreateICmpULT(Next, ConstantInt::get(I64, K), "u0c");
    copyVA(FirstStore, cast<Instruction>(Next));
    copyVA(FirstStore, cast<Instruction>(Cond));
    Index->addIncoming(Next, Loop);
    BranchInst *Latch = LB.CreateCondBr(Cond, Loop, Exit);
    copyVA(Stores.back(), Latch);
    OldTerm->eraseFromParent();

    for (StoreInst *S : Stores)
      S->eraseFromParent();
    for (GetElementPtrInst *G : AddressGEPs)
      G->eraseFromParent();
    return true;
  };
  for (auto &KV : ByBase) {
    auto &All = KV.second;
    if (All.size() < MinWindows)
      continue;
    std::sort(All.begin(), All.end(), [&](size_t A, size_t B) {
      return Anchors[A].Pos < Anchors[B].Pos;
    });
    // Contiguous sub-runs: a later lane may be split by cross traffic
    // (corpus arrays lane 7's value def and store bracket the table
    // load), so every slice [s, e] with >= MinWindows anchors is a
    // candidate. Longest first.
    for (size_t Len = All.size(); Len >= MinWindows; --Len) {
      for (size_t s = 0; s + Len <= All.size(); ++s) {
        SmallVector<size_t, 8> Idx;
        for (size_t q = s; q < s + Len; ++q)
          Idx.push_back(All[q]);
        size_t K = Idx.size();
    int64_t Stride = Anchors[Idx[1]].Off - Anchors[Idx[0]].Off;
    if (Stride <= 0 || Stride > 64)
      continue;
    bool AddrOK = true;
    for (size_t j = 0; j < K; ++j)
      if (Anchors[Idx[j]].Off != Anchors[Idx[0]].Off + (int64_t)j * Stride)
        AddrOK = false;
    if (!AddrOK)
      continue;
    if (TryConstantFill(Idx, KV.first, Stride))
      return 1;
    // Lane ranges must tile contiguously from the first lane's value-def
    // start: lane 0 starts at its value def (or its address GEP when the
    // value is a bare leaf/constant), later lanes start right after the
    // previous anchor store.
    size_t Start = Anchors[Idx[0]].Pos;
    {
      auto *S0 = cast<StoreInst>(Insns[Anchors[Idx[0]].Pos]);
      Value *V0 = S0->getValueOperand();
      // Lane 0 starts at its value-def chain when the stored value is
      // computed in-block, else at its address GEP when that directly
      // precedes the store (bare leaf/constant lane), else at the store.
      if (auto *D0 = dyn_cast<Instruction>(V0)) {
        if (Def.count(V0) && Def[V0]->getParent() == &B) {
          size_t s = Anchors[Idx[0]].Pos;
          while (s > 0) {
            Instruction *Prev = Insns[s - 1];
            bool Feeds = false;
            for (size_t q = s; q <= Anchors[Idx[0]].Pos; ++q) {
              for (Use &U : Insns[q]->operands())
                if (U.get() == Prev)
                  Feeds = true;
            }
            if (!Feeds)
              break;
            if (Prev->getOpcode() != Instruction::Mul &&
                Prev->getOpcode() != Instruction::Shl &&
                Prev->getOpcode() != Instruction::Add &&
                Prev->getOpcode() != Instruction::Sub &&
                !isa<CastInst>(Prev))
              break;
            s--;
            (void)D0;
          }
          Start = s;
        }
      }
      if (Start == Anchors[Idx[0]].Pos) {
        if (Anchors[Idx[0]].Pos > 0 &&
            Insns[Anchors[Idx[0]].Pos - 1] == S0->getPointerOperand())
          Start = Anchors[Idx[0]].Pos - 1;
      }
    }
    bool Tiled = true;
    for (size_t j = 1; j < K; ++j)
      if (Anchors[Idx[j - 1]].Pos + 1 > Anchors[Idx[j]].Pos)
        Tiled = false;
    // Tank classification: instructions inside lane ranges that neither
    // feed the anchor (transitively) nor are the anchor/address GEP.
    // Loop-independent, side-effect-free Tank (no calls/stores/volatile,
    // operands all loop-invariant, i.e. not Lane defs) is sunk after the
    // loop at build time; anything else vetoes. Corpus arrays lane 7
    // brackets the table load (`and`/global-GEP/double-load feeding the
    // return) between its value def and store.
    auto FeedsAnchor = [&](size_t j) {
      SmallPtrSet<Value *, 8> Feed;
      SmallVector<Value *, 8> Stack;
      auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
      Feed.insert(S->getValueOperand());
      Stack.push_back(S->getValueOperand());
      while (!Stack.empty()) {
        Value *V = Stack.pop_back_val();
        if (auto *D = dyn_cast<Instruction>(V)) {
          if (!Def.count(V) || Def[V]->getParent() != &B)
            continue;
          for (Use &U : D->operands()) {
            if (Feed.insert(U.get()).second)
              Stack.push_back(U.get());
          }
        }
      }
      return Feed;
    };
    // no value def, so leading lanes may be narrower. Find the first full
    // lane First (containing a computed value); all lanes j >= First
    // share width M, and all lanes j < First are narrow prologue (each
    // holds only its address GEP and anchor store). First must leave
    // >= MinWindows full lanes.
    size_t First = K;
    for (size_t j = 0; j < K; ++j) {
      auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
      Value *V = S->getValueOperand();
      // Full lane: the value is computed by an instruction inside this
      // lane's own range (a bare leaf read from prologue, like lane 1's
      // %t, does not make a full lane).
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      if (auto *DI = dyn_cast<Instruction>(V)) {
        bool InLane = false;
        for (size_t i = LB0; i < LE0; ++i)
          if (Insns[i] == DI)
            InLane = true;
        if (InLane && Def.count(V) && Def[V]->getParent() == &B) {
          (void)DI;
          First = j;
          break;
        }
      }
    }
    if (First >= K || First + 1 >= K)
      continue;
    // Worth-it threshold: K - First >= 4 runs on structure alone; a
    // 3-lane run additionally needs metadata evidence. Shape proves N;
    // evidence only confirms short runs. Never gate the count on D == N:
    // asm windows and IR windows count different things.
    if (K - First < MinWindows)
      continue;
#if __has_include("revenant/Evidence/Evidence.h")
    if (K - First == MinWindows) {
      auto Factor = revenant::evidence::unrollFactor(F);
      if (!Factor)
        continue;
    }
#endif
    // Tank pass: classify every in-range instruction. Lane = anchor,
    // address GEP, or transitive anchor feeder. Tank = side-effect-free
    // (no call/store/invoke/volatile), loop-invariant (no operand is a
    // Lane def of any lane), and not feeding any anchor. Anything else
    // vetoes. Tank is sunk after the loop at build time.
    SmallPtrSet<Instruction *, 16> Tank;
    bool TankOK = true;
    // Lane-feeder sets per lane, plus the union of all Lane defs (an
    // instruction feeding lane j's anchor is Lane even if textually
    // inside another lane's range, which cannot happen for tiled lanes
    // but is checked anyway).
    std::vector<SmallPtrSet<Value *, 8>> FeedSets;
    for (size_t j = 0; j < K; ++j)
      FeedSets.push_back(FeedsAnchor(j));
    SmallPtrSet<Instruction *, 32> LaneDefs;
    for (size_t j = 0; j < K; ++j) {
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
      for (size_t i = LB0; i < LE0; ++i) {
        Instruction *I = Insns[i];
        if (I == S || I == S->getPointerOperand() || FeedSets[j].count(I)) {
          LaneDefs.insert(I);
        }
      }
    }
    for (size_t j = 0; j < K && TankOK; ++j) {
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      for (size_t i = LB0; i < LE0 && TankOK; ++i) {
        Instruction *I = Insns[i];
        if (LaneDefs.count(I))
          continue;
        // Candidate Tank: pure, loop-invariant, anchor-independent.
        if (I->mayHaveSideEffects() || isa<CallInst>(I) || isa<InvokeInst>(I) ||
            isa<StoreInst>(I) || isa<LoadInst>(I) || isa<CmpInst>(I) ||
            isa<BranchInst>(I) || isa<SwitchInst>(I) || isa<PHINode>(I)) {
          // Loads are Tank only when loop-invariant and alias-free vs the
          // loop's stores (globals/constants); compare/branch never Tank.
          if (auto *L = dyn_cast<LoadInst>(I)) {
            bool Invariant = true;
            for (Use &U : L->operands())
              if (LaneDefs.count(dyn_cast<Instruction>(U.get())) ||
                  U.get() == KV.first)
                Invariant = false;
            if (Invariant &&
                (L->getPointerOperand() == L->getPointerOperand())) {
              // Loop-invariant address; alias check below covers stores.
            } else {
              TankOK = false;
              break;
            }
          } else {
            TankOK = false;
            break;
          }
        }
        for (Use &U : I->operands())
          if (LaneDefs.count(dyn_cast<Instruction>(U.get()))) {
            TankOK = false;
            break;
          }
        if (TankOK)
          Tank.insert(I);
      }
    }
    if (!TankOK)
      continue;
    // Tank must not alias the loop's stores: every Tank load must read
    // outside the stored base's object. Anchors store through KV.first;
    // a Tank load through any other root is alias-free by construction
    // (different alloca/global). Loads through KV.first vetoed above.
    // Lane widths count Lane insns only (Tank excluded): full lanes
    // j >= First share Lane-width M.
    auto LaneWidth = [&](size_t j) -> size_t {
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      size_t W = 0;
      for (size_t i = LB0; i < LE0; ++i)
        if (!Tank.count(Insns[i]))
          ++W;
      return W;
    };
    bool WidthOK = true;
    for (size_t j = First + 1; j < K; ++j)
      if (LaneWidth(j) != LaneWidth(First))
        WidthOK = false;
    size_t M = LaneWidth(First);
    if (M < 2)
      WidthOK = false;
    if (!WidthOK)
      continue;
    size_t RunEnd = Anchors[Idx[K - 1]].Pos + 1;
    // Explicit per-lane Lane vectors (Tank excluded). Full lanes share
    // Lane-width M; each ends in its anchor store with the address GEP
    // second-to-last (value defs before it differ per lane by design).
    std::vector<std::vector<Instruction *>> Lanes;
    for (size_t j = 0; j < K; ++j) {
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      std::vector<Instruction *> L;
      for (size_t i = LB0; i < LE0; ++i)
        if (!Tank.count(Insns[i]))
          L.push_back(Insns[i]);
      Lanes.push_back(std::move(L));
    }
    // Structural check on full lanes j >= First: same Lane-width M, same
    // address GEP shape (same base operand), anchor store last.
    bool ShapeOK = true;
    for (size_t j = First; j < K && ShapeOK; ++j) {
      if (Lanes[j].size() != M) {
        ShapeOK = false;
        break;
      }
      auto *S = dyn_cast<StoreInst>(Lanes[j].back());
      auto *G = Lanes[j].size() >= 2
                    ? dyn_cast<GetElementPtrInst>(Lanes[j][M - 2])
                    : nullptr;
      if (!S || !G)
        ShapeOK = false;
      else if (G->getPointerOperand() != KV.first)
        ShapeOK = false;
    }
    if (!ShapeOK)
      continue;
    // Lane base helper: first instruction index of lane j (raw positions;
    // Tank members interleave but ranges still tile).
    size_t FirstFullBase = Anchors[Idx[First]].Pos - M + 1;
    auto LaneBase = [&](size_t j) -> size_t {
      if (j < First)
        return (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      return Anchors[Idx[j]].Pos - M + 1;
    };
    // Affine value check: lane scales are c*j + d over the lane index
    // over one shared leaf (arrays: 0, 1, 2, 3, 4, 5, 6 over %t; lane 0
    // stores constant 0, which is scale 0 of any leaf).
    SmallVector<LaneValue, 8> Vals;
    bool ValsOK = true;
    for (size_t j = 0; j < K; ++j) {
      auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
      auto V = anchorValue(S, Def);
      if (!V) {
        ValsOK = false;
        break;
      }
      Vals.push_back(*V);
    }
    if (!ValsOK)
      continue;
    Value *Leaf = nullptr;
    for (auto &V : Vals)
      if (V.Leaf) {
        Leaf = V.Leaf;
        break;
      }
    for (auto &V : Vals)
      if (V.Leaf && V.Leaf != Leaf) {
        ValsOK = false;
        break;
      }
    if (!ValsOK || !Leaf)
      continue;
    auto ScaleOf = [&](size_t j) -> int64_t {
      return Vals[j].Leaf ? Vals[j].Scale : 0;
    };
    int64_t Step = ScaleOf(1) - ScaleOf(0);
    bool StepOK = true;
    for (size_t j = 0; j < K; ++j)
      if (ScaleOf(j) != ScaleOf(0) + (int64_t)j * Step)
        StepOK = false;
    if (!StepOK)
      continue;
    // Identical lanes are not a loop.
    if (Step == 0)
      continue;

    // Reject vector work: no vector-typed instruction in the run.
    bool HasVec = false;
    for (size_t j = 0; j < K && !HasVec; ++j) {
      size_t LB0 = LaneBase(j), LE0 = Anchors[Idx[j]].Pos + 1;
      for (size_t i = LB0; i < LE0; ++i)
        if (Insns[i]->getType()->isVectorTy())
          HasVec = true;
    }
    if (HasVec)
      continue;

    // Reject calls inside the lanes (side effects the loop cannot carry).
    // A lane-internal load of another base is cross traffic. Traffic
    // after the run (live-outs) is handled by exit PHIs below.
    bool Clean = true;
    for (size_t j = 0; j < K && Clean; ++j) {
      size_t LB0 = LaneBase(j), LE0 = Anchors[Idx[j]].Pos + 1;
      for (size_t i = LB0; i < LE0 && Clean; ++i) {
        Instruction *I = Insns[i];
        if (Tank.count(I))
          continue; // vetted for sinking at classification
        if (isa<CallInst>(I) || isa<InvokeInst>(I))
          Clean = false;
        if (auto *L = dyn_cast<LoadInst>(I))
          if (L->getPointerOperand() != KV.first)
            Clean = false;
      }
    }
    if (!Clean)
      continue;

    // Evidence: the anchor stores carry !revenant.va from one shared
    // range with monotonically increasing VAs (the unroller emits one
    // source statement's copies at consecutive addresses). Only the
    // anchors must carry VAs: lifted IR commonly drops provenance from
    // lane value defs and address GEPs (corpus arrays: only the stores
    // carry VAs). Evidence only, never semantics: the affine/address
    // proof above stands alone.
    bool VAOK = true;
    uint64_t PrevVA = 0;
    uint64_t FirstVA = 0;
    for (size_t j = 0; j < K && VAOK; ++j) {
      auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
      auto V = vaOf(*S);
      if (!V) {
        VAOK = false;
        break;
      }
      if (j == 0)
        FirstVA = *V;
      else if (*V <= PrevVA)
        VAOK = false;
      PrevVA = *V;
    }
    if (!VAOK)
      continue;
    if (PrevVA - FirstVA > 512)
      continue;
    // Liveness: no earlier lane's Lane value read outside the run (same
    // rule as trySynthesize); only the last lane's values may be live
    // out. Run membership is Lane insns + Tank + anchors: an address GEP
    // used by its own lane store is internal, not a live-out. Tank defs
    // feeding outside the run are expected (they sink after the loop)
    // and never block.
    SmallPtrSet<Value *, 16> RunSet;
    for (size_t j = 0; j < K; ++j)
      for (Instruction *I : Lanes[j])
        RunSet.insert(I);
    for (Instruction *T : Tank)
      RunSet.insert(T);
    bool Blocked = false;
    for (size_t j = 0; j + 1 < K && !Blocked; ++j) {
      for (Instruction *D : Lanes[j]) {
        if (D->getType()->isVoidTy())
          continue;
        for (User *U : D->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != &B || !RunSet.count(UI)) {
            Blocked = true;
            break;
          }
        }
      }
    }
    if (Blocked)
      continue;

    // Live-out last-lane dsts become exit PHIs. The loop body keeps only
    // the address GEP and anchor store of lane First (see below); a
    // live-out lane value def (e.g. the last lane's mul, read by the
    // return) has no kept counterpart, so it is recorded here and
    // rebound to the rewired value expression after the rewire below
    // (LiveValueDef).
    SmallVector<std::pair<Value *, Value *>, 4> LiveOut;
    Value *LiveValueDef = nullptr; // last lane's value def, if live out
    {
      auto &Last = Lanes[K - 1];
      auto &Body = Lanes[First];
      for (size_t p = 0; p < Last.size(); ++p) {
        Value *D = Last[p];
        if (D->getType()->isVoidTy())
          continue;
        bool Outside = false;
        for (User *U : D->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != &B || !RunSet.count(UI)) {
            Outside = true;
            break;
          }
        }
        if (!Outside)
          continue;
        Value *Counter = p < Body.size() ? Body[p] : nullptr;
        // The address GEP pairs positionally; a value def is rebound
        // post-rewire.
        if (Counter && isa<GetElementPtrInst>(Counter))
          LiveOut.push_back({D, Counter});
        else
          LiveValueDef = D;
      }
    }

    // Build: preheader ([..Start)) -> loop -> exit, mirroring
    // trySynthesize. The anchor value operand is rewired to Step*i + D0.
    LLVMContext &Ctx = F.getContext();
    BasicBlock *Pre = &B;
    BasicBlock *Exit = (RunEnd < N)
                           ? Pre->splitBasicBlock(Insns[RunEnd], "unroll.exit")
                           : Pre->splitBasicBlock(Pre->getTerminator(),
                                                  "unroll.exit");
    BasicBlock *Loop = Pre->splitBasicBlock(Insns[Start], "unroll");
    // The loop keeps lane First's address GEP + anchor store; every
    // other Lane insn joins the drop set. Tank insns are sunk to the
    // exit front (they are loop-invariant and side-effect-free).
    SmallPtrSet<Instruction *, 16> DropSet;
    auto &BodyLane = Lanes[First];
    auto *BodyStore = cast<StoreInst>(BodyLane.back());
    Value *BodyAddr = BodyStore->getPointerOperand();
    SmallVector<Instruction *, 8> BodyInsns;
    for (Instruction *I : BodyLane) {
      if (I == BodyStore || I == BodyAddr)
        BodyInsns.push_back(I);
      else
        DropSet.insert(I);
    }
    for (size_t j = 0; j < K; ++j) {
      if (j == First)
        continue;
      for (Instruction *I : Lanes[j])
        DropSet.insert(I);
    }
    // Narrow prologue lanes (j < First) are included above (all j != First
    // drop); the rewired body subsumes them.
    IRBuilder<> LB(Loop->getContext());
    LB.SetInsertPoint(Loop, Loop->begin());
    Type *I64 = Type::getInt64Ty(Ctx);
    PHINode *IPhi = LB.CreatePHI(I64, 2, "u0i");
    IPhi->addIncoming(ConstantInt::get(I64, 0), Pre);
    if (MDNode *N = Pre->getTerminator()->getMetadata(VA_MD))
      IPhi->setMetadata(VA_MD, N);

    // Rewire the kept anchor store's value to Leaf*(Step*i + Scale0).
    // Vals[j] is lane j's scale of the shared leaf over the lane index;
    // the body computes the index part from the counter and scales by
    // the leaf once. RewiredFinal carries the loop's value for a
    // live-out lane value def (see LiveValueDef above).
    Value *RewiredFinal = nullptr;
    {
      auto *S = cast<StoreInst>(BodyInsns.back());
      IRBuilder<> EB(S);
      Value *Scaled = EB.CreateMul(IPhi, ConstantInt::get(I64, Step), "u0o");
      Value *Wide = Scaled;
      if (ScaleOf(0) != 0)
        Wide = EB.CreateAdd(Scaled, ConstantInt::get(I64, ScaleOf(0)), "u0o");
      // Widen the leaf to the counter width before scaling: the leaf is
      // the source-width factor (i32), the counter math is i64.
      Value *WideLeaf = Leaf;
      if (Leaf->getType() != I64) {
        if (Leaf->getType()->isIntegerTy() &&
            Leaf->getType()->getIntegerBitWidth() < 64)
          WideLeaf = EB.CreateSExt(Leaf, I64, "u0o");
        else
          continue; // non-integer leaf: leave the run alone
      }
      Value *Final = EB.CreateMul(WideLeaf, Wide, "u0o");
      Type *OpTy = S->getValueOperand()->getType();
      if (Final->getType() != OpTy) {
        if (OpTy->isIntegerTy() && Final->getType()->isIntegerTy()) {
          unsigned W = OpTy->getIntegerBitWidth();
          unsigned FW = Final->getType()->getIntegerBitWidth();
          if (W < FW)
            Final = EB.CreateTrunc(Final, OpTy, "u0o");
          else
            Final = EB.CreateSExt(Final, OpTy, "u0o");
        } else {
          continue; // type mismatch: leave the run alone
        }
      }
      if (auto *MI = dyn_cast<Instruction>(Scaled))
        copyVA(S, MI);
      if (auto *MI = dyn_cast<Instruction>(Final))
        if (Final != Scaled)
          copyVA(S, MI);
      S->setOperand(0, Final);
      RewiredFinal = Final;
      Value *BasePtr = KV.first;
      IRBuilder<> AB(S);
      Value *Off = AB.CreateMul(IPhi, ConstantInt::get(I64, Stride), "u0a");
      Value *ByteBase = BasePtr;
      if (!BasePtr->getType()->isPointerTy())
        continue;
      // base byte offset of lane 0 + i*Stride.
      Value *Full = Off;
      if (Anchors[Idx[0]].Off != 0)
        Full = AB.CreateAdd(Off, ConstantInt::get(I64, Anchors[Idx[0]].Off),
                            "u0a");
      Value *NewAddr = AB.CreateGEP(Type::getInt8Ty(Ctx), ByteBase, Full, "u0a");
      S->setOperand(1, NewAddr);
      if (auto *MI = dyn_cast<Instruction>(NewAddr))
        copyVA(S, MI);
    }

    // Latch: i_next = i + 1; c = i_next ult K; cbr.
    Instruction *LatchBr = Loop->getTerminator();
    {
      IRBuilder<> EB(LatchBr);
      Value *Next = EB.CreateAdd(IPhi, ConstantInt::get(I64, 1), "u0inext");
      Value *Cond = EB.CreateICmpULT(Next, ConstantInt::get(I64, (int64_t)K),
                                     "u0c");
      copyVA(LatchBr, cast<Instruction>(Next));
      copyVA(LatchBr, cast<Instruction>(Cond));
      IPhi->addIncoming(Next, Loop);
      EB.CreateCondBr(Cond, Loop, Exit);
      LatchBr->eraseFromParent();
    }
    BasicBlock *ExitBlk = Exit;
    IRBuilder<> XB(&ExitBlk->front());
    // Sink Tank insns after the exit PHIs, in original order (they are
    // loop-invariant and side-effect-free; operands are available:
    // nothing sunk reads a Lane def, checked at classification).
    Instruction *SinkPt = &ExitBlk->front();
    while (isa<PHINode>(SinkPt))
      SinkPt = SinkPt->getNextNode();
    for (size_t j = 0; j < K; ++j) {
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      for (size_t i = LB0; i < LE0; ++i)
        if (Tank.count(Insns[i]))
          Insns[i]->moveBefore(SinkPt);
    }
    DenseMap<Value *, Value *> Rename;
    for (auto &LO : LiveOut) {
      PHINode *P = XB.CreatePHI(LO.second->getType(), 1, "u0x");
      P->addIncoming(LO.second, Loop);
      if (auto *D = dyn_cast<Instruction>(LO.second))
        copyVA(D, P);
    }
    // A live-out lane value def carries the last iteration's rewired
    // value (RewiredFinal dominates the latch, hence the exit).
    if (LiveValueDef && RewiredFinal &&
        LiveValueDef->getType() == RewiredFinal->getType()) {
      PHINode *P = XB.CreatePHI(LiveValueDef->getType(), 1, "u0x");
      P->addIncoming(RewiredFinal, Loop);
      if (auto *D = dyn_cast<Instruction>(RewiredFinal))
        copyVA(D, P);
      Rename[LiveValueDef] = P;
    }
    for (auto &KV2 : Rename)
      KV2.first->replaceAllUsesWith(KV2.second);
    SmallVector<Instruction *, 16> DropDead;
    for (auto It = Loop->rbegin(); It != Loop->rend(); ++It)
      if (DropSet.count(&*It))
        DropDead.push_back(&*It);
    for (Instruction *I : DropDead)
      I->eraseFromParent();
    for (PHINode &P : Loop->phis()) {
      for (unsigned i = 0; i < P.getNumIncomingValues(); ++i) {
        if (P.getIncomingBlock(i) != Pre && P.getIncomingBlock(i) != Loop) {
          P.setIncomingBlock(i, P.getIncomingBlock(i) == Exit ? Loop : Pre);
        }
      }
    }
    for (Function::iterator BI = F.begin(), BE = F.end(); BI != BE; ++BI) {
      BasicBlock *OB = &*BI;
      if (OB == Loop || OB == ExitBlk)
        continue;
      for (Instruction &I : *OB)
        if (auto *P = dyn_cast<PHINode>(&I))
          for (unsigned i = 0; i < P->getNumIncomingValues(); ++i)
            if (P->getIncomingBlock(i) == Pre)
              P->setIncomingBlock(i, ExitBlk);
    }
    Notes.push_back(Pre->getName());
    return (unsigned)K;
      } // next start
    } // next length
  } // next base
  return 0;
}

// Chained-region full unroll (Rust vec_ops family): N single-entry /
// single-exit diamond regions, each guard-keys on a loop-carried length
// and stores a per-copy constant fitting a quadratic in the copy index.
// Region j: guard G_j (`cmp len_j, g_j`, br grow_j / join_j), optional
// grow block (same callee, same args, br join_j), join J_j (buf/len PHIs
// merging guard/grow paths, `store sq_j` at element index j, latch br to
// G_{j+1} or the function tail). The rolled loop carries (i, buf, len):
// the guard becomes `len == g(i)`, the square store becomes sq(i), the
// grow path stays a conditional side block inside the loop.
unsigned tryRerollRegions(Function &F, SmallVectorImpl<StringRef> &Notes) {
  // Linear block walk: follow single-successor joins into next guards.
  SmallVector<BasicBlock *, 32> Order;
  for (BasicBlock &B : F)
    Order.push_back(&B);
  // Candidate guards: cond-br blocks whose condition is `icmp eq X, C`
  // with a constant, where one successor is a grow block (single call +
  // reloads, unconditional br back) and the other is a join with PHIs.
  struct Region {
    BasicBlock *Guard = nullptr;
    BasicBlock *Grow = nullptr; // may equal Join when no grow call
    BasicBlock *Join = nullptr;
    int64_t GuardConst = 0;
    int64_t SquareConst = 0;
    bool HasSquare = false;
  };
  SmallVector<Region, 8> Regions;
  for (BasicBlock *G : Order) {
    auto *BI = dyn_cast<BranchInst>(G->getTerminator());
    if (!BI || !BI->isConditional())
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp || Cmp->getPredicate() != ICmpInst::ICMP_EQ)
      continue;
    auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    if (!C)
      continue;
    auto S = signedConst(C);
    if (!S)
      continue;
    // Successors: one must be a grow (call + br join), other the join.
    BasicBlock *T = BI->getSuccessor(0), *Fl = BI->getSuccessor(1);
    Region R;
    R.Guard = G;
    R.GuardConst = *S;
    // Try both orientations.
    for (int O = 0; O < 2; ++O) {
      BasicBlock *MaybeGrow = O == 0 ? T : Fl;
      BasicBlock *MaybeJoin = O == 0 ? Fl : T;
      // Grow: each non-phi, non-terminator insn is a call to the same
      // callee or a load; ends with unconditional br to MaybeJoin.
      auto *GBr = dyn_cast<BranchInst>(MaybeGrow->getTerminator());
      if (!GBr || GBr->isConditional() || GBr->getSuccessor(0) != MaybeJoin)
        continue;
      bool GrowOK = true;
      for (Instruction &I : *MaybeGrow) {
        if (isa<PHINode>(&I) || I.isTerminator())
          continue;
        if (isa<CallInst>(&I) || isa<LoadInst>(&I))
          continue;
        GrowOK = false;
        break;
      }
      if (!GrowOK)
        continue;
      // Join: opens with PHIs, contains a square store at element index
      // SOC, ends with a conditional latch br (next guard) or the tail.
      bool HasPhi = false;
      for (Instruction &I : *MaybeJoin) {
        if (isa<PHINode>(&I))
          HasPhi = true;
        break;
      }
      if (!HasPhi)
        continue;
      R.Grow = MaybeGrow;
      R.Join = MaybeJoin;
      break;
    }
    if (!R.Join)
      continue;
    // Square: a store of a constant to a GEP with constant element
    // index in the join.
    for (Instruction &I : *R.Join) {
      auto *S2 = dyn_cast<StoreInst>(&I);
      if (!S2)
        continue;
      auto *VC = dyn_cast<ConstantInt>(S2->getValueOperand());
      if (!VC)
        continue;
      auto *G2 = dyn_cast<GetElementPtrInst>(S2->getPointerOperand());
      if (!G2 || G2->getNumIndices() != 1)
        continue;
      auto *IC = dyn_cast<ConstantInt>(G2->getOperand(1));
      if (!IC)
        continue;
      auto SV = signedConst(VC);
      auto IV = signedConst(IC);
      if (!SV || !IV)
        continue;
      // One square per region: the element-index store. Distinguish
      // from the index-slot store (constant base, non-GEP address).
      R.SquareConst = *SV;
      R.HasSquare = true;
      break;
    }
    Regions.push_back(R);
  }
  if (Regions.size() < 3)
    return 0;
  // Chain: the join fuses the next guard's latch compare (one block holds
  // PHIs + square store + next `cmp len, g`), so join_j must branch to
  // grow_{j+1}/join_{j+1}. The last join may instead fall to the tail
  // (no guard compare). Guard consts must be affine g(j) = a*j + b;
  // squares must fit s(j) = a*j^2 + b*j + c solved from the first three,
  // verified on all. Grow callees must all match with equal args.
  // Chain check first (positional).
  for (size_t j = 0; j + 1 < Regions.size(); ++j) {
    bool Next = false;
    for (BasicBlock *S : successors(Regions[j].Join))
      if (S == Regions[j + 1].Grow || S == Regions[j + 1].Join)
        Next = true;
    if (!Next)
      return 0;
  }
  // Guard affine: g(j) = G0 + j * (G1 - G0).
  int64_t GStep = Regions[1].GuardConst - Regions[0].GuardConst;
  for (size_t j = 0; j < Regions.size(); ++j)
    if (Regions[j].GuardConst != Regions[0].GuardConst + (int64_t)j * GStep)
      return 0;
  // All regions need squares.
  for (auto &R : Regions)
    if (!R.HasSquare)
      return 0;
  // Quadratic fit from the first three squares, verified on all.
  // s(j) = A*j^2 + B*j + C; C = s0; A + B = s1 - s0; 4A + 2B = s2 - s0.
  int64_t s0 = Regions[0].SquareConst, s1 = Regions[1].SquareConst,
           s2 = Regions[2].SquareConst;
  int64_t d1 = s1 - s0, d2 = s2 - s0;
  // 2A = d2 - 2*d1 must be even.
  if ((d2 - 2 * d1) % 2 != 0)
    return 0;
  int64_t A = (d2 - 2 * d1) / 2;
  int64_t B = d1 - A;
  for (size_t j = 0; j < Regions.size(); ++j) {
    int64_t jj = (int64_t)j;
    if (A * jj * jj + B * jj + s0 != Regions[j].SquareConst)
      return 0;
  }
  // Grow callees: identical direct callees, identical (loop-invariant)
  // non-call operands. Only when every region has a grow call.
  const Function *Callee = nullptr;
  for (auto &R : Regions) {
    if (R.Grow == R.Join)
      return 0;
    const Function *CF = nullptr;
    for (Instruction &I : *R.Grow) {
      auto *CB = dyn_cast<CallInst>(&I);
      if (!CB || CB->isIndirectCall())
        continue;
      if (CF)
        return 0; // two calls per grow: out of scope
      CF = CB->getCalledFunction();
    }
    if (!CF)
      return 0;
    if (!Callee)
      Callee = CF;
    else if (Callee != CF)
      return 0;
  }
  // Evidence: guard/store VAs monotonic in one shared range (same rule
  // as lanes: shape proves, VA confirms).
  uint64_t PrevVA = 0, FirstVA = 0;
  for (size_t j = 0; j < Regions.size(); ++j) {
    auto V = vaOf(*Regions[j].Guard->getTerminator());
    if (!V)
      return 0;
    if (j == 0)
      FirstVA = *V;
    else if (*V <= PrevVA)
      return 0;
    PrevVA = *V;
    if (PrevVA - FirstVA > 4096)
      return 0;
  }
  // Build: fold the chain into one loop reusing region 0's three blocks
  // (guard_0/grow_0/join_0) as the loop body.
  //   - Header H = guard_0: new counter phi i (0..N-1) plus loop-carried
  //     buf/len PHIs. Each join_0 PHI merges a guard-path value (the
  //     previous iteration's value, i.e. the loop PHI) and a grow-path
  //     value. The loop PHIs take [prologue value, join backedge].
  //   - Guard const g_j -> GStep*i + G0; square const -> A*i^2 + B*i + s0;
  //     element index -> baseIdx + i. All computed from the counter.
  //   - Latch at join_0 end: i+1, ult N -> H : Exit. Regions 1.. erased;
  //     tail uses of region N-1 values rebound to exit PHIs.
  size_t N = Regions.size();
  BasicBlock *H = Regions[0].Guard;
  BasicBlock *Grow0 = Regions[0].Grow;
  BasicBlock *J0 = Regions[0].Join;
  LLVMContext &Ctx = F.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  // Prologue values: join_0's guard-path PHI incomings (from guard_0's
  // predecessors outside the chain).
  struct Carry {
    PHINode *JoinPhi = nullptr;
    Value *Prologue = nullptr;
    PHINode *LoopPhi = nullptr;
  };
  SmallVector<Carry, 4> Carries;
  for (PHINode &P : J0->phis()) {
    // The guard-path incoming arrives from guard_0 (fallthrough) — find
    // which incoming comes from outside grow_0.
    Value *Pro = nullptr;
    for (unsigned k = 0; k < P.getNumIncomingValues(); ++k) {
      if (P.getIncomingBlock(k) != Grow0) {
        Pro = P.getIncomingValue(k);
        break;
      }
    }
    if (!Pro)
      return 0;
    Carries.push_back({&P, Pro, nullptr});
  }
  if (Carries.empty())
    return 0;
  // The tail: successors of the last join outside the chain, or the last
  // join itself when it has none (it ends in the function tail: opaque
  // side effects + noreturn, which must be preserved, not erased). In
  // the latter case the last join is kept as the loop's exit block: its
  // PHIs rebase to the loop merges and its incoming blocks retarget.
  BasicBlock *Tail = nullptr;
  bool KeepLast = false;
  {
    BasicBlock *JL = Regions[N - 1].Join;
    for (BasicBlock *S : successors(JL)) {
      bool InChain = false;
      for (auto &R : Regions)
        if (S == R.Guard || S == R.Grow || S == R.Join)
          InChain = true;
      if (!InChain) {
        if (Tail)
          return 0; // two tails: out of scope
        Tail = S;
      }
    }
    if (!Tail) {
      Tail = JL;
      KeepLast = true;
    }
  }
  // ---- Validation phase (read-only; no mutation below this point may
  // bail: every check that can fail runs here, before the split). ----
  ICmpInst *GuardCmp = nullptr;
  unsigned GuardOp = 0;
  bool GuardTrunc = false;
  {
    auto *BI = dyn_cast<BranchInst>(H->getTerminator());
    if (!BI || !BI->isConditional())
      return 0;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      return 0;
    for (unsigned o = 0; o < 2; ++o) {
      if (auto *CC = dyn_cast<ConstantInt>(Cmp->getOperand(o))) {
        if (CC->getSExtValue() == Regions[0].GuardConst) {
          GuardCmp = Cmp;
          GuardOp = o;
          auto *OT = Cmp->getOperand(o)->getType();
          if (OT == I64)
            GuardTrunc = false;
          else if (OT->isIntegerTy() && OT->getIntegerBitWidth() < 64)
            GuardTrunc = true;
          else
            return 0;
          break;
        }
      }
    }
    if (!GuardCmp)
      return 0;
  }
  // Square store found, types compatible.
  StoreInst *SqStore = nullptr;
  GetElementPtrInst *SqGEP = nullptr;
  bool SqTrunc = false, IxTrunc = false, IxIs64 = false;
  int64_t BaseIdx = 0;
  {
    for (Instruction &I : *J0) {
      auto *S = dyn_cast<StoreInst>(&I);
      if (!S)
        continue;
      auto *VC = dyn_cast<ConstantInt>(S->getValueOperand());
      if (!VC || VC->getSExtValue() != Regions[0].SquareConst)
        continue;
      auto *G = dyn_cast<GetElementPtrInst>(S->getPointerOperand());
      if (!G || G->getNumIndices() != 1)
        continue;
      Type *VT = S->getValueOperand()->getType();
      if (VT != I64) {
        if (VT->isIntegerTy() && VT->getIntegerBitWidth() <= 64)
          SqTrunc = true;
        else
          return 0;
      }
      auto *IC = dyn_cast<ConstantInt>(G->getOperand(1));
      if (!IC)
        continue;
      Type *IT = IC->getType();
      if (IT != I64 && !(IT->isIntegerTy() && IT->getIntegerBitWidth() < 64))
        continue;
      SqStore = S;
      SqTrunc = (VT != I64);
      IxTrunc = (IT != I64);
      IxIs64 = (IT == I64);
      BaseIdx = IC->getSExtValue();
      break;
    }
    if (!SqStore)
      return 0;
  }
  // Tail reads: no outside use of chain-interior values except through
  // the last join's PHIs (the loop's live-outs).
  {
    BasicBlock *JL = Regions[N - 1].Join;
    SmallPtrSet<Value *, 8> LastPhis;
    for (PHINode &P : JL->phis())
      LastPhis.insert(&P);
    for (BasicBlock &OBB : F) {
      BasicBlock *OB = &OBB;
      bool InChain = OB == H || OB == Grow0 || OB == J0;
      for (auto &R : Regions)
        if (OB == R.Guard || OB == R.Grow || OB == R.Join)
          InChain = true;
      if (InChain)
        continue;
      for (Instruction &I : *OB) {
        for (Use &U : I.operands()) {
          auto *D = dyn_cast<Instruction>(U.get());
          if (!D)
            continue;
          BasicBlock *PB = D->getParent();
          bool DefInChain = false;
          for (auto &R : Regions)
            if (PB == R.Guard || PB == R.Grow || PB == R.Join)
              DefInChain = true;
          if (!DefInChain)
            continue;
          if (!LastPhis.count(D))
            return 0;
        }
      }
    }
  }
  // Exit rebind pairs: each last-join PHI pairs with the next join_0 PHI
  // of the same type in order. The last join may carry a subset (the
  // tailless last copy has no len PHI: no guard compare follows it).
  // Every last-join PHI must pair; unpaired join_0 PHIs are fine.
  SmallVector<std::pair<PHINode *, PHINode *>, 4> ExitPairs;
  {
    BasicBlock *JL = Regions[N - 1].Join;
    SmallVector<PHINode *, 4> J0Phis, JLPhis;
    for (PHINode &P : J0->phis())
      J0Phis.push_back(&P);
    for (PHINode &P : JL->phis())
      JLPhis.push_back(&P);
    SmallVector<bool, 4> Used(J0Phis.size(), false);
    for (PHINode *LP : JLPhis) {
      bool Found = false;
      for (size_t k = 0; k < J0Phis.size(); ++k) {
        if (!Used[k] && J0Phis[k]->getType() == LP->getType()) {
          Used[k] = true;
          ExitPairs.push_back({LP, J0Phis[k]});
          Found = true;
          break;
        }
      }
      if (!Found)
        return 0;
    }
    if (ExitPairs.empty())
      return 0;
  }
  // Dead-use check: every non-void, non-terminator insn in erased regions
  // is used only inside erased regions or is a rebound last-join PHI.
  {
    SmallPtrSet<BasicBlock *, 12> DeadSet;
    for (size_t j = 1; j < N; ++j) {
      DeadSet.insert(Regions[j].Guard);
      if (Regions[j].Grow != Regions[j].Join)
        DeadSet.insert(Regions[j].Grow);
      if (KeepLast && j == N - 1)
        continue;
      DeadSet.insert(Regions[j].Join);
    }
    SmallPtrSet<Value *, 8> Rebound;
    for (auto &PR : ExitPairs)
      Rebound.insert(PR.first);
    for (BasicBlock *D : DeadSet) {
      for (Instruction &I : *D) {
        if (I.getType()->isVoidTy() || I.isTerminator())
          continue;
        for (User *U : I.users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI)
            return 0;
          if (!DeadSet.count(UI->getParent()) && !Rebound.count(&I))
            return 0;
        }
      }
    }
  }
  // guard compare so Pre keeps the prologue: H = [cmp, br].
  {
    auto *BI = dyn_cast<BranchInst>(H->getTerminator());
    if (!BI || !BI->isConditional())
      return 0;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      return 0;
    BasicBlock *NH = H->splitBasicBlock(Cmp, "vec.head");
    // The prologue half keeps the name/uses; NH is the loop header.
    H = NH;
  }
  // Counter + loop PHIs at the header top.
  IRBuilder<> HB(&H->front());
  PHINode *IPhi = HB.CreatePHI(I64, 2, "u0i");
  // Preheader is now H's single predecessor by construction.
  BasicBlock *Pre = H->getSinglePredecessor();
  if (!Pre)
    return 0;
  IPhi->addIncoming(ConstantInt::get(I64, 0), Pre);
  for (auto &C : Carries) {
    PHINode *LP = HB.CreatePHI(C.JoinPhi->getType(), 2, "u0c");
    LP->addIncoming(C.Prologue, Pre);
    C.LoopPhi = LP;
  }
  // Redirect guard_0's reads of the prologue values to the loop PHIs:
  // the join_0 guard-path incomings.
  for (auto &C : Carries) {
    for (unsigned k = 0; k < C.JoinPhi->getNumIncomingValues(); ++k)
      if (C.JoinPhi->getIncomingBlock(k) != Grow0)
        C.JoinPhi->setIncomingValue(k, C.LoopPhi);
    // Guard reads the len through the join PHI chain: replace uses of
    // the prologue value inside guard_0/grow_0 with the loop PHI. Only
    // safe when the prologue value is defined outside the chain.
    if (auto *PD = dyn_cast<Instruction>(C.Prologue)) {
      if (PD->getParent() == H || PD->getParent() == Grow0 ||
          PD->getParent() == J0)
        return 0;
    }
    // NB: collect first, and skip the loop PHIs' own prologue incomings
    // (added above); rewriting those would self-reference the loop PHI.
    SmallVector<Use *, 8> ToRew;
    for (Use &U : C.Prologue->uses()) {
      auto *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI)
        return 0;
      if (UI == C.LoopPhi)
        continue;
      ToRew.push_back(&U);
    }
    for (Use *U : ToRew) {
      auto *UI = dyn_cast<Instruction>(U->getUser());
      if (UI->getParent() == H || UI->getParent() == Grow0)
        U->set(C.LoopPhi);
    }
  }
  // Counter expressions, built at the top of the header (after PHIs).
  IRBuilder<> EB(&H->front());
  // Advance past PHIs for insertion.
  Instruction *InsPt = &H->front();
  while (isa<PHINode>(InsPt))
    InsPt = InsPt->getNextNode();
  EB.SetInsertPoint(InsPt);
  // Guard const: GStep*i + G0.
  Value *GExpr = EB.CreateMul(IPhi, ConstantInt::get(I64, GStep), "u0g");
  if (Regions[0].GuardConst != 0)
    GExpr = EB.CreateAdd(GExpr, ConstantInt::get(I64, Regions[0].GuardConst),
                         "u0g");
  // Find guard_0's icmp and replace its constant operand.
  {
    auto *BI = dyn_cast<BranchInst>(H->getTerminator());
    if (!BI || !BI->isConditional())
      return 0;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      return 0;
    bool Rewired = false;
    for (unsigned o = 0; o < 2; ++o) {
      if (auto *CC = dyn_cast<ConstantInt>(Cmp->getOperand(o))) {
        if (CC->getSExtValue() == Regions[0].GuardConst) {
          // Types: icmp may be narrower than i64; truncate.
          Value *G = GExpr;
          if (G->getType() != Cmp->getOperand(o)->getType()) {
            auto *OT = Cmp->getOperand(o)->getType();
            if (OT->isIntegerTy() && OT->getIntegerBitWidth() < 64)
              G = EB.CreateTrunc(G, OT, "u0g");
            else
              return 0;
          }
          Cmp->setOperand(o, G);
          Rewired = true;
          break;
        }
      }
    }
    if (!Rewired)
      return 0;
  }
  // Square + index rewrite in join_0: find the square store (constant
  // value + constant-index GEP) and rewire both to counter exprs.
  Value *SqFinal = nullptr;
  {
    bool Done = false;
    for (Instruction &I : *J0) {
      auto *S = dyn_cast<StoreInst>(&I);
      if (!S)
        continue;
      auto *VC = dyn_cast<ConstantInt>(S->getValueOperand());
      if (!VC || VC->getSExtValue() != Regions[0].SquareConst)
        continue;
      auto *G = dyn_cast<GetElementPtrInst>(S->getPointerOperand());
      if (!G || G->getNumIndices() != 1)
        continue;
      // Square: A*i^2 + B*i + s0.
      IRBuilder<> SB(S);
      Value *I2 = SB.CreateMul(IPhi, IPhi, "u0q");
      Value *Sq = I2;
      if (A != 0)
        Sq = SB.CreateMul(I2, ConstantInt::get(I64, A), "u0q");
      else
        Sq = ConstantInt::get(I64, 0);
      if (B != 0)
        Sq = SB.CreateAdd(Sq, SB.CreateMul(IPhi, ConstantInt::get(I64, B),
                                           "u0q"),
                          "u0q");
      if (Regions[0].SquareConst != 0 || (A == 0 && B == 0))
        Sq = SB.CreateAdd(Sq,
                          ConstantInt::get(I64, Regions[0].SquareConst),
                          "u0q");
      Type *VT = S->getValueOperand()->getType();
      if (Sq->getType() != VT) {
        if (VT->isIntegerTy() && VT->getIntegerBitWidth() <= 64)
          Sq = SB.CreateTrunc(Sq, VT, "u0q");
        else
          return 0;
      }
      S->setOperand(0, Sq);
      SqFinal = Sq;
      // Index: baseIdx + i. Recover baseIdx from region 0's GEP index
      // minus 0 (region j stores at element j => base 0 here); general
      // form keeps the region-0 index as the i=0 value.
      auto *IC = dyn_cast<ConstantInt>(G->getOperand(1));
      if (!IC)
        return 0;
      int64_t BaseIdx = IC->getSExtValue();
      Value *Ix = IPhi;
      if (BaseIdx != 0)
        Ix = SB.CreateAdd(IPhi, ConstantInt::get(I64, BaseIdx), "u0x");
      Type *IT = IC->getType();
      if (Ix->getType() != IT) {
        if (IT->isIntegerTy() && IT->getIntegerBitWidth() < 64)
          Ix = SB.CreateTrunc(Ix, IT, "u0x");
        else if (IT == I64)
          ;
        else
          return 0;
      }
      G->setOperand(1, Ix);
      copyVA(S, cast<Instruction>(Ix == IPhi ? IPhi : Ix));
      Done = true;
      break;
    }
    if (!Done)
      return 0;
  }
  // Backedges: join_0 PHIs gain the loop backedge (their own merge
  // already combines guard/grow paths per iteration); the loop PHIs take
  // the join_0 merge outputs. Latch: split join_0's latch edge.
  for (auto &C : Carries) {
    // The join PHI's per-iteration merge value feeds the loop PHI.
    C.LoopPhi->addIncoming(C.JoinPhi, J0);
  }
  // Latch: rewrite join_0's terminator (which branches to guard_1) to
  // the counted latch. The only live successor needed is the tail.
  {
    Instruction *OldTerm = J0->getTerminator();
    IRBuilder<> LB(OldTerm);
    Value *Next = LB.CreateAdd(IPhi, ConstantInt::get(I64, 1), "u0inext");
    Value *Cond = LB.CreateICmpULT(Next, ConstantInt::get(I64, (int64_t)N),
                                   "u0c");
    IPhi->addIncoming(Next, J0);
    LB.CreateCondBr(Cond, H, Tail);
    OldTerm->eraseFromParent();
  }
  // Erase regions 1..: ExitPairs (validated above) rebind last-join
  // merges to the loop merges. No bailouts below: all checks ran pre-split.
  {
    BasicBlock *JL = Regions[N - 1].Join;
    for (auto &PR : ExitPairs)
      PR.first->replaceAllUsesWith(PR.second);
    // Retarget tail's incoming blocks from the last join to the new exit
    // edge (the tail is reached from J0's latch now, i.e. from J0).
    for (Instruction &I : *Tail) {
      if (auto *P = dyn_cast<PHINode>(&I)) {
        for (unsigned k = 0; k < P->getNumIncomingValues(); ++k)
          if (P->getIncomingBlock(k) == JL)
            P->setIncomingBlock(k, J0);
      }
    }
  }
  // Erase regions 1..N-1 blocks (leaving H/Grow0/J0, plus the last join
  // when it is the kept tail). Uses were validated pre-split; the erase
  // itself cannot fail.
  if (KeepLast) {
    // The kept tail's PHIs are rebound to the loop merges above; their
    // incoming blocks are erased regions, so the PHIs themselves must go.
    BasicBlock *JL = Regions[N - 1].Join;
    SmallVector<PHINode *, 4> DeadPhis;
    for (PHINode &P : JL->phis())
      DeadPhis.push_back(&P);
    for (PHINode *P : DeadPhis)
      P->eraseFromParent();
  }
  {
    SmallVector<BasicBlock *, 8> Dead;
    for (size_t j = 1; j < N; ++j) {
      Dead.push_back(Regions[j].Guard);
      if (Regions[j].Grow != Regions[j].Join)
        Dead.push_back(Regions[j].Grow);
      if (KeepLast && j == N - 1)
        continue; // kept tail: retargeted, not erased
      Dead.push_back(Regions[j].Join);
    }
    for (BasicBlock *D : Dead) {
      D->dropAllReferences();
      D->eraseFromParent();
    }
  }
  (void)Callee;
  (void)GStep;
  (void)SqFinal;
  Notes.push_back(H->getName());
  return (unsigned)N;
}

} // namespace revenant
