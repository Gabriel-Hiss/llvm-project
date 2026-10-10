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
#include "revenant/Evidence/Evidence.h"
#include "revenant/Transforms/Reroll/RerollFull.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/bit.h"
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
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

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


// Constants in lifted IR include i128 masks. Reroll's affine model is
// signed i64, so reject values and differences it cannot represent.
std::optional<int64_t> signedConstant(const ConstantInt *C) {
  if (!C->getValue().isSignedIntN(64))
    return std::nullopt;
  return C->getSExtValue();
}

std::optional<int64_t> constantDifference(const ConstantInt *A,
                                        const ConstantInt *B) {
  if (!signedConstant(A) || !signedConstant(B))
    return std::nullopt;
  APInt D = B->getValue().sextOrTrunc(65) - A->getValue().sextOrTrunc(65);
  if (!D.isSignedIntN(64))
    return std::nullopt;
  return D.getSExtValue();
}

// Match `Phi = phi [Init, Pre...] [Next, Self]` where Next = Phi +/- Step
// with |Step| in [MinStep, MaxStep]. Returns the step magnitude and the
// increment instruction. The self-latch contributes exactly one incoming
// value; the other incomings are loop entries, and a peeled loop keeps
// several of them (one per path into the header), which the fold carries
// through untouched.
bool matchStrideIV(PHINode *Phi, BasicBlock *Header, int64_t &StepOut,
                   Instruction *&StepInstOut) {
  if (!Phi->getType()->isIntegerTy())
    return false;
  Value *Back = nullptr;
  unsigned NBack = 0;
  for (unsigned i = 0, e = Phi->getNumIncomingValues(); i != e; ++i)
    if (Phi->getIncomingBlock(i) == Header) {
      Back = Phi->getIncomingValue(i);
      ++NBack;
    }
  if (NBack != 1 || !Back)
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
  auto SignedStep = signedConstant(C);
  if (!SignedStep || *SignedStep == INT64_MIN)
    return false;
  int64_t Step = *SignedStep;
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
std::optional<Affine> affineOfGEP(const GetElementPtrInst &G, Value *IV,
                                  const DefLookup &DL, unsigned Depth = 0);

std::optional<Affine> affineOfValue(Value *V, Value *IV, const DefLookup &DL,
                                    unsigned Depth) {
  if (Depth > MaxUnfoldDepth)
    return Affine{0, 0};
  if (V == IV)
    return Affine{1, 0};
  if (auto *C = dyn_cast<ConstantInt>(V)) {
    auto Signed = signedConstant(C);
    if (!Signed)
      return std::nullopt;
    return Affine{0, *Signed};
  }
  if (Instruction *Def = DL.find(V)) {
    if (auto *GDef = dyn_cast<GetElementPtrInst>(Def))
      return affineOfGEP(*GDef, IV, DL, Depth + 1);
    // Only the arith the reference models as affine unfolds; a call or
    // an intrinsic is an opaque leaf (0, 0), not a failed match.
    switch (Def->getOpcode()) {
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::Shl:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::Trunc:
    case Instruction::BitCast:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr: {
      // Arithmetic the model cannot fold is ALSO an opaque leaf, not a
      // failed match. `mul (load i), (load i)` carries no coefficient, so
      // there is nothing to scale; but its operands still carry the
      // element index, and a consumer that needs that index reaches it
      // through the bare-var rule only if the mul reads as a leaf.
      // Returning nullopt here hid the index from every consumer, which
      // is why a multi-accumulator unroll's per-window accumulators never
      // resolved to a slot and never folded.
      auto A = affineOfInsn(*Def, IV, DL, Depth + 1);
      return A ? *A : Affine{0, 0};
    }
    default:
      return Affine{0, 0}; // Load/Phi/Call/... are opaque leaves.
    }
  }
  return Affine{0, 0}; // Params, globals: opaque leaves.
}

// Combined affine of an instruction (the reference `affine_of` over the
// instruction's own arithmetic). A compare has no affine; a div/rem/and/
// or/xor/shr has none either (the reference `affine_of` returns None for
// them), and the caller then sees a failed match rather than a bogus one.
// GEPs go through affineOfGEP, reached before this.
std::optional<Affine> affineOfInsn(const Instruction &I, Value *IV,
                                    const DefLookup &DL, unsigned Depth) {
  if (isa<CmpInst>(&I))
    return std::nullopt;
  if (auto *G = dyn_cast<GetElementPtrInst>(&I))
    return affineOfGEP(*G, IV, DL, Depth);
  auto Operand = [&](unsigned K) {
    return affineOfValue(I.getOperand(K), IV, DL, Depth);
  };
  switch (I.getOpcode()) {
  case Instruction::Add: {
    // The reference sums the operand exprs; LLVM only allows extra
    // operands on a call, so a two-operand add is the whole story.
    if (I.getNumOperands() != 2)
      return std::nullopt;
    auto A = Operand(0), B = Operand(1);
    if (!A || !B)
      return std::nullopt;
    return Affine{A->first + B->first, A->second + B->second};
  }
  case Instruction::Sub: {
    if (I.getNumOperands() != 2)
      return std::nullopt;
    auto A = Operand(0), B = Operand(1);
    if (!A || !B)
      return std::nullopt;
    return Affine{A->first - B->first, A->second - B->second};
  }
  case Instruction::Mul:
  case Instruction::Shl: {
    // One side must be a constant: it scales (or shifts) the other.
    if (I.getNumOperands() != 2)
      return std::nullopt;
    for (unsigned K = 0; K < 2; ++K) {
      auto *C = dyn_cast<ConstantInt>(I.getOperand(K));
      if (!C)
        continue;
      if (I.getOpcode() == Instruction::Shl) {
        if (!C->getValue().isIntN(32))
          return std::nullopt;
      }
      auto Other = affineOfValue(I.getOperand(1 - K), IV, DL, Depth);
      if (!Other)
        return std::nullopt;
      auto Signed = signedConstant(C);
      if (!Signed)
        return std::nullopt;
      int64_t Factor = I.getOpcode() == Instruction::Shl
                           ? (int64_t(1) << C->getZExtValue())
                           : *Signed;
      return Affine{Other->first * Factor, Other->second * Factor};
    }
    return std::nullopt;
  }
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
  case Instruction::BitCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr: {
    // Pass-through: the reference `affine_of` strips these.
    if (I.getNumOperands() != 1)
      return std::nullopt;
    return Operand(0);
  }
  default:
    return std::nullopt;
  }
}

// Affine of a GEP def: base + Σ index_k * sizeof(result element), matching
// the reference `Expr::Op("gep", [base, mul(idx, sz)])` unfolded through
// `affine_of`.
std::optional<Affine> affineOfGEP(const GetElementPtrInst &G, Value *IV,
                                  const DefLookup &DL, unsigned Depth) {
  int64_t Scale =
      (int64_t)G.getResultElementType()->getScalarSizeInBits() / 8;
  if (Scale <= 0)
    Scale = 1;
  auto Base =
      affineOfValue(const_cast<Value *>(G.getPointerOperand()), IV, DL, Depth + 1);
  if (!Base)
    return std::nullopt;
  Affine Acc = *Base;
  for (Value *Ix : G.indices()) {
    auto O = affineOfValue(const_cast<Value *>(Ix), IV, DL, Depth + 1);
    if (!O)
      return std::nullopt;
    Acc.first += O->first * Scale;
    Acc.second += O->second * Scale;
  }
  return Acc;
}

// Element slot of a value through a memory-deriving (Load/Gep/Cast/Bin)
// def, mirroring the reference `var_index`: the combined affine of the
// def's own operands. None when not IV-derived, poison (INT64_MAX) when
// off a clean boundary.
//
// The reference stops at the affine (a mul of two loads is affine (0, 0),
// so it carries no index). Recovering a rolled accumulator chain needs
// the index of `mul(load(i), load(i))` too, so when the affine is
// degenerate the operands' own indices are consulted: they must all
// agree, and that common index is the def's index.
std::optional<int64_t> varIndex(Value *V, Value *IV, const DefLookup &DL,
                                unsigned Depth = 0) {
  if (Depth > MaxUnfoldDepth)
    return std::nullopt;
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
  if (A && A->first != 0) {
    if (A->second % A->first != 0)
      return INT64_MAX;
    return A->second / A->first;
  }
  // Degenerate affine: consult the operands. `nullopt` here means "no
  // index in any operand", which is the reference answer too.
  std::optional<int64_t> Common;
  for (const Use &U : Def->operands()) {
    if (isa<Constant>(U.get()))
      continue;
    auto D = varIndex(U.get(), IV, DL, Depth + 1);
    if (!D)
      return std::nullopt;
    if (*D == INT64_MAX)
      return INT64_MAX;
    if (!Common)
      Common = D;
    else if (*Common != *D)
      return INT64_MAX;
  }
  return Common;
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
                           std::pair<Value *, int64_t>> &VarPairs,
                  const SmallPtrSetImpl<Value *> &Accs,
                  DenseMap<std::pair<unsigned, unsigned>,
                           SmallPtrSet<Value *, 2>> &AccPairs) {
  if (A == C)
    return true;
  auto *CA = dyn_cast<ConstantInt>(A);
  auto *CC = dyn_cast<ConstantInt>(C);
  if (CA && CC) {
    auto Difference = constantDifference(CA, CC);
    if (!Difference)
      return false;
    int64_t D = *Difference;
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
    if (D == INT64_MIN && It->second == -1)
      return false;
    // Linear shift (d == w*d1) or a periodic multiple of it.
    return D % It->second == 0 && D / It->second <= W;
  }
  if (isa<Constant>(A) || isa<Constant>(C))
    return false;
  // Multi-accumulator unroll: window 0's acc add reads its own acc phi
  // and window w's reads a *different* one (`acc0 += a[i]`,
  // `acc1 += a[i+1]`, merged after the loop). The phis must be pairwise
  // distinct per window so the pairing identifies a genuine per-window
  // accumulator set rather than two unrelated ones. The window offset is
  // sometimes folded into the acc value instead
  // (`acc1' = acc1 + 1`), which also pairs.
  if (isa<PHINode>(A) && Accs.count(A)) {
    Value *AccC = C;
    int64_t Offset = 0;
    if (auto *BO = dyn_cast<BinaryOperator>(C))
      if (BO->getOpcode() == Instruction::Add && BO->getParent() &&
          BO->getParent() == dyn_cast<Instruction>(A)->getParent()) {
        for (unsigned K = 0; K < 2; ++K) {
          if (isa<PHINode>(BO->getOperand(K)) &&
              Accs.count(BO->getOperand(K)) && BO->getOperand(K) != A) {
            AccC = BO->getOperand(K);
            auto *CO = dyn_cast<ConstantInt>(BO->getOperand(1 - K));
            if (!CO)
              return false;
            Offset = CO->getSExtValue();
          }
        }
      }
    if (!isa<PHINode>(AccC) || !Accs.count(AccC))
      return false;
    if (Offset != 0 && Offset != W)
      return false;
    auto Key = std::make_pair(Pos, Op);
    auto &Seen = AccPairs[Key];
    if (Seen.empty())
      Seen.insert(A);
    if (!Seen.insert(AccC).second)
      return false; // the same acc phi twice: not a per-window set
    return true;
  }
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
        if (!operandsPair(const_cast<Value *>(IA2->get()),
                          const_cast<Value *>(IC2->get()), W, IV, DL, W0Pos,
                          WSets, Pos, Op, Deltas, VarPairs, Accs, AccPairs))
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

// ---------------------------------------------------------------------------
// Step-instruction discipline.
//
// The fold rewrites StepInst's constant in place (add/sub by the divided
// step), so every user of StepInst that SURVIVES the fold observes the
// divided step, which changes what it computes. The other users are legal
// only when the fold erases them all.
//
// `Erased` is a dead-user closure: seeded with the dropped windows and the
// accumulator-collapse adds the fold is about to perform, then closed under
// "every user is already in the set". Window membership alone is not enough
// — address math in a dropped window can feed a kept slot-0 instruction,
// and the remap substitutes only dropped values, so that kept instruction
// would silently read the divided step.
//
// The collapse is modelled, not run: collapse erases an `add Old,New`
// outside the header whose operands are all the kept update (New) and the
// dropped ones (Old, already in Remap) of the same window position, where
// New itself reads a loop phi. Predicting it keeps this a pure pre-mutation
// gate, so a bail leaves the IR exactly as found.
// ---------------------------------------------------------------------------
bool stepUsersFolded(BasicBlock *H, Instruction *StepInst, PHINode *Phi,
                     const SmallPtrSetImpl<Instruction *> &Drop,
                     const DenseMap<Value *, Value *> &Remap,
                     const SmallPtrSetImpl<Value *> &Accs) {
  SmallPtrSet<Instruction *, 32> Erased(Drop.begin(), Drop.end());
  // Predicted collapse erasures: the add/fadd users of dropped values whose
  // operands are all {New, Old} of the same remap entry, where New reads a
  // loop phi. Mirrors the collapse loop below but only records the set.
  for (auto &KV : Remap) {
    Value *Old = KV.first, *New = KV.second;
    auto *NewI = dyn_cast<Instruction>(New);
    if (!NewI || Drop.count(NewI))
      continue;
    bool KeptReadsPhi = false;
    for (const Use &U : NewI->operands())
      if (isa<PHINode>(U.get())) {
        KeptReadsPhi = true;
        break;
      }
    if (!KeptReadsPhi)
      continue;
    for (User *U : Old->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() == H || UI->getType()->isVoidTy())
        continue;
      if (UI->getOpcode() != Instruction::Add &&
          UI->getOpcode() != Instruction::FAdd)
        continue;
      bool AllSame = true;
      for (const Use &O : UI->operands())
        if (O.get() != New && O.get() != Old) {
          AllSame = false;
          break;
        }
      if (AllSame)
        Erased.insert(UI);
    }
  }
  // Close the set: an instruction is erased when every one of its users is
  // already erased. Iterate to a fixed point over the header's
  // instructions; the closure is small and terminates because each round
  // only adds.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : *H) {
      if (Erased.count(&I) || isa<PHINode>(&I) || &I == StepInst)
        continue;
      if (I.use_empty())
        continue; // already dead for its own reasons; not our concern here
      bool AllErased = true;
      for (User *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || !Erased.count(UI)) {
          AllErased = false;
          break;
        }
      }
      if (AllErased) {
        Erased.insert(&I);
        Changed = true;
      }
    }
  }
  // Every user of the step must be erased, or be the induction phi, or be
  // the exit compare's own use of the step (the branch that exitTrip-
  // Compatible already proved). The compare itself is in LatchDefs and
  // therefore pinned, and its condition reads the step, so that read is
  // permitted and is exactly the trip count being preserved.
  for (User *U : StepInst->users()) {
    if (U == Phi)
      continue;
    if (auto *UI = dyn_cast<Instruction>(U))
      if (Erased.count(UI))
        continue;
    // The latch terminator's condition may read the step through its own
    // def chain; that is the proved exit, not a surviving consumer.
    if (auto *UI = dyn_cast<Instruction>(U)) {
      if (auto *Br = dyn_cast<BranchInst>(H->getTerminator()))
        if (Br->isConditional() && Br->getCondition() == UI)
          continue;
    }
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Exit legality for a fold.
//
// Folding divides the IV step from S to S/D and keeps the latch condition
// untouched, so the folded index walks the same exit with D times as many
// steps. Two facts make that legal, and both have to be proved from how the
// values are built:
//
//   1. dense coverage, Step == D * G (checked at the call site): every
//      element the unrolled body touched, the folded body still touches.
//   2. the exit operand sits on the induction lattice modulo the ORIGINAL
//      step S, for every loop entry. The unrolled loop exits at
//      `entry + k*S`; the folded one at `entry + k*(S/D)`, which is the
//      same value only when the exit was a multiple of S away from the
//      entry to begin with.
//
// Nothing here looks at values a runtime argument may take. A load, a
// parameter and an unknown call are never a proved multiple, so loops whose
// legality is only true for some inputs bail and stay unrolled.
// ---------------------------------------------------------------------------

constexpr unsigned MaxMultipleDepth = 8;

// True when V is provably a multiple of S (S >= 2) by construction, in
// the integer domain V actually lives in. This is where the lattice
// matters: an i8 unsigned bound of 253 is not divisible by 3, but reading
// it as a signed i64 turns it into -3, which is, so every multiple test
// below works on the raw bits of V's own type and only then compares.
//
// Recognized constructions: a constant multiple, a multiply or left shift
// by a multiple of S, a mask whose low log2(S) bits are zero (S a power of
// two), a sum of multiples, or a widening of a multiple. Bit-preserving
// casts pass through because they leave the low bits alone.
bool provedMultiple(Value *V, int64_t S, unsigned Depth = 0) {
  if (S <= 1)
    return true;
  if (Depth > MaxMultipleDepth)
    return false;
  // The raw bits, interpreted in V's own width.
  auto Multiple = [&](const APInt &Bits) {
    if (Bits.getBitWidth() > 64)
      return false;
    // S is a positive i64 that fits in Bits' width (the caller only ever
    // passes values the loop can produce in that width).
    if (S < 0 || !Bits.getBitWidth() ||
        (uint64_t)S > (uint64_t{1} << (Bits.getBitWidth() - 1)))
      return false;
    APInt Div = APInt(Bits.getBitWidth(), (uint64_t)S);
    APInt Rem(Bits.getBitWidth(), 0);
    APInt::udivrem(Bits, Div, Rem, Rem);
    return Rem.isZero();
  };
  if (auto *C = dyn_cast<ConstantInt>(V))
    return Multiple(C->getValue());
  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;
  switch (I->getOpcode()) {
  case Instruction::Mul: {
    // (a * b) % S == 0 when either factor is a multiple, or when a constant
    // factor is itself a multiple of S.
    for (unsigned K = 0; K < 2; ++K) {
      auto *C = dyn_cast<ConstantInt>(I->getOperand(K));
      if (C && Multiple(C->getValue()))
        return true;
    }
    return provedMultiple(I->getOperand(0), S, Depth + 1) ||
           provedMultiple(I->getOperand(1), S, Depth + 1);
  }
  case Instruction::Shl: {
    auto *C = dyn_cast<ConstantInt>(I->getOperand(1));
    if (!C)
      return false;
    if (!C->getValue().isIntN(32) || C->getValue().isNegative())
      return false;
    // Only a power-of-two modulus has a shift proving it: 2^Sh is a
    // multiple of S iff S divides 2^Sh.
    if (S < 0 || (S & (S - 1)) != 0)
      return false;
    uint64_t Sh = C->getZExtValue();
    if (Sh > 62)
      return false;
    uint64_t Factor = uint64_t(1) << Sh;
    if ((uint64_t)S > Factor || Factor % (uint64_t)S != 0)
      return false;
    return true; // any value shifted left by >= log2(S)
  }
  case Instruction::And: {
    // `and X, m` clears the low log2(S) bits exactly when m does, and a
    // power-of-two modulus is decided by those bits alone.
    if (S < 0 || (S & (S - 1)) != 0)
      return false;
    for (unsigned K = 0; K < 2; ++K) {
      auto *C = dyn_cast<ConstantInt>(I->getOperand(K));
      if (!C)
        continue;
      APInt M = C->getValue();
      if (M.getBitWidth() >= 64 || S < 0)
        return false;
      APInt Low = APInt(M.getBitWidth(), (uint64_t)(S - 1));
      if ((M & Low).isZero())
        return true;
    }
    return false;
  }
  case Instruction::Add:
    return provedMultiple(I->getOperand(0), S, Depth + 1) &&
           provedMultiple(I->getOperand(1), S, Depth + 1);
  case Instruction::Sub:
    return provedMultiple(I->getOperand(0), S, Depth + 1) &&
           provedMultiple(I->getOperand(1), S, Depth + 1);
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::BitCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr: {
    // Preserve (or widen) the low bits, so a multiple stays a multiple,
    // provided S still fits in the narrow type.
    Value *Op = I->getOperand(0);
    unsigned Bits = Op->getType()->getIntegerBitWidth();
    if (Bits == 0 || Bits >= 64)
      return false;
    if ((uint64_t)S >= (uint64_t{1} << (Bits - 1)))
      return false;
    return provedMultiple(Op, S, Depth + 1);
  }
  case Instruction::Trunc: {
    // Only safe when the low log2(S) bits survive the truncation.
    if (S < 0 || (S & (S - 1)) != 0)
      return false;
    Value *Op = I->getOperand(0);
    unsigned Bits = I->getType()->getIntegerBitWidth();
    if (Bits == 0 || Bits >= 64)
      return false;
    if ((uint64_t)S >= (uint64_t{1} << (Bits - 1)))
      return false;
    return provedMultiple(Op, S, Depth + 1);
  }
  case Instruction::PHI: {
    auto *PN = cast<PHINode>(I);
    for (unsigned K = 0, E = PN->getNumIncomingValues(); K != E; ++K)
      if (!provedMultiple(PN->getIncomingValue(K), S, Depth + 1))
        return false;
    return true;
  }
  default:
    return false;
  }
}

// True when Bound - Entry is provably a multiple of S: both ends are, or
// Bound is built as Entry plus a proved multiple (the pointer-loop form
// `end = start + n * stride`).
bool provedMultipleDiff(Value *Bound, Value *Entry, int64_t S) {
  if (Bound == Entry)
    return true;
  if (provedMultiple(Bound, S) && provedMultiple(Entry, S))
    return true;
  auto *I = dyn_cast<Instruction>(Bound);
  if (!I)
    return false;
  if (I->getOpcode() == Instruction::Add) {
    for (unsigned K = 0; K < 2; ++K)
      if (I->getOperand(K) == Entry &&
          provedMultiple(I->getOperand(1 - K), S))
        return true;
  } else if (I->getOpcode() == Instruction::Sub) {
    if (I->getOperand(0) == Entry && provedMultiple(I->getOperand(1), S))
      return true;
  }
  return false;
}

// Which operand of the exit compare walks the induction, and how.
struct ExitSide {
  bool Valid = false;
  bool TestsStep = false; // the compare reads the step value, not the index
  int64_t Offset = 0;     // constant offset on top of the index
  bool HasOffset = false; // the operand is `index + offset`, not the index
};

// Proven legality of one folding candidate. `H` is the self-latch header,
// `IV`/`StepInst` its induction and step, `Step` the ORIGINAL step
// magnitude and `D` the window count. Returns false when the fold would
// change the set of iterations the loop performs.
//
// When the exit tests a *coupled* counter rather than the induction
// (`%trip = phi [%start], [%trip - 1]`, `icmp eq %trip.next, 0`), the fold
// is still legal — but only if the caller also rescales the counter: the
// rolled loop runs D times as many iterations, so a counter whose step is
// a constant ±1 needs its entry multiplied by D. That is provable only
// when every entry is a constant (the trip count is then exactly that
// constant) and the counter has no user besides its step and the compare
// (rescaling changes the values it walks). `CoupledTrip` reports such a
// counter; it stays null when the compare already walks the induction.
bool exitTripCompatible(BasicBlock *H, PHINode *IV, Instruction *StepInst,
                        int64_t Step, int64_t D, const DefLookup &DL,
                        Instruction *Term, PHINode *&CoupledTrip) {
  CoupledTrip = nullptr;
  auto *Br = dyn_cast<BranchInst>(Term);
  if (!Br || !Br->isConditional())
    return false;
  auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
  if (!Cmp || Cmp->getParent() != H)
    return false; // a value flow we do not model: leave the loop alone

  Value *A = Cmp->getOperand(0), *B = Cmp->getOperand(1);
  ExitSide Side;
  // A coupled trip counter: the exit tests another PHI's backedge step
  // against zero (`%trip = phi [%start], [%trip - 1]`, `icmp eq %trip.next,
  // 0`). This is the shape lifted c-o2 `dot4` reaches the fold stage with:
  // `%rax.0` is the element induction and `%r11.0.i32` the coupled
  // countdown, exited on `%8 == 0`.
  //
  // Checked before the induction classifier, because neither operand walks
  // the folded induction and the classifier would reject the compare as
  // "not a trip count".
  if (auto *AS = dyn_cast<BinaryOperator>(A)) {
    if ((AS->getOpcode() == Instruction::Add ||
         AS->getOpcode() == Instruction::Sub) &&
        AS->getParent() == H) {
      Value *Other = AS->getOperand(0) == AS->getOperand(1)
                         ? nullptr
                         : AS->getOperand(0);
      auto *BP = dyn_cast_or_null<PHINode>(Other);
      if (BP && BP != IV && BP->getType()->isIntegerTy() &&
          isa<ConstantInt>(B) && cast<ConstantInt>(B)->isZero() &&
          (Cmp->getPredicate() == ICmpInst::ICMP_EQ ||
           Cmp->getPredicate() == ICmpInst::ICMP_NE)) {
        // The counter's backedge must be exactly this add/sub, and it must
        // step by 1 so its trip count is its entry value.
        Value *CBack = nullptr;
        for (unsigned K = 0, E = BP->getNumIncomingValues(); K != E; ++K)
          if (BP->getIncomingBlock(K) == H)
            CBack = BP->getIncomingValue(K);
        if (CBack != AS)
          return false;
        auto *CO = dyn_cast<ConstantInt>(
            AS->getOperand(0) == BP ? AS->getOperand(1)
                                    : AS->getOperand(1) == BP
                                          ? AS->getOperand(0)
                                          : nullptr);
        // `sub phi, 1` and `add phi, -1` are both unit countdown steps: the
        // trip count is |entry| either way and the folded loop runs D times
        // as many of them.
        if (!CO || CO->getSExtValue() != 1 && CO->getSExtValue() != -1)
          return false;
        // Every entry must be a constant, and the counter may have no user
        // but its own step: rescaling walks a different value sequence, so
        // any other reader would observe it.
        for (unsigned K = 0, E = BP->getNumIncomingValues(); K != E; ++K) {
          if (BP->getIncomingBlock(K) == H)
            continue;
          auto *Entry = dyn_cast<ConstantInt>(BP->getIncomingValue(K));
          if (!Entry || Entry->getSExtValue() < D)
            return false;
        }
        for (User *U : BP->users())
          if (U != AS)
            return false;
        // The add/sub itself may only feed the counter and the compare.
        for (User *U : AS->users())
          if (U != BP && U != Cmp)
            return false;
        CoupledTrip = BP;
        return true;
      }
    }
  }
  auto classify = [&](Value *V) -> bool {
    Side = ExitSide();
    if (V == IV) {
      Side.Valid = true;
      return true;
    }
    if (V == StepInst) {
      Side.Valid = true;
      Side.TestsStep = true;
      return true;
    }
    // `index + C` where C is a constant: the exit lands C away from the
    // index, so C has to sit on the lattice too.
    if (auto *I = dyn_cast<Instruction>(V)) {
      if (isa<ICmpInst>(I) || isa<PHINode>(I))
        return false;
      auto Aff = affineOfInsn(*I, IV, DL, 0);
      if (!Aff || (Aff->first != 1 && Aff->first != -1))
        return false;
      if (Aff->second % Step != 0)
        return false;
      Side.Valid = true;
      Side.Offset = Aff->second;
      Side.HasOffset = true;
      Side.TestsStep = false;
      return true;
    }
    return false;
  };
  if (!classify(A)) {
    if (!classify(B))
      return false;
    std::swap(A, B);
  } else if (classify(B)) {
    return false; // both operands walk the induction: not a trip count
  }
  // Once classified, A is the induction side and B the bound.
  Value *Bound = B;

  SmallVector<Value *, 4> Entries;
  for (unsigned K = 0, E = IV->getNumIncomingValues(); K != E; ++K)
    if (IV->getIncomingBlock(K) != H)
      Entries.push_back(IV->getIncomingValue(K));
  if (Entries.empty())
    return false;

  switch (Cmp->getPredicate()) {
  case ICmpInst::ICMP_EQ:
  case ICmpInst::ICMP_NE: {
    // `index (+ offset) == bound`: the original exits when the induction
    // has advanced to `bound`, so `bound` has to be a multiple of S away
    // from every entry. The folded induction then reaches the very same
    // value in exactly D times as many steps.
    for (Value *E : Entries)
      if (!provedMultipleDiff(Bound, E, Step))
        return false;
    break;
  }
  case ICmpInst::ICMP_ULT:
  case ICmpInst::ICMP_ULE:
  case ICmpInst::ICMP_SLT:
  case ICmpInst::ICMP_SLE: {
    if (Side.TestsStep || Side.HasOffset)
      return false;
    for (Value *E : Entries)
      if (!provedMultiple(E, Step))
        return false;
    if (!provedMultiple(Bound, Step))
      return false;
    break;
  }
  default:
    return false;
  }

  // A latch-tested loop executes its body before checking the exit. Folding
  // D windows changes the iteration count by D, so prove the original loop
  // survives through its first D body executions. Constant entry/bound pairs
  // are exact; runtime values remain untouched unless separately proven.
  if (Br->getSuccessor(0) == H || Br->getSuccessor(1) == H) {
    bool ContinueOnTrue = Br->getSuccessor(0) == H;
    for (Value *E : Entries) {
      auto *EntryC = dyn_cast<ConstantInt>(E);
      auto *BoundC = dyn_cast<ConstantInt>(Bound);
      if (!EntryC || !BoundC)
        return false;
      APInt At = EntryC->getValue();
      for (int64_t I = 1; I < D; ++I)
        At += APInt(At.getBitWidth(), Step);
      bool Continue = ICmpInst::compare(
          At, BoundC->getValue(), Cmp->getPredicate());
      if (Continue != ContinueOnTrue)
        return false;
    }
  }
  return true;
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
  if (!SelfLatch) {
    return 0;
  }

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
    // Dense coverage: the windows must tile the whole step. Sparse windows
    // (Step > D * G) leave elements between them untouched, and the folded
    // body — one window per iteration — would skip exactly those.
    if (NewStep != G)
      continue;
    // The fold divides the step and leaves the latch condition alone, so
    // the exit has to be proved reachable on the same lattice; see
    // exitTripCompatible. An unproved trip count means the folded loop
    // would run a different number of times, so the loop stays unrolled.
    // A coupled countdown (`icmp eq %trip.next, 0`) is legal only when the
    // counter's entry is a constant the fold rescales below.
    PHINode *CoupledTrip = nullptr;
    if (!exitTripCompatible(H, IV.Phi, IV.StepInst, IV.Step, D, DL, Term,
                            CoupledTrip))
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
    // Per (window position, operand): the acc phis already paired there,
    // so a multi-accumulator unroll's phis stay pairwise distinct.
    DenseMap<std::pair<unsigned, unsigned>, SmallPtrSet<Value *, 2>> AccPairs;
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
                                  (unsigned)pos, o, DeltaMap, VarPairs, Accs,
                                  AccPairs);
          if (!Paired) {
            ShapeOK = false;
            break;
          }
        }
      }
    }
    if (!ShapeOK)
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

    // The escapes the fold can absorb. A dropped window's value may leave
    // the loop only into the accumulator-collapse add: the merge that sums
    // the per-window accumulators after the loop, whose operands are all
    // the kept update and the dropped ones of one remap entry. Every other
    // escape would survive the fold reading a value the fold rewrote, so
    // it blocks the fold. Computed before any mutation so a bail leaves
    // the IR exactly as found.
    //
    // This is the check that covers the LAST window, and therefore the
    // D == 2 case: a two-window fold's only dropped window is the last
    // one, and an escape it cannot absorb is exactly the shape that leaves
    // two accumulators summing the same products after the remap.
    SmallPtrSet<Instruction *, 8> CollapseTargets;
    for (auto &KV : Remap) {
      Value *Old = KV.first, *New = KV.second;
      auto *NewI = dyn_cast<Instruction>(New);
      if (!NewI || !NewI->getType()->isIntegerTy() &&
                      !NewI->getType()->isFloatingPointTy())
        continue;
      bool KeptReadsPhi = false;
      for (const Use &U : NewI->operands())
        if (isa<PHINode>(U.get())) {
          KeptReadsPhi = true;
          break;
        }
      if (!KeptReadsPhi)
        continue;
      for (User *U : Old->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || UI->getParent() == H || UI->getType()->isVoidTy())
          continue;
        if (UI->getOpcode() != Instruction::Add &&
            UI->getOpcode() != Instruction::FAdd)
          continue;
        bool AllSame = true;
        for (const Use &O : UI->operands())
          if (O.get() != New && O.get() != Old) {
            AllSame = false;
            break;
          }
        if (AllSame)
          CollapseTargets.insert(UI);
      }
    }

    // Only the last window's values survive the fold (as their slot-0
    // counterparts); a value of a *dropped* window read outside the
    // windows blocks the fold. Window 0 is kept, so its values may be
    // read outside freely — that is the ordinary accumulator case.
    DenseMap<Instruction *, int64_t> Body;
    for (auto &KV : Slots)
      for (Instruction *I : KV.second)
        Body[I] = KV.first;
    bool LiveBlock = false;
    for (int64_t w = 1; w < D && !LiveBlock; ++w) {
      auto It = Slots.find(w * G);
      if (It == Slots.end())
        continue;
      bool LastWindow = (w == D - 1);
      for (Instruction *I : It->second) {
        if (I->getType()->isVoidTy())
          continue;
        for (User *U : I->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI)
            continue;
          if (UI->getParent() != H) {
            // An escape out of the loop. An intermediate window has no
            // post-loop equivalent for a mid-loop value, so any escape
            // blocks the fold. The last window does have one — the rolled
            // accumulator — and the fold supplies it two ways: the
            // accumulator-collapse add, and any PHI whose incoming is the
            // dropped value (the remap retargets it to the kept update).
            if (!LastWindow ||
                (!CollapseTargets.count(UI) && !isa<PHINode>(UI))) {
              LiveBlock = true;
              break;
            }
            continue;
          }
          // An in-header reader that is not part of the windows. A
          // backedge phi reading the last window's accumulator is the
          // ordinary merge shape and the remap retargets it; an
          // intermediate window feeding a phi has no equivalent, so it
          // still blocks the fold.
          if (!Body.count(UI) && !(LastWindow && isa<PHINode>(UI))) {
            LiveBlock = true;
            break;
          }
        }
        if (LiveBlock)
          break;
      }
    }
    if (LiveBlock)
      continue;
    SmallPtrSet<Instruction *, 16> Drop;
    for (int64_t w = 1; w < D; ++w) {
      auto It = Slots.find(w * G);
      if (It == Slots.end())
        continue;
      for (Instruction *I : It->second)
        Drop.insert(I);
    }
    // Extend the drop set with the dead-user closure: once the windows are
    // gone, the address math that fed ONLY them is dead too, and it has to
    // be erased rather than left behind reading the divided step.
    //
    // This is what a lifted dot4 needs. Its window-1 addresses run
    // `shl (%iv + 2), 2` -> `gep i8` -> `gep i32 -1`, i.e. straight off the
    // step instruction, and the step's constant is exactly what the fold
    // divides. Without the closure the shl survives and computes
    // `(%iv + 1) * 4` for an iteration that no longer exists.
    //
    // The closure never reaches the induction phi, the step instruction, or
    // the latch-condition chain: each of those keeps a live user, so
    // "every user is already dropped" fails for them.
    {
      SmallPtrSet<Instruction *, 32> ErasedChain;
      for (Instruction *I : Drop)
        ErasedChain.insert(I);
      bool Grew = true;
      while (Grew) {
        Grew = false;
        for (Instruction &I : *H) {
          if (ErasedChain.count(&I) || isa<PHINode>(&I) ||
              &I == IV.StepInst || LatchDefs.count(&I))
            continue;
          if (I.use_empty())
            continue; // already dead for its own reasons
          bool AllDropped = true;
          for (User *U : I.users()) {
            auto *UI = dyn_cast<Instruction>(U);
            if (!UI || !ErasedChain.count(UI)) {
              AllDropped = false;
              break;
            }
          }
          if (AllDropped) {
            ErasedChain.insert(&I);
            Grew = true;
          }
        }
      }
      for (Instruction *I : ErasedChain)
        Drop.insert(I);
    }
    // Step-instruction discipline. The fold rewrites the step's constant
    // in place, so every user of the step that SURVIVES the fold observes
    // the divided step, which is wrong. The other users are legal only
    // when the fold erases them all.
    //
    // The erased set is a dead-user closure, not window membership. Seeded
    // with the dropped windows plus the accumulator-collapse adds the fold
    // is about to perform (modelled here, before any mutation, so a bail
    // leaves the IR untouched), then closed under "every user is already
    // in the set". Window membership alone would leak: address math in a
    // dropped window can feed a kept slot-0 instruction, and remap only
    // substitutes dropped values, so that kept instruction would silently
    // read the divided step. Predict the collapse rather than running it:
    // a post-collapse bail would leave the collapse's own mutations
    // standing on a no-fire path.
    if (!stepUsersFolded(H, IV.StepInst, IV.Phi, Drop, Remap, Accs))
      continue;
    // Collapse the unroll's accumulator merge. A multi-accumulator unroll
    // sums its per-window accumulator values after the loop; rolled, they
    // are one accumulator, so the merge outside the header must drop its
    // dropped-window terms. The guard is tight: an add outside the header
    // whose operands are all the kept accumulator update (New) and the
    // dropped ones (Old, being replaced by New) of the same window
    // position, where the kept update itself reads a loop phi.
    bool Collapsed = true;
    while (Collapsed) {
      Collapsed = false;
      for (auto &KV : Remap) {
        Value *Old = KV.first, *New = KV.second;
        if (Old->use_empty())
          continue;
        auto *NewI = dyn_cast<Instruction>(New);
        if (!NewI || Drop.count(NewI))
          continue;
        bool KeptReadsPhi = false;
        for (const Use &U : NewI->operands())
          if (isa<PHINode>(U.get())) {
            KeptReadsPhi = true;
            break;
          }
        if (!KeptReadsPhi)
          continue;
        for (User *U : Old->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || Drop.count(UI) || UI == IV.StepInst ||
              UI->getParent() == H || UI->getType()->isVoidTy())
            continue;
          if (UI->getOpcode() != Instruction::Add &&
              UI->getOpcode() != Instruction::FAdd)
            continue;
          bool AllSame = true;
          for (const Use &O : UI->operands())
            if (O.get() != New && O.get() != Old) {
              AllSame = false;
              break;
            }
          if (!AllSame)
            continue;
          UI->replaceAllUsesWith(New);
          UI->eraseFromParent();
          Collapsed = true;
          break;
        }
        if (Collapsed)
          break;
      }
    }
    // Rewrite the step: keep the sign (add/sub), divide magnitude.
    if (auto *BO = dyn_cast<BinaryOperator>(IV.StepInst)) {
      int64_t Old = cast<ConstantInt>(BO->getOperand(1))->getSExtValue();
      int64_t Sign = Old >= 0 ? 1 : -1;
      BO->setOperand(1, ConstantInt::get(BO->getOperand(1)->getType(),
                                        Sign * NewStep));
    }
    // Rescale the coupled countdown: the rolled loop runs D times as many
    // iterations, so its entry (proved a constant by
    // exitTripCompatible) becomes entry * D. Its step stays ±1, so the
    // `icmp eq %trip.next, 0` exit still fires exactly at the end.
    if (CoupledTrip)
      for (unsigned K = 0, E = CoupledTrip->getNumIncomingValues(); K != E;
           ++K) {
        if (CoupledTrip->getIncomingBlock(K) == H)
          continue;
        auto *C = dyn_cast<ConstantInt>(CoupledTrip->getIncomingValue(K));
        if (!C)
          continue;
        CoupledTrip->setIncomingValue(
            K, ConstantInt::get(C->getType(), C->getSExtValue() * D));
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
    // Erase the dropped set in reverse DEPENDENCY order. Reverse program
    // order is not the same thing once the dead closure is in the set: a
    // window's address math (the GEP and the load) is defined before the
    // dropped add that consumes it, so program order would try to erase
    // the load while its user is still live. Erasing whatever has become
    // use-empty, repeatedly, is the dependency order and needs no sort.
    bool AnyLeft = true;
    while (AnyLeft) {
      AnyLeft = false;
      for (Instruction &I : make_early_inc_range(*H))
        if (Drop.count(&I) && I.use_empty()) {
          I.eraseFromParent();
          AnyLeft = true;
        }
    }
    // A dropped window's accumulator phi has no users left once its window
    // is gone; erase it so the header keeps only the rolled accumulator.
    for (Instruction &I : make_early_inc_range(*H)) {
      auto *Phi = dyn_cast<PHINode>(&I);
      if (Phi && Phi->use_empty())
        Phi->eraseFromParent();
    }

    // The rewritten IV (phi +/- NewStep) is a simple recurrence by
    // construction; no SCEV query needed.
    Notes.push_back(H->getName());
    return (unsigned)D;
  }
  return 0;
}

