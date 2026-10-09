//===- Reroll.cpp - Reverse loop unrolling ----------------------*- C++ -*-===//
//
// Part of the Revenant decompiler port, under the Apache License v2.0 with
// LLVM Exceptions. See https://llvm.org/LICENSE.txt for details.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `revenant-reroll`: folds unrolled loop bodies back into a single
// induction-variable step (partial unrolls) and synthesizes loops for
// fully-unrolled straight-line code (full unrolls, e.g. `-O3` unroll-by-N
// with epilogue or `-funroll-loops` targets).
//
// Ports `crates/unopt/src/reroll.rs` (`Reroll`), which runs after lane-0
// scalarization and devectorization in the Rust pipeline:
//
//   lane0 -> devectorize -> reroll -> unrotate -> tail-recursion
//
// Two shapes, mirroring the reference:
//
// - Partial (in-loop fold): a single-block loop whose IV steps by K per
//   iteration contains D identical-modulo-offset copies of one element's
//   work (same instructions, address constants advanced). Folding keeps
//   one copy and divides the step by D, so `i += 8` over 8 identical
//   `s += a[i+j]` windows becomes `i += 1` over `s += a[i]`.
// - Full (straight-line synthesis): straight-line runs of K identical
//   windows (fully-unrolled loops) are rewritten as a fresh counted loop
//   around the first window's body, with accumulator threads threaded
//   through new PHIs.
//
// Matching is shape/offset based; anything ambiguous stays put. Metadata
// contract (see RvIRContract): every rewritten instruction keeps its
// `!revenant.va` provenance, `!revenant.opaque` calls are never created
// or dropped by this pass, and `!revenant.region` annotations on header
// terminators are preserved (loop-region metadata touched by the fold is
// moved to the new latch/branch).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Reroll/Reroll.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;


namespace revenant {
namespace {

// Provenance metadata kind. RvIRContract owns the canonical spelling;
// read it dynamically so the pass works before/after Contract.h lands.
constexpr char VA_MD[] = "revenant.va";
constexpr char REGION_MD[] = "revenant.region";
constexpr char OPAQUE_MD[] = "revenant.opaque";

// Only fold steps inside this magnitude band (reference: 2..=4096).
constexpr int64_t MinStep = 2;
constexpr int64_t MaxStep = 4096;

// Synthesis needs enough evidence to be worth a loop (reference: >= 4
// windows before a loop is synthesized).
constexpr unsigned MinWindowsForSynth = 4;

// Shape key of an instruction: opcode plus type-class, mirroring the
// reference `expr::insn_kind` discriminant.
struct ShapeKey {
  unsigned Opcode = 0;
  unsigned TypeClass = 0; // 0 = void/none, else bit-width class bucket
  unsigned Extra = 0;     // predicate/call-kind discriminator

