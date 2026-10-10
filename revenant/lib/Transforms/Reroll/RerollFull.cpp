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
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

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
  if (N < 9)
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
    if (!G || G->getParent() != &B || G->getNumIndices() != 1)
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
    if (!Tiled)
      continue;
    // Lane widths: lanes whose stored value is a bare leaf/constant need
    // no value def, so leading lanes may be narrower. Find the first full
    // lane First (containing a computed value); all lanes j >= First
    // share width M, and all lanes j < First are narrow prologue (each
    // holds only its address GEP and anchor store). First must leave
    // >= MinWindows full lanes.
    size_t First = K;
    size_t M = 0;
    for (size_t j = 0; j < K; ++j) {
      auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
      Value *V = S->getValueOperand();
      if (auto *DI = dyn_cast<Instruction>(V)) {
        if (Def.count(V) && Def[V]->getParent() == &B) {
          (void)DI;
          First = j;
          break;
        }
      }
    }
    if (First >= K || First + 1 >= K)
      continue;
    M = Anchors[Idx[First + 1]].Pos - Anchors[Idx[First]].Pos;
    if (M == 0)
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
    bool WidthOK = true;
    for (size_t j = First + 1; j < K; ++j)
      if (Anchors[Idx[j]].Pos - Anchors[Idx[j - 1]].Pos != M)
        WidthOK = false;
    if (!WidthOK)
      continue;
    // Narrow lanes before F hold only address GEP + store.
    for (size_t j = 0; j < First && WidthOK; ++j) {
      size_t LB0 = (j == 0) ? Start : Anchors[Idx[j - 1]].Pos + 1;
      size_t LE0 = Anchors[Idx[j]].Pos + 1;
      if (LE0 - LB0 > M)
        WidthOK = false;
      for (size_t i = LB0; i < LE0 && WidthOK; ++i) {
        Instruction *I = Insns[i];
        auto *S = cast<StoreInst>(Insns[Anchors[Idx[j]].Pos]);
        if (I == S || I == S->getPointerOperand())
          continue;
        WidthOK = false;
      }
    }
    if (!WidthOK)
      continue;
    size_t RunEnd = Anchors[Idx[K - 1]].Pos + 1;
    // Structural check on full lanes j >= First: only the address GEP
    // (position M-2) must match structurally (same base operand); the
    // value-def prefix (positions 0..M-3) differs per lane by design
    // (shl vs mul of the shared leaf) and is covered by the affine
    // model below, and the anchor store (M-1) likewise.
    bool ShapeOK = true;
    if (M < 2)
      ShapeOK = false;
    for (size_t j = First + 1; j < K && ShapeOK; ++j) {
      Instruction *A = Insns[Anchors[Idx[First]].Pos - 1];
      Instruction *C = Insns[Anchors[Idx[j]].Pos - 1];
      auto *GA = dyn_cast<GetElementPtrInst>(A);
      auto *GC = dyn_cast<GetElementPtrInst>(C);
      if (!GA || !GC)
        ShapeOK = false;
      else if (GA->getPointerOperand() != GC->getPointerOperand() ||
               GA->getNumIndices() != GC->getNumIndices())
        ShapeOK = false;
    }
    if (!ShapeOK)
      continue;
    // Lane base helper: first instruction index of lane j.
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
    // Liveness: no earlier lane's value read outside the run (same rule
    // as trySynthesize); only the last lane's values may be live out.
    // Run membership includes the lane stores (void): an address GEP
    // used by its own lane store is internal, not a live-out.
    SmallPtrSet<Value *, 16> RunSet;
    for (size_t j = 0; j < K; ++j) {
      size_t LB0 = LaneBase(j), LE0 = Anchors[Idx[j]].Pos + 1;
      for (size_t i = LB0; i < LE0; ++i)
        RunSet.insert(Insns[i]);
    }
    bool Blocked = false;
    for (size_t j = 0; j + 1 < K && !Blocked; ++j) {
      size_t LB0 = LaneBase(j), LE0 = Anchors[Idx[j]].Pos + 1;
      for (size_t i = LB0; i < LE0 && !Blocked; ++i) {
        Value *D = Insns[i];
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
    // the address GEP and anchor store (see below); a live-out lane value
    // def (e.g. the last lane's mul, read by the return) has no kept
    // counterpart, so it is recorded here and rebound to the rewired
    // value expression after the rewire below (LiveValueDef).
    size_t BodyM = M;
    size_t BodyBase = FirstFullBase;
    SmallVector<std::pair<Value *, Value *>, 4> LiveOut;
    Value *LiveValueDef = nullptr; // last lane's value def, if live out
    {
      size_t LB0 = Anchors[Idx[K - 1]].Pos - M + 1;
      for (size_t p = 0; p < M; ++p) {
        Value *D = Insns[LB0 + p];
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
        Value *Counter = Insns[BodyBase + p];
        // The anchor store (void) never reaches here; the address GEP
        // pairs positionally; a value def is rebound post-rewire.
        if (Counter == Insns[BodyBase + BodyM - 1]->getOperand(1) ||
            isa<GetElementPtrInst>(Counter))
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
    // Drop every lane after the body: the loop keeps [BodyBase,
    // BodyBase + BodyM), i.e. lane 1's shape. Lane 0's narrower prefix
    // ([Start, BodyBase)) is prologue that the rewired body subsumes.
    SmallPtrSet<Instruction *, 16> DropSet;
    {
      SmallVector<Instruction *, 16> InLoop;
      for (Instruction &I : *Loop)
        if (!I.isTerminator())
          InLoop.push_back(&I);
      // InLoop[0] == Insns[Start]; body starts at BodyBase.
      size_t BodyOff = BodyBase - Start;
      for (size_t i = 0; i < InLoop.size(); ++i)
        if (i < BodyOff || i >= BodyOff + BodyM)
          DropSet.insert(InLoop[i]);
    }
    SmallVector<Instruction *, 8> BodyInsns;
    // The body keeps only the address GEP and the anchor store: lane 1's
    // value def (mul/shl of the leaf) is subsumed by the rewired
    // Leaf*(Step*i + Scale0) expression, so it joins the drop set.
    {
      auto *BodyStore = cast<StoreInst>(Insns[BodyBase + BodyM - 1]);
      Value *BodyAddr = BodyStore->getPointerOperand();
      for (size_t p = 0; p < BodyM; ++p) {
        Instruction *I = Insns[BodyBase + p];
        if (I == BodyStore || I == BodyAddr)
          BodyInsns.push_back(I);
        else
          DropSet.insert(I);
      }
    }
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
    DenseMap<Value *, Value *> Rename;
    for (auto &LO : LiveOut) {
      PHINode *P = XB.CreatePHI(LO.second->getType(), 1, "u0x");
      P->addIncoming(LO.second, Loop);
      if (auto *D = dyn_cast<Instruction>(LO.second))
        copyVA(D, P);
      Rename[LO.first] = P;
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

} // namespace revenant