// Fold a single-block self-latch loop whose induction is a `ptr` PHI stepped
// by a constant-stride byte GEP (the lifted LSR form): D identical memory
// windows at constant element offsets from the pointer reduce to one window
// and the stride divides by D. An integer mirror PHI stepping by the same
// byte count is divided too, so the end-address exit keeps its meaning.
//
// Shape (c-gcc-o2 `fill_vec`, c-clang-o2 byte loops):
//   %ptr = phi ptr [%base, Pre], [%step, H], [%peeled, Peel]
//   %step = getelementptr i8, %ptr, K          ; K = D * sizeof(elem)
//   <mem> T %v, %ptr                           ; window 0
//   <mem> T %v, getelementptr T, %ptr, 1       ; window 1
//   ...
//   %mirror = phi i64 [%base.bits, Pre], [%mstep, H], ...
//   %mstep = add i64 %mirror, K
//   %done = icmp eq i64 %mstep, %end
//   br i1 %done, label %Exit, label %H
unsigned tryFoldPtrLoop(BasicBlock *H, SmallVectorImpl<StringRef> &Notes) {
  bool SelfLatch = false;
  for (BasicBlock *S : successors(H))
    if (S == H)
      SelfLatch = true;
  if (!SelfLatch)
    return 0;

  // The pointer induction: exactly one ptr PHI, one header edge, and the
  // back value is a constant byte GEP on it.
  PHINode *Ptr = nullptr;
  for (Instruction &I : *H) {
    auto *P = dyn_cast<PHINode>(&I);
    if (!P)
      continue;
    if (!P->getType()->isPointerTy())
      continue;
    if (Ptr)
      return 0; // two pointer inductions: leave it to another pass
    Ptr = P;
  }
  if (!Ptr) {
    return 0;
  }
  Value *Back = nullptr;
  unsigned NBack = 0;
  for (unsigned i = 0, e = Ptr->getNumIncomingValues(); i != e; ++i)
    if (Ptr->getIncomingBlock(i) == H) {
      Back = Ptr->getIncomingValue(i);
      ++NBack;
    }
  if (NBack != 1 || !Back) {
    return 0;
  }
  auto *StepGEP = dyn_cast<GetElementPtrInst>(Back);
  if (!StepGEP || StepGEP->getParent() != H) {
    return 0;
  }
  if (StepGEP->getPointerOperand() != Ptr || StepGEP->getNumIndices() != 1) {
    return 0;
  }
  auto *KConst = dyn_cast<ConstantInt>(StepGEP->getOperand(1));
  if (!KConst) {
    return 0;
  }
  int64_t K = KConst->getSExtValue();
  if (K < 2) {
    return 0;
  }

  // Collect the memory windows: loads/stores on the pointer, directly or
  // through a single constant-index GEP whose element type decides the
  // element size. Anything else touching the pointer bails.
  Type *ElemTy = nullptr;
  int64_t ElemSize = 0;
  struct Win {
    Instruction *Mem;  // load or store
    int64_t Off;       // element offset from the pointer
  };
  SmallVector<Win, 8> Wins;
  for (User *U : Ptr->users()) {
    auto *I = dyn_cast<Instruction>(U);
    if (!I) {
      return 0;
    }
    if (I == StepGEP)
      continue;
    GetElementPtrInst *IdxGEP = nullptr;
    if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
      // direct touch
    } else if (auto *G = dyn_cast<GetElementPtrInst>(I)) {
      if (G->getParent() != H || G->getNumIndices() != 1 ||
          G->getPointerOperand() != Ptr)
        return 0;
      auto *C = dyn_cast<ConstantInt>(G->getOperand(1));
      if (!C)
        return 0;
      IdxGEP = G;
      // the GEP's users must be the memory ops of this window
      for (User *GU : G->users()) {
        auto *GM = dyn_cast<Instruction>(GU);
        if (!GM || !(isa<LoadInst>(GM) || isa<StoreInst>(GM)))
          return 0;
        if (GM->getParent() != H)
          return 0;
        Type *T = isa<LoadInst>(GM) ? cast<LoadInst>(GM)->getType()
                                    : cast<StoreInst>(GM)->getValueOperand()->getType();
        if (!T->isSized())
          return 0;
        int64_t Sz = (int64_t)T->getPrimitiveSizeInBits() / 8;
        if (!ElemTy) {
          ElemTy = T;
          ElemSize = Sz;
        } else if (T != ElemTy) {
          return 0;
        }
        Wins.push_back({GM, C->getSExtValue()});
      }
      continue;
    } else {
      return 0; // some other use of the pointer induction
    }
    if (I->getParent() != H)
      return 0;
    Type *T = isa<LoadInst>(I) ? cast<LoadInst>(I)->getType()
                                : cast<StoreInst>(I)->getValueOperand()->getType();
    if (!T->isSized())
      return 0;
    int64_t Sz = (int64_t)T->getPrimitiveSizeInBits() / 8;
    if (!ElemTy) {
      ElemTy = T;
      ElemSize = Sz;
    } else if (T != ElemTy) {
      return 0;
    }
    Wins.push_back({I, 0});
  }
  if (Wins.empty() || !ElemTy || ElemSize <= 0) {
    return 0;
  }
  if (K % ElemSize != 0) {
    return 0;
  }
  int64_t D = K / ElemSize;
  if (D < 2 || D > 64) {
    return 0;
  }
  // The window offsets must be exactly 0..D-1, one memory op each, and the
  // memory ops must be stores: a load's result feeds the loop body (the
  // reduction and copy chains), which only the slot-based fold may rewrite,
  // so pointer-induction load windows stay for tryFoldLoop.
  llvm::sort(Wins, [](const Win &A, const Win &B) { return A.Off < B.Off; });
  for (auto &W : Wins)
    if (isa<LoadInst>(W.Mem))
      return 0;
  for (int64_t J = 0; J < D; ++J) {
    if ((int64_t)Wins.size() <= J || Wins[J].Off != J)
      return 0;
  }
  if ((int64_t)Wins.size() != D) {
    return 0;
  }
  // Pair the windows: same opcode, same types, same non-address operands.
  auto *W0 = Wins[0].Mem;
  for (int64_t J = 1; J < D; ++J) {
    auto *WJ = Wins[J].Mem;
    if (WJ->getOpcode() != W0->getOpcode() ||
        WJ->getNumOperands() != W0->getNumOperands()) {
      return 0;
    }
    for (unsigned O = 0; O < W0->getNumOperands(); ++O) {
      Value *A = W0->getOperand(O), *C = WJ->getOperand(O);
      if (A == C)
        continue;
      // The pointer operand differs by design; everything else must match.
      bool APtr = A == Ptr || (isa<GetElementPtrInst>(A) &&
                               cast<GetElementPtrInst>(A)->getPointerOperand() == Ptr);
      bool CPtr = C == Ptr || (isa<GetElementPtrInst>(C) &&
                               cast<GetElementPtrInst>(C)->getPointerOperand() == Ptr);
      if (APtr && CPtr)
        continue;
      return 0;
    }
  }

  // Fold: point every window's memory op at window 0's address, erase the
  // dropped windows (address GEP first, memory op after), and divide the
  // pointer stride (and the mirror stride, when present) by D.
  Value *W0Addr = W0->getOperand(isa<LoadInst>(W0) ? 0 : 1);
  for (int64_t J = 1; J < D; ++J) {
    auto *WJ = Wins[J].Mem;
    Value *Addr = WJ->getOperand(isa<LoadInst>(WJ) ? 0 : 1);
    if (isa<LoadInst>(WJ))
      cast<LoadInst>(WJ)->setOperand(0, W0Addr);
    else
      cast<StoreInst>(WJ)->setOperand(1, W0Addr);
    // Drop the offset GEP now that its memory op moved.
    if (auto *G = dyn_cast<GetElementPtrInst>(Addr))
      if (G != StepGEP && G->use_empty())
        G->eraseFromParent();
    WJ->eraseFromParent();
  }
  StepGEP->setOperand(1, ConstantInt::get(KConst->getType(), K / D));

  // The integer mirror, when the exit tests it: same division.
  for (Instruction &I : *H) {
    auto *P = dyn_cast<PHINode>(&I);
    if (!P || P == Ptr || !P->getType()->isIntegerTy())
      continue;
    Value *MBack = nullptr;
    for (unsigned i = 0, e = P->getNumIncomingValues(); i != e; ++i)
      if (P->getIncomingBlock(i) == H)
        MBack = P->getIncomingValue(i);
    auto *Add = dyn_cast<BinaryOperator>(MBack);
    if (!Add || Add->getOpcode() != Instruction::Add || Add->getParent() != H)
      continue;
    auto *MC = dyn_cast<ConstantInt>(
        Add->getOperand(0) == P ? Add->getOperand(1)
                                : Add->getOperand(1) == P ? Add->getOperand(0)
                                                          : nullptr);
    if (!MC || MC->getSExtValue() != K)
      continue;
    Add->setOperand(Add->getOperand(0) == P ? 1 : 0,
                    ConstantInt::get(MC->getType(), K / D));
  }

  Notes.push_back(H->getName());
  return (unsigned)D;
}