  bool operator==(const ShapeKey &O) const {
    return Opcode == O.Opcode && TypeClass == O.TypeClass && Extra == O.Extra;
  }
};



unsigned widthClass(Type *T) {
  if (T->isVoidTy())
    return 0;
  if (T->isPointerTy())
    return 4096;
  if (T->isFloatingPointTy())
    return 2048 + T->getScalarSizeInBits();
  if (T->isIntegerTy())
    return T->getIntegerBitWidth();
  if (auto *VT = dyn_cast<VectorType>(T))
    return 1024 + VT->getElementCount().getKnownMinValue();
  return 8192;
}

ShapeKey shapeOf(const Instruction &I) {
  ShapeKey K;
  K.Opcode = I.getOpcode();
  K.TypeClass = widthClass(I.getType());
  if (auto *CI = dyn_cast<CmpInst>(&I))
    K.Extra = CI->getPredicate();
  else if (auto *BO = dyn_cast<BinaryOperator>(&I))
    K.Extra = BO->getOpcode();
  else if (auto *CB = dyn_cast<CallBase>(&I)) {
    if (CB->getCalledFunction())
      K.Extra = (unsigned)CB->getCalledFunction()->getName().size() + 7;
    else
      K.Extra = CB->isIndirectCall() ? 3 : 5;
  } else if (auto *CI = dyn_cast<CastInst>(&I))
    K.Extra = CI->getOpcode();
  else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
    K.Extra = (unsigned)GEP->getNumIndices();
  return K;
}

// Copy !revenant.va (and any revenant.region on terminators) from Src to
// Dst so provenance survives the rewrite.
void copyProvenance(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
  if (MDNode *N = Src->getMetadata(REGION_MD))
    Dst->setMetadata(REGION_MD, N);
  if (MDNode *N = Src->getMetadata(OPAQUE_MD))
    Dst->setMetadata(OPAQUE_MD, N);
}


// Match `Phi = phi [Init, Pre] [Next, Self]` where Next = Phi +/- Step
// with |Step| in [MinStep, MaxStep]. Returns the step magnitude and the
// increment instruction.
bool matchStrideIV(PHINode *Phi, BasicBlock *Header, int64_t &StepOut,
                   Instruction *&StepInstOut) {
  if (Phi->getNumIncomingValues() != 2)
    return false;
  if (!Phi->getType()->isIntegerTy())
    return false;
  Value *Back = nullptr;
  for (unsigned i = 0; i < 2; ++i)
    if (Phi->getIncomingBlock(i) == Header)
      Back = Phi->getIncomingValue(i);
  if (!Back)
    return false;
  auto *BO = dyn_cast<BinaryOperator>(Back);
  if (!BO || (BO->getOpcode() != Instruction::Add &&
              BO->getOpcode() != Instruction::Sub))
    return false;
  if (BO->getParent() != Header)
    return false;
  Value *Other = nullptr;
  if (BO->getOperand(0) == Phi)
    Other = BO->getOperand(1);
  else
    return false;
  auto *C = dyn_cast<ConstantInt>(Other);
  if (!C)
    return false;
  int64_t Step = C->getSExtValue();
  if (BO->getOpcode() == Instruction::Sub)
    Step = -Step;
  int64_t Mag = Step >= 0 ? Step : -Step;
  if (Mag < MinStep || Mag > MaxStep)
    return false;
  StepOut = (uint64_t)Mag;
  StepInstOut = BO;
  return true;
}

// Value-expression normalization, mirroring `crates/unopt/src/expr.rs`:
// `affineOfValue` unfolds a value through pure in-block defs (Bin, Cast,
// Gep, Cmp, Select) into a (coef, off) affine with value = base + coef*IV
// + off. Load/Phi/Call/param defs are opaque leaves (0, 0); constants are
// (0, c). Only Bin/Cast/Gep/Cmp/Select defs unfold, so accumulator
// chains never leak sibling indices.
using Affine = std::pair<int64_t, int64_t>; // (coef, off)
static constexpr unsigned MaxUnfoldDepth = 8;

// Maps an SSA value to its defining instruction in the same block.
struct DefLookup {
  DenseMap<Value *, Instruction *> Map;
  Instruction *find(Value *V) const {
    auto It = Map.find(V);
    return It == Map.end() ? nullptr : It->second;
  }
};

std::optional<Affine> affineOfValue(Value *V, Value *IV, const DefLookup &DL,
                                    unsigned Depth = 0);
std::optional<Affine> affineOfInsn(const Instruction &I, Value *IV,
                                    const DefLookup &DL, unsigned Depth = 0);

std::optional<Affine> affineOfValue(Value *V, Value *IV, const DefLookup &DL,
                                    unsigned Depth) {
  if (Depth > MaxUnfoldDepth)
    return Affine{0, 0};
  if (V == IV)
    return Affine{1, 0};
  if (auto *C = dyn_cast<ConstantInt>(V))
    return Affine{0, C->getSExtValue()};
  if (Instruction *Def = DL.find(V)) {
    if (auto *GDef = dyn_cast<GetElementPtrInst>(Def)) {
      // Unfolding a GEP *def*: indices scale by the element size (the
      // reference folds `mul(idx, elemsize)` into the gep expr).
      int64_t Scale =
          (int64_t)GDef->getResultElementType()->getScalarSizeInBits() / 8;
      if (Scale <= 0)
        Scale = 1;
      auto Base = affineOfValue(GDef->getPointerOperand(), IV, DL, Depth + 1);
      if (!Base)
        return std::nullopt;
      Affine Acc = *Base;
      for (Value *Ix : GDef->indices()) {
        auto O = affineOfValue(Ix, IV, DL, Depth + 1);
        if (!O)
          return std::nullopt;
        Acc.first += O->first * Scale;
        Acc.second += O->second * Scale;
      }
      return Acc;
    }
    switch (Def->getOpcode()) {
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::Trunc:
    case Instruction::BitCast:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::GetElementPtr:
    case Instruction::ICmp:
    case Instruction::FCmp:
    case Instruction::Select:
      return affineOfInsn(*Def, IV, DL, Depth + 1);
    default:
      return Affine{0, 0}; // Load/Phi/Call/... are opaque leaves.
    }
  }
  return Affine{0, 0}; // Params, globals: opaque leaves.
}

// Combined affine of an instruction's operands (the reference
// `Expr::Op("add", insn_exprs)`): a plain unscaled sum. Compares have
// no affine.
std::optional<Affine> affineOfInsn(const Instruction &I, Value *IV,
                                    const DefLookup &DL, unsigned Depth) {
  if (isa<CmpInst>(&I))
    return std::nullopt;
  int64_t Coef = 0, Off = 0;
  for (const Use &U : I.operands()) {
    auto A = affineOfValue(U.get(), IV, DL, Depth);
    if (!A)
      return std::nullopt;
    Coef += A->first;
    Off += A->second;
  }
  return Affine{Coef, Off};
}

// Element slot of a value through a memory-deriving (Load/Gep/Cast/Bin)
// def, mirroring the reference `var_index`: the combined affine of the
// def's own operands. None when not IV-derived, poison (INT64_MAX) when
// off a clean boundary.
std::optional<int64_t> varIndex(Value *V, Value *IV, const DefLookup &DL) {
  Instruction *Def = DL.find(V);
  if (!Def)
    return std::nullopt;
  switch (Def->getOpcode()) {
  case Instruction::Load:
  case Instruction::GetElementPtr:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
  case Instruction::BitCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::Shl:
    break;
  default:
    return std::nullopt;
  }
  auto A = affineOfInsn(*Def, IV, DL, 0);
  if (!A || A->first == 0)
    return std::nullopt;
  if (A->second % A->first != 0)
    return INT64_MAX;
  return A->second / A->first;
}


// Slot of an instruction relative to IV, mirroring the reference
// `slot_of`: the combined affine of the operand exprs first, then the
// bare-var rule (a `Leaf` operand whose def is memory-deriving
// contributes `var_index`). None = shared; INT64_MAX = poison.
std::optional<int64_t> slotOf(const Instruction &I, Value *IV,
                              const DefLookup &DL,
                              DenseMap<Value *, std::optional<int64_t>> &Memo,
                              DenseMap<Value *, std::optional<int64_t>> &VIMemo) {
  auto Cached = Memo.find(const_cast<Instruction *>(&I));
  if (Cached != Memo.end())
    return Cached->second;

  std::optional<int64_t> Slot;
  if (auto A = affineOfInsn(I, IV, DL, 0)) {
    if (A->first != 0)
      Slot = (A->second % A->first == 0)
                 ? std::optional<int64_t>(A->second / A->first)
                 : std::optional<int64_t>(INT64_MAX);
  }
  // Bare-var rule: a direct `Leaf` (non-constant, non-IV) operand whose
  // def is memory-deriving contributes its def slot.
  for (const Use &U : I.operands()) {
    Value *V = U.get();
    if (V == IV || isa<Constant>(V))
      continue;
    if (!isa<Instruction>(V) && !isa<Argument>(V))
      continue;
    // Only direct leaves: the operand itself must be a bare value here,
    // i.e. its affine is an opaque (0, 0) leaf.
    auto A = affineOfValue(V, IV, DL, 0);
    if (!A || A->first != 0 || A->second != 0)
      continue;
    // Memoize varIndex per value (reference `memo` map).
    auto VC = VIMemo.find(V);
    std::optional<int64_t> D;
    if (VC != VIMemo.end())
      D = VC->second;
    else {
      D = varIndex(V, IV, DL);
      VIMemo[V] = D;
    }
    if (!D)
      continue;
    if (!Slot)
      Slot = D;
    else if (*Slot != *D)
      Slot = INT64_MAX;
  }
  Memo[const_cast<Instruction *>(&I)] = Slot;
  return Slot;
}

bool isPureDef(const Instruction &I) {
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::BitCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::GetElementPtr:
  case Instruction::Load:
  case Instruction::ICmp:
  case Instruction::FCmp:
  case Instruction::Select:
  case Instruction::PHI:
    return true;
  default:
    return false;
  }
}


// Structural equality of two instructions up to per-window constant
// shift: opcodes match, and each operand pair is (a) identical, (b) the
// same-position def in the other window, (c) an acc chain edge, or
// (d) constants differing by W * Delta recorded at window 1.
bool operandsPair(Value *A, Value *C, int64_t W, Value *IV,
                  const DefLookup &DL,
                  const DenseMap<Value *, unsigned> &W0Pos,
                  const std::vector<std::vector<Instruction *>> &WSets,
                  unsigned Pos, unsigned Op,
                  DenseMap<std::pair<unsigned, unsigned>, int64_t> &Deltas,
                  DenseMap<std::pair<unsigned, unsigned>,
                           std::pair<Value *, int64_t>> &VarPairs) {
  if (A == C)
    return true;
  auto *CA = dyn_cast<ConstantInt>(A);
  auto *CC = dyn_cast<ConstantInt>(C);
  if (CA && CC) {
    int64_t D = CC->getSExtValue() - CA->getSExtValue();
    if (D == 0)
      return true;
    auto Key = std::make_pair(Pos, Op);
    auto It = Deltas.find(Key);
    if (W == 1) {
      Deltas[Key] = D;
      return true;
    }
    if (It == Deltas.end() || It->second == 0)
      return false;
    // Linear shift (d == w*d1) or a periodic multiple of it.
    return D % It->second == 0 && D / It->second <= W;
  }
  if (isa<Constant>(A) || isa<Constant>(C))
    return false;
  // Same-position def pair (l0 vs l_j): both defined at the same window
  // position in their own window.
  auto IA = W0Pos.find(A);
  if (IA != W0Pos.end()) {
    const auto &WJ = WSets[(size_t)W];
    if (IA->second < WJ.size() && WJ[IA->second] == C)
      return true;
    // Fall through to structural check below.
  }
  // Memory re-derivations of the same shape at a shifted index pair
  // directly (e.g. gep(p, i) vs gep(p, i+w)).
  if (auto *GA = dyn_cast<GetElementPtrInst>(A))
    if (auto *GC = dyn_cast<GetElementPtrInst>(C)) {
      if (GA->getPointerOperand() != GC->getPointerOperand())
        return false;
      if (GA->getNumIndices() != GC->getNumIndices())
        return false;
      auto IA2 = GA->idx_begin(), IC2 = GC->idx_begin();
      for (; IA2 != GA->idx_end(); ++IA2, ++IC2)
        if (!operandsPair(IA2->get(), IC2->get(), W, IV, DL, W0Pos, WSets,
                          Pos, Op, Deltas, VarPairs))
          return false;
      return true;
    }
  // Mem-derived pair (reference `operand_pair_ok` var-index rule): both
  // unfold through memory-deriving defs to element indices differing by
  // exactly w (same stream shifted), or repeat a fixed gap recorded at
  // the first divergence (e.g. stream bases a fixed stride apart).
  auto IdxA = varIndex(A, IV, DL);
  auto IdxC = varIndex(C, IV, DL);
  if (IdxA && IdxC && *IdxA != INT64_MAX && *IdxC != INT64_MAX) {
    int64_t Diff = *IdxC - *IdxA;
    if (Diff == W)
      return true;
    auto Key = std::make_pair(Pos, Op);
    auto It = VarPairs.find(Key);
    if (It != VarPairs.end())
      return It->second.first == C && It->second.second == Diff;
    VarPairs[Key] = {C, Diff};
    return true;
  }
  return false;
}

// Try to fold one single-block self-latch loop: find a stride IV,
// partition the body into element slots 0..D-1, verify shape, drop windows
// 1... On success rewrites the step and returns the fold factor D.
//
// Takes the latch header directly (no LoopInfo/SCEV: the pass runs from a
// statically-linked plugin DLL whose analysis keys would mismatch opt's,
// and the reference (`graph::natural_loops`) does its own backedge walk
// anyway).
unsigned tryFoldLoop(BasicBlock *H, SmallVectorImpl<StringRef> &Notes) {
  // Single-block self-latch only: the header branches to itself.
  bool SelfLatch = false;
  for (BasicBlock *S : successors(H))
    if (S == H)
      SelfLatch = true;
  if (!SelfLatch)
    return 0;

  // Collect stride IV candidates.
  struct IVCand {
    PHINode *Phi;
    int64_t Step;
    Instruction *StepInst;
  };
  SmallVector<IVCand, 2> IVs;
  for (Instruction &I : *H) {
    auto *Phi = dyn_cast<PHINode>(&I);
    if (!Phi || !Phi->getType()->isIntegerTy())
      continue;
    if (Phi->getNumIncomingValues() != 2)
      continue;
    int64_t Step = 0;
    Instruction *StepInst = nullptr;
    if (matchStrideIV(Phi, H, Step, StepInst))
      IVs.push_back({Phi, Step, StepInst});
  }
  if (IVs.empty())
    return 0;

  for (auto &IV : IVs) {
    // Accumulator PHIs: int/float/ptr phis other than the IV.
    SmallPtrSet<Value *, 8> Accs;
    for (Instruction &I : *H)
      if (auto *P = dyn_cast<PHINode>(&I))
        if (P != IV.Phi)
          Accs.insert(P);

    // Latch-condition def closure: terminator uses plus their transitive
    // defs are the loop test, never per-element work.
    SmallPtrSet<Value *, 16> LatchDefs;
    SmallVector<Value *, 8> Stack;
    auto *Term = H->getTerminator();
    for (Use &U : Term->operands()) {
      LatchDefs.insert(U.get());
      Stack.push_back(U.get());
    }
    while (!Stack.empty()) {
      Value *V = Stack.pop_back_val();
      if (auto *D = dyn_cast<Instruction>(V))
        if (D->getParent() == H)
          for (Use &U : D->operands())
            if (LatchDefs.insert(U.get()).second)
              Stack.push_back(U.get());
    }

    DefLookup DL;
    for (Instruction &DI : *H)
      if (!DI.getType()->isVoidTy())
        DL.Map[&DI] = &DI;
    DenseMap<Value *, std::optional<int64_t>> Memo, VIMemo;
    DenseMap<int64_t, SmallVector<Instruction *, 8>> Slots;
    SmallVector<Instruction *, 8> Pinned;
    for (Instruction &I : *H) {
      if (isa<PHINode>(&I) || &I == IV.StepInst || LatchDefs.count(&I)) {
        Pinned.push_back(&I);
        continue;
      }
      auto S = slotOf(I, IV.Phi, DL, Memo, VIMemo);
      if (!S) {
        continue; // shared
      } else if (*S == INT64_MAX || *S < 0 || *S >= IV.Step) {
        Pinned.push_back(&I); // next-IV/exit math
      } else {
        Slots[*S].push_back(&I);
      }
    }

    // Move insns consumed by non-adjacent slots out to shared: drop them
    // from their slot (reference: the shared-propagation fixpoint).
    DenseMap<Instruction *, int64_t> InsnSlot;
    for (auto &KV : Slots)
      for (Instruction *I : KV.second)
        InsnSlot[I] = KV.first;
    bool Moved = true;
    while (Moved) {
      Moved = false;
      SmallVector<Instruction *, 8> ToDrop;
      for (auto &KV : Slots) {
        for (Instruction *I : KV.second) {
          Value *D = I;
          bool Bad = false;
          for (User *U : D->users()) {
            auto *UI = dyn_cast<Instruction>(U);
            if (!UI || UI->getParent() != H || isa<PHINode>(UI))
              continue;
            auto It = InsnSlot.find(UI);
            if (It == InsnSlot.end()) {
              // Unslotted non-phi user propagates sharedness unless it
              // only feeds phis (backedge adapter).
              bool OnlyPhi = true;
              for (User *UU : UI->users())
                if (!isa<PHINode>(UU))
                  OnlyPhi = false;
              if (!OnlyPhi) {
                Bad = true;
                break;
              }
            } else if (std::llabs(It->second - KV.first) > 1) {
              Bad = true;
              break;
            }
          }
          if (Bad)
            ToDrop.push_back(I);
        }
      }
      for (Instruction *I : ToDrop) {
        auto It = InsnSlot.find(I);
        if (It == InsnSlot.end())
          continue;
        auto &Vec = Slots[It->second];
        Vec.erase(std::remove(Vec.begin(), Vec.end(), I), Vec.end());
        InsnSlot.erase(I);
        Moved = true;
      }
    }
    SmallVector<int64_t, 8> EmptySlots;
    for (auto &KV : Slots)
      if (KV.second.empty())
        EmptySlots.push_back(KV.first);
    for (int64_t S : EmptySlots)
      Slots.erase(S);

    // Internal insns: pure defs consumed only within their own slot are
    // per-window address math absorbed into pairing; skip positionally.
    SmallPtrSet<Instruction *, 16> Internal;
    for (auto &KV : Slots) {
      for (Instruction *I : KV.second) {
        if (!isPureDef(*I) || I->getType()->isVoidTy())
          continue;
        bool Within = true;
        for (User *U : I->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != H) {
            Within = false;
            break;
          }
          auto It = InsnSlot.find(UI);
          if (It == InsnSlot.end() || It->second != KV.first) {
            Within = false;
            break;
          }
        }
        if (Within)
          Internal.insert(I);
      }
    }
    auto Effective = [&](int64_t S) {
      SmallVector<Instruction *, 8> Out;
      auto It = Slots.find(S);
      if (It == Slots.end())
        return Out;
      for (Instruction *I : It->second)
        if (!Internal.count(I))
          Out.push_back(I);
      return Out;
    };

    // Need D >= 2 occupied slots forming 0, g, 2g, ... and step % D == 0.
    SmallVector<int64_t, 8> Keys;
    for (auto &KV : Slots)
      Keys.push_back(KV.first);
    llvm::sort(Keys);
    if (Keys.size() < 2 || Keys[0] != 0)
      continue;
    int64_t G = Keys[1] - Keys[0];
    if (G <= 0)
      continue;
    bool Arith = true;
    for (size_t i = 0; i < Keys.size(); ++i)
      if (Keys[i] != (int64_t)i * G)
        Arith = false;
    if (!Arith)
      continue;
    int64_t D = (int64_t)Keys.size();
    if (IV.Step % D != 0)
      continue;
    int64_t NewStep = IV.Step / D;
    if (NewStep < 1)
      continue;

    auto W0 = Effective(0);
    std::vector<std::vector<Instruction *>> WSets;
    for (int64_t w = 0; w < D; ++w) {
      auto E = Effective(w * G);
      WSets.emplace_back(E.begin(), E.end());
    }
    if (W0.empty())
      continue;
    bool AnyEmpty = false;
    for (auto &W : WSets)
      if (W.empty())
        AnyEmpty = true;
    if (AnyEmpty)
      continue;

    DenseMap<std::pair<unsigned, unsigned>, int64_t> DeltaMap;
    DenseMap<std::pair<unsigned, unsigned>, std::pair<Value *, int64_t>>
        VarPairs;
    DenseMap<Value *, unsigned> W0Pos;
    for (unsigned p = 0; p < W0.size(); ++p)
      if (!W0[p]->getType()->isVoidTy())
        W0Pos[W0[p]] = p;
    bool ShapeOK = true;
    for (int64_t w = 1; w < D && ShapeOK; ++w) {
      auto &WJ = WSets[(size_t)w];
      if (WJ.size() != W0.size()) {
        ShapeOK = false;
        break;
      }
      for (size_t pos = 0; pos < W0.size() && ShapeOK; ++pos) {
        Instruction *A = W0[pos], *C = WJ[pos];
        if (!(shapeOf(*A) == shapeOf(*C))) {
          ShapeOK = false;
          break;
        }
        if (A->getNumOperands() != C->getNumOperands()) {
          ShapeOK = false;
          break;
        }
        for (unsigned o = 0; o < A->getNumOperands(); ++o) {
          Value *VA = A->getOperand(o), *VC = C->getOperand(o);
          if (VA == VC)
            continue;
          // Same-position self-ref or acc-chain continuation.
          bool Paired = false;
          auto PA = W0Pos.find(VA);
          if (PA != W0Pos.end() && PA->second < WJ.size() &&
              WJ[PA->second] == VC)
            Paired = true;
          // Acc chain: w0 reads the phi; w_w reads w_{w-1}[pos]'s dst.
          if (!Paired && Accs.count(VA)) {
            auto &Prev = WSets[(size_t)w - 1];
            if (pos < Prev.size() && Prev[pos] == VC)
              Paired = true;
          }
          if (!Paired)
            Paired = operandsPair(VA, VC, w, IV.Phi, DL, W0Pos, WSets,
                                  (unsigned)pos, o, DeltaMap, VarPairs);
          if (!Paired) {
            ShapeOK = false;
            break;
          }
        }
      }
    }
    if (!ShapeOK)
      continue;

    // Only the last window's values survive the fold; an earlier
    // window's value read outside the windows blocks the fold.
    DenseMap<Instruction *, int64_t> Body;
    for (auto &KV : Slots)
      for (Instruction *I : KV.second)
        Body[I] = KV.first;
    auto ReadOutside = [&](Value *V) {
      for (User *U : V->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI)
          continue;
        if (UI->getParent() != H)
          return true;
        if (!Body.count(UI))
          return true;
      }
      // Terminator reads outside the windows (latch uses its own defs).
      return false;
    };
    bool LiveBlock = false;
    for (int64_t w = 0; w < D - 1 && !LiveBlock; ++w) {
      auto It = Slots.find(w * G);
      if (It == Slots.end())
        continue;
      for (Instruction *I : It->second) {
        if (I->getType()->isVoidTy())
          continue;
        if (ReadOutside(I)) {
          LiveBlock = true;
          break;
        }
      }
    }
    if (LiveBlock)
      continue;