// Straight-line synthesis: find the longest run of K >= 4 identical-shape
// windows of size M and build a counted loop around window 0. Returns the
// trip count K on success.
unsigned trySynthesize(Function &F, BasicBlock &B,
                       SmallVectorImpl<StringRef> &Notes) {
  SmallVector<Instruction *, 32> Insns;
  for (Instruction &I : B) {
    if (isa<PHINode>(&I))
      return 0; // PHIs cannot move into the synthesized body
    Insns.push_back(&I);
  }
  size_t N = Insns.size();
  if (N < 12)
    return 0;

  // Interned shape keys and a flat operand table, so the candidate scan
  // below compares integers instead of walking Use lists and APInts.
  SmallVector<ShapeKey, 32> Keys;
  DenseMap<const Instruction *, uint32_t> Index;
  for (size_t i = 0; i < N; ++i) {
    Keys.push_back(shapeOf(*Insns[i]));
    Index[Insns[i]] = (uint32_t)i;
  }
  constexpr uint32_t NoDef = UINT32_MAX;
  struct Operand {
    Value *V;
    int64_t Imm;  // the value when Int && Fits
    uint32_t Def; // index in Insns of an in-block def, else NoDef
    bool Int;     // ConstantInt
    bool Fits;    // ConstantInt representable as signed i64
    bool Leaf;    // accumulator leaf: non-constant, not defined in B
  };
  std::vector<Operand> Ops;
  std::vector<uint32_t> OpBegin(N + 1, 0);
  for (size_t i = 0; i < N; ++i) {
    OpBegin[i] = (uint32_t)Ops.size();
    for (Value *V : Insns[i]->operands()) {
      Operand Op{V, 0, NoDef, false, false, false};
      if (auto *CI = dyn_cast<ConstantInt>(V)) {
        Op.Int = true;
        if (auto S = signedConstant(CI)) {
          Op.Fits = true;
          Op.Imm = *S;
        }
      } else if (auto *D = dyn_cast<Instruction>(V);
                 D && D->getParent() == &B) {
        Op.Def = Index.lookup(D);
      } else {
        Op.Leaf = !isa<Constant>(V);
      }
      Ops.push_back(Op);
    }
  }
  OpBegin[N] = (uint32_t)Ops.size();

  auto SameKey = [&](size_t A, size_t B) {
    return Keys[A] == Keys[B];
  };

  // Candidates are tried for every window size and start, so a block of N
  // instructions sees O(N^2) of them; each one only reads the tables above
  // and allocates nothing until it matches.
  std::vector<size_t> Same(N + 1, 0);
  for (size_t M = 1; M <= N / MinWindowsForSynth; ++M) {
    // same[i]: run length of key[j] == key[j+M] from i.
    Same[N - M] = 0;
    for (size_t i = N - M; i-- > 0;) {
      Same[i] = SameKey(i, i + M) ? Same[i + 1] + 1 : 0;
      if (i == 0)
        break;
    }
    for (size_t Start = 0; Start + MinWindowsForSynth * M <= N; ++Start) {
      if (Same[Start] < 3 * M) {
        continue;
      }
      // Scan forward for matching windows with a uniform delta. Window w
      // is Insns[Start + w*M, Start + (w+1)*M).
      DenseMap<std::pair<unsigned, unsigned>, std::pair<Value *, Type *>>
          Threads;
      std::optional<int64_t> Delta;
      size_t K = 1;
      bool ScanOK = true;
      while (Start + (K + 1) * M <= N && ScanOK) {
        const size_t JBase = Start + K * M, PrevBase = Start + (K - 1) * M;
        std::optional<int64_t> WinDelta;
        bool SawOff = false;
        for (size_t p = 0; p < M && ScanOK; ++p) {
          const size_t IA = Start + p, IC = JBase + p;
          if (!(Keys[IA] == Keys[IC])) {
            ScanOK = false;
            break;
          }
          const unsigned NumOps = OpBegin[IA + 1] - OpBegin[IA];
          if (NumOps != OpBegin[IC + 1] - OpBegin[IC]) {
            ScanOK = false;
            break;
          }
          const Operand *OA = Ops.data() + OpBegin[IA];
          const Operand *OC = Ops.data() + OpBegin[IC];
          for (unsigned o = 0; o < NumOps; ++o) {
            const Operand &A = OA[o], &C = OC[o];
            if (A.V == C.V)
              continue;
            if (A.Int && C.Int) {
              int64_t D;
              if (!A.Fits || !C.Fits || SubOverflow(C.Imm, A.Imm, D)) {
                ScanOK = false;
                break;
              }
              if (D != 0) {
                SawOff = true;
                if (D % (int64_t)K != 0) {
                  ScanOK = false;
                  break;
                }
                int64_t Per = D / (int64_t)K;
                if (!WinDelta)
                  WinDelta = Per;
                else if (*WinDelta != Per) {
                  ScanOK = false;
                  break;
                }

              }
              continue;
            }
            // Acc thread: leaf from outside vs previous window dst.
            if (A.Leaf && C.Def == PrevBase + p) {
              Threads[{p, o}] = {A.V, A.V->getType()};
              continue;
            }
            // Same-position def pair: a window-0 def read where window K
            // reads its own def in the same slot.
            if (A.Def != NoDef && A.Def >= Start && A.Def < Start + M &&
                C.Def == JBase + (A.Def - Start))
              continue;
            // Equal-valued pair only (handled above); a differing var pair
            // would reread window 0 every iteration (reference rejects it).
            ScanOK = false;
            break;
          }
        }
        if (!ScanOK)
          break;
        if (!SawOff) {
          ScanOK = false;
          break;
        }
        if (!WinDelta)
          break;
        if (!Delta)
          Delta = WinDelta;
        else if (*Delta != *WinDelta)
          break;
        ++K;
      }
      // Short runs below the worth-it threshold synthesize only with
      // lift-recorded unroll evidence for the function: the construction
      // is correct for any K >= 2 (uniform delta, acc threads, latch),
      // but K < 4 alone is too thin to act on. The recorded factor's
      // exact value does not gate K (asm windows and IR windows count
      // different things); its presence confirms the function holds
      // unrolled copies worth re-rolling. Absent metadata keeps the skip.
      if (K < MinWindowsForSynth) {
        if (K < 2 || !revenant::evidence::unrollFactor(F))
          continue;
      }
      if (Threads.empty())
        continue; // independent windows: vector work, not a loop
      if (!Delta)
        continue;
      // An earlier window's value read outside the run blocks synthesis
      // (no single loop value exists for it).
      size_t RunEnd = Start + K * M;
      SmallPtrSet<Value *, 16> RunSet;
      for (size_t i = Start; i < RunEnd; ++i)
        if (!Insns[i]->getType()->isVoidTy())
          RunSet.insert(Insns[i]);
      bool Blocked = false;
      for (size_t i = Start; i < Start + (K - 1) * M && !Blocked; ++i) {
        Value *D = Insns[i];
        if (D->getType()->isVoidTy())
          continue;
        for (User *U : D->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || UI->getParent() != &B || !RunSet.count(UI)) {
            // A user outside the run (or outside the block).
            if (!UI || UI->getParent() != &B || !RunSet.count(UI)) {
              Blocked = true;
              break;
            }
          }
        }
      }
      if (Blocked)
        continue;

      // Live-out last-window dsts become exit PHIs.
      SmallVector<std::pair<Value *, Value *>, 4> LiveOut; // (last, w0eq)
      for (size_t p = 0; p < M; ++p) {
        Value *D = Insns[Start + (K - 1) * M + p];
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
        if (Outside)
          LiveOut.push_back({D, Insns[Start + p]});
      }

      // Build: preheader (instrs[..Start]) -> loop -> exit.
      LLVMContext &Ctx = F.getContext();
      BasicBlock *Pre = &B;
      // Carve the run out: Exit takes [RunEnd, end) (or just the
      // terminator when the run reaches the block end), then Loop takes
      // [Start, RunEnd), leaving Pre with [..Start).
      BasicBlock *Exit = (RunEnd < N)
                              ? Pre->splitBasicBlock(Insns[RunEnd], "unroll.exit")
                              : Pre->splitBasicBlock(Pre->getTerminator(),
                                                     "unroll.exit");
      // Now split the run head: Pre keeps [..Start), Loop gets [Start, end).
      // (With Start == 0, Pre is left empty with just the branch.)
      BasicBlock *Loop = Pre->splitBasicBlock(Insns[Start], "unroll");
      // Pre = [..Start), Loop = [Start, RunEnd), ExitTail holds the rest.
      // The middle block's terminator is the unconditional split branch;
      // replace it below with the latch. First drop windows 1.. from Loop.
      SmallPtrSet<Instruction *, 16> DropSet;
      // Loop currently holds [Start, RunEnd); erase windows 1...
      {
        SmallVector<Instruction *, 16> InLoop;
        for (Instruction &I : *Loop)
          if (!I.isTerminator())
            InLoop.push_back(&I);
        for (size_t i = M; i < InLoop.size(); ++i)
          DropSet.insert(InLoop[i]);
      }

      // Snapshot the kept window-0 body now: creating the counter/acc
      // phis below inserts into Loop, and they must not join the rewire
      // walk (they are not window-0 members).
      SmallVector<Instruction *, 8> BodyInsns;
      for (size_t p = 0; p < M; ++p)
        BodyInsns.push_back(Insns[Start + p]);
      IRBuilder<> LB(Loop->getContext());
      // PHIs must open the block: insert at its head (Loop holds only
      // window-0 body code here, no phis yet).
      LB.SetInsertPoint(Loop, Loop->begin());
      Type *I64 = Type::getInt64Ty(Ctx);
      PHINode *IPhi = LB.CreatePHI(I64, 2, "u0i");
      IPhi->addIncoming(ConstantInt::get(I64, 0), Pre);
      DenseMap<std::pair<unsigned, unsigned>, PHINode *> AccPhi;
      for (auto &T : Threads) {
        Value *Leaf = T.second.first;
        Type *Ty = T.second.second;
        PHINode *P = LB.CreatePHI(Ty, 2, "u0a");
        // Backedge from the produced body dst (window-0 def kept).
        Value *Prod = Insns[Start + T.first.first];
        P->addIncoming(Leaf, Pre);
        P->addIncoming(Prod, Loop);
        AccPhi[T.first] = P;
        copyProvenance(Insns[Start + T.first.first], P);
      }
      if (MDNode *N = Pre->getTerminator()->getMetadata(VA_MD))
        IPhi->setMetadata(VA_MD, N);

      // Rewire the kept body: constants shifted by Delta become
      // i*Delta + c0 (truncated/extended to the operand width), acc
      // leaves become their acc phis.
      IRBuilder<> BB(&Loop->front());
      for (size_t p = 0; p < BodyInsns.size(); ++p) {
        Instruction *NI = BodyInsns[p];
        Instruction *W0I = Insns[Start + p];
        Instruction *W1I = Insns[Start + M + p];
        for (unsigned o = 0; o < NI->getNumOperands(); ++o) {
          Value *VA = W0I->getOperand(o), *VC = W1I->getOperand(o);
          auto *IA = dyn_cast<ConstantInt>(VA);
          auto *IC = dyn_cast<ConstantInt>(VC);
          if (IA && IC && constantDifference(IA, IC) == Delta) {
            // Emit i*Delta + c0 ahead of NI.
            IRBuilder<> EB(NI);
            Value *Scaled = EB.CreateMul(IPhi, ConstantInt::get(I64, *Delta),
                                         "u0o");
            Value *Wide = Scaled;
            if (IA->getSExtValue() != 0)
              Wide = EB.CreateAdd(Scaled,
                                  ConstantInt::get(I64, IA->getSExtValue()),
                                  "u0o");
            Type *OpTy = NI->getOperand(o)->getType();
            Value *Final = Wide;
            if (OpTy->isIntegerTy() && OpTy->getIntegerBitWidth() != 64) {
              unsigned W = OpTy->getIntegerBitWidth();
              Final = W < 64 ? EB.CreateTrunc(Wide, OpTy, "u0o")
                             : EB.CreateSExt(Wide, OpTy, "u0o");
            }
            if (auto *MDBC = dyn_cast<Instruction>(Scaled))
              copyProvenance(NI, MDBC);
            if (auto *MDBC2 = dyn_cast<Instruction>(Final))
              if (Final != Scaled)
                copyProvenance(NI, MDBC2);
            NI->setOperand(o, Final);
          } else {
            // Acc rewiring: operand equals the thread leaf.
            for (auto &T : Threads) {
              if (T.first.first == p && T.first.second == o &&
                  VA == T.second.first) {
                NI->setOperand(o, AccPhi[T.first]);
                break;
              }
            }
          }
        }
      }
      // Latch: i_next = i + 1; c = i_next ult K; cbr.
      Instruction *LatchBr = Loop->getTerminator();
      {
        IRBuilder<> EB(LatchBr);
        Value *Next = EB.CreateAdd(IPhi, ConstantInt::get(I64, 1), "u0inext");
        Value *Cond = EB.CreateICmpULT(Next, ConstantInt::get(I64, (int64_t)K),
                                       "u0c");
        copyProvenance(LatchBr, cast<Instruction>(Next));
        copyProvenance(LatchBr, cast<Instruction>(Cond));
        // Register the counter backedge (the phi had only the preheader
        // entry so far).
        IPhi->addIncoming(Next, Loop);
        EB.CreateCondBr(Cond, Loop, Exit);
        LatchBr->eraseFromParent();
      }
      // Exit PHIs for live-outs first (they reference kept window-0
      // values), then redirect outside users of dropped last-window
      // values to the exit phis. Only then erase the dropped windows.
      BasicBlock *ExitBlk = Exit;
      IRBuilder<> XB(&ExitBlk->front());
      DenseMap<Value *, Value *> Rename;
      for (auto &LO : LiveOut) {
        PHINode *P = XB.CreatePHI(LO.second->getType(), 1, "u0x");
        P->addIncoming(LO.second, Loop);
        if (auto *D = dyn_cast<Instruction>(LO.second))
          copyProvenance(D, P);
        Rename[LO.first] = P;
      }
      for (auto &KV : Rename)
        KV.first->replaceAllUsesWith(KV.second);
      // Erase dropped window instructions in reverse program order so
      // users vanish before their operands. (Outside users were
      // redirected above; remaining uses are inside dropped code.)
      SmallVector<Instruction *, 16> DropDead;
      for (auto It = Loop->rbegin(); It != Loop->rend(); ++It)
        if (DropSet.count(&*It))
          DropDead.push_back(&*It);
      for (Instruction *I : DropDead)
        I->eraseFromParent();
      // Pre branches to Loop (splitBasicBlock already did); fix PHI
      // incoming blocks: IPhi/acc phis list Pre and Loop.
      for (PHINode &P : Loop->phis()) {
        for (unsigned i = 0; i < P.getNumIncomingValues(); ++i) {
          if (P.getIncomingBlock(i) != Pre && P.getIncomingBlock(i) != Loop) {
            P.setIncomingBlock(i, P.getIncomingBlock(i) == Exit ? Loop
                                                                : Pre);
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
      // The new counter (phi 0 / +1) is an add recurrence by construction.
      Notes.push_back(Pre->getName());
      return (unsigned)K;
    }
  }
  return 0;
}
} // namespace

PreservedAnalyses RerollPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  SmallVector<StringRef, 8> Notes;
  bool Changed = false;

  // In-loop folding first (up to 4 rounds: folding can expose a further
  // fold, e.g. nested unroll factors), then straight-line synthesis.
  // Backedges are found directly (no LoopInfo: see tryFoldLoop).
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
    // Pointer-induction unrolls (lifted LSR `ptr` PHI stepped by a byte GEP)
    // are folded separately: the slot machinery is integer-index based.
    for (BasicBlock *H : Headers)
      if (tryFoldPtrLoop(H, Notes))
        RoundChanged = true;
    Changed |= RoundChanged;
    if (!RoundChanged)
      break;
  }
  // Multi-accumulator fold: try before single-accumulator rounds so we
  // don't miss headers that tryFoldLoop would consume.
  if (unrollAccumFold(F, Notes))
    Changed = true;
  // Full/partial straight-line re-rolling (RerollFull.cpp) runs before the
  // constant-shift synth: affine-value windows subsume no existing shape,
  // and trying them first keeps the cheaper structural synth as fallback.
  SmallVector<BasicBlock *, 16> Blocks;
  for (BasicBlock &B : F)
    Blocks.push_back(&B);
  for (BasicBlock *B : Blocks)
    if (tryRerollFull(F, *B, Notes))
      Changed = true;
  SmallVector<BasicBlock *, 16> SynthBlocks;
  for (BasicBlock &B : F)
    SynthBlocks.push_back(&B);
  for (BasicBlock *B : SynthBlocks)
    if (trySynthesize(F, *B, Notes))
      Changed = true;

  if (!Changed)
    return PreservedAnalyses::all();
  // CFG changed in both shapes; keep it simple and invalidate.
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