    // Fold: drop windows 1.., remap last-window dsts to slot-0 dsts,
    // rewrite the step constant.
    DenseMap<Value *, Value *> Remap;
    for (int64_t w = 1; w < D; ++w) {
      auto &WJ = WSets[(size_t)w];
      for (size_t pos = 0; pos < W0.size(); ++pos) {
        Instruction *A = W0[pos], *C = WJ[pos];
        if (!A->getType()->isVoidTy() && !C->getType()->isVoidTy())
          Remap[C] = A;
      }
    }
    SmallPtrSet<Instruction *, 16> Drop;
    for (int64_t w = 1; w < D; ++w) {
      auto It = Slots.find(w * G);
      if (It == Slots.end())
        continue;
      for (Instruction *I : It->second)
        Drop.insert(I);
    }
    // Rewrite the step: keep the sign (add/sub), divide magnitude.
    if (auto *BO = dyn_cast<BinaryOperator>(IV.StepInst)) {
      int64_t Old = cast<ConstantInt>(BO->getOperand(1))->getSExtValue();
      int64_t Sign = Old >= 0 ? 1 : -1;
      BO->setOperand(1, ConstantInt::get(BO->getOperand(1)->getType(),
                                        Sign * NewStep));
    }
    // Remap every use of a dropped effective value to its slot-0
    // counterpart (phi backedges, other blocks, terminators). Uses inside
    // dropped instructions are redirected too; those insns vanish below.
    for (auto &KV : Remap) {
      Value *Old = KV.first, *New = KV.second;
      if (Old == New)
        continue;
      Old->replaceAllUsesWith(New);
    }
    // Erase dropped windows in reverse program order so users vanish
    // before their operands (cycles only pass through kept phis, already
    // remapped above).
    SmallVector<Instruction *, 16> Dead;
    for (auto It = H->rbegin(); It != H->rend(); ++It)
      if (Drop.count(&*It))
        Dead.push_back(&*It);
    for (Instruction *I : Dead)
      I->eraseFromParent();

    // The rewritten IV (phi +/- NewStep) is a simple recurrence by
    // construction; no SCEV query needed.
    Notes.push_back(H->getName());
    return (unsigned)D;
  }
  return 0;
}

} // namespace

PreservedAnalyses RerollPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  SmallVector<StringRef, 8> Notes;
  bool Changed = false;

  // In-loop folding (up to 4 rounds: folding can expose a further fold).
  // Backedges are found directly (no LoopInfo: see tryFoldLoop).
  // Straight-line synthesis lands in the next commit.
  for (int Round = 0; Round < 4; ++Round) {
    bool RoundChanged = false;
    SmallVector<BasicBlock *, 8> Headers;
    for (BasicBlock &B : F) {
      for (BasicBlock *S : successors(&B))
        if (S == &B) {
          Headers.push_back(&B);
          break;
        }
    }
    for (BasicBlock *H : Headers)
      if (tryFoldLoop(H, Notes))
        RoundChanged = true;
    Changed |= RoundChanged;
    if (!RoundChanged)
      break;
  }

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin` interop. revenant-opt links
// statically and does not need it. Guarded to the standalone plugin DLL:
// a static link of every transform would otherwise export
// llvmGetPassPluginInfo more than once. Windows needs the explicit
// dllexport; without it opt fails with "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN
llvm::PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantReroll", "0.1",
          [](llvm::PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](llvm::StringRef Name, llvm::FunctionPassManager &FPM,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "revenant-reroll") {
                    FPM.addPass(revenant::RerollPass());
                    return true;
                  }
                  return false;
                });
          }};
}

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
#endif
