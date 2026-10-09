//===-- Idioms.cpp - compiler-idiom reversal pass -------------------*- C++ -*-===//
// Port of `crates/idioms` (magic-div, min/max/abs, rotates, bit tricks,
// byte-swap, strength-reduced multiplies, compiler intrinsics) as one
// Function pass named `revenant-idioms`.
//
// Every rewrite preserves `!revenant.va` provenance: the new instruction
// inherits the metadata of the matched root so the VA map stays aligned.
// `!revenant.opaque` instructions are never touched.
//
// Strategy notes per idiom family:
//
// - Magic division/modulo (MSVC `mul`+fixup and clang `imul`+sign-correct
//   shapes): identify the widened dividend and evaluate the arithmetic DAG
//   as q(x), even when x is itself an instruction. Recover |d| from the
//   first positive nonzero result, then exhaustively verify i8/i16 or check
//   quotient boundaries and extrema for i32/i64 before rewriting.
// - abs/min/max/clamp: icmp+select (signed and unsigned, both operand
//   orders) become llvm.abs/llvm.{s,u}{min,max}; xor+sub negations fold to
//   sub(0,x). Signed subtraction-flag chains become llvm.scmp.
// - Saturating arithmetic: exact unsigned add/sub select forms become
//   llvm.uadd.sat/llvm.usub.sat.
// - Rotates: (a<<c)|(a>>(w-c)) folds to llvm.fshl.
// - Byte-swap: only a complete, reversed byte permutation becomes
//   llvm.bswap.
// - Kernighan popcount loops become llvm.ctpop after checking their loop
//   state and side-effect-free body. Existing ctpop calls pass through.
// - Bit tests already use the readable `and`/`icmp` IR form.
// - Strength-reduced multiplies: mul by a power of two folds to shl; other
//   constant multiplies stay as mul.
// - Memset/memcpy loop recovery belongs to revenant-patterns, not this pass.

#include "revenant/Transforms/Idioms/Idioms.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace llvm;
using namespace llvm::PatternMatch;

namespace {

// Copy !revenant.va / !revenant.opaque metadata from Old to New.
void copyProvenance(Instruction *New, Instruction *Old) {
  if (MDNode *VA = Old->getMetadata("revenant.va"))
    New->setMetadata("revenant.va", VA);
  if (MDNode *Op = Old->getMetadata("revenant.opaque"))
    New->setMetadata("revenant.opaque", Op);
}

bool hasOpaque(Instruction *I) { return I->hasMetadata("revenant.opaque"); }

// ---- magic division: concrete DAG evaluator ------------------------------
// Interprets an instruction DAG as q(x) with wrap-around semantics.
// Values are carried as 128-bit APInts holding a W-bit value.
// On success sets Dividend (the single non-constant leaf), WideMul (a
// W->2W widening multiply was seen) and WidthConflict (leaf seen at two
// widths). Returns false when the DAG leaves the supported op set.
struct DagEval {
  APInt XVal{32, 0}; // concrete input, stored at the root width
  Value *Dividend = nullptr;
  unsigned DividendW = 0;
  bool WidthConflict = false;
  bool WideMul = false;
  Value *Input = nullptr;
  unsigned Budget = 256;
  SmallPtrSet<Value *, 16> Visited;
  DenseMap<Value *, std::optional<std::pair<APInt, unsigned>>> Memo;

  bool evalOperand(Value *V, unsigned W, APInt &Out128);
  bool evalInst(Instruction *I, APInt &Out128, unsigned &OutW);
};

bool DagEval::evalOperand(Value *V, unsigned W, APInt &Out128) {
  if (V == Input && Input) {
    Dividend = Input;
    DividendW = W;
    Out128 = XVal.zextOrTrunc(W).zext(128);
    return true;
  }
  if (Input && !isa<ConstantInt>(V) && !isa<Instruction>(V))
    return false;
  if (auto *C = dyn_cast<ConstantInt>(V)) {
    Out128 = C->getValue().zextOrTrunc(W).zext(128);
    return true;
  }
  if (auto *I = dyn_cast<Instruction>(V)) {
    auto It = Memo.find(I);
    if (It != Memo.end()) {
      if (!It->second)
        return false;
      Out128 = It->second->first.zextOrTrunc(W).zext(128);
      return true;
    }
    Visited.insert(I);
    APInt R(128, 0);
    unsigned RW = 0;
    bool Ok = evalInst(I, R, RW);
    if (!Ok)
      Memo[I] = std::nullopt;
    else {
      Memo[I] = std::make_pair(R, RW);
      Out128 = R.trunc(RW).zextOrTrunc(W).zext(128);
    }
    return Ok;
  }
  // Leaf: the dividend candidate. Only one distinct leaf allowed.
  if (!Dividend) {
    Dividend = V;
    DividendW = W;
  } else {
    if (Dividend != V)
      return false;
    if (DividendW != W)
      WidthConflict = true;
  }
  Out128 = XVal.zextOrTrunc(W).zext(128);
  return true;
}

bool DagEval::evalInst(Instruction *I, APInt &Out128, unsigned &OutW) {
  if (Budget == 0)
    return false;
  --Budget;
  // Integer arithmetic only: a pointer, vector or float anywhere in the node
  // (ptrtoint, pointer select/icmp) ends the evaluation.
  if (!I->getType()->isIntegerTy() ||
      !llvm::all_of(I->operands(),
                    [](const Use &U) { return U->getType()->isIntegerTy(); }))
    return false;
  if (auto *BI = dyn_cast<BinaryOperator>(I)) {
    unsigned W = BI->getType()->getIntegerBitWidth();
    if (W < 8 || W > 64)
      return false;
    if (BI->getOpcode() == Instruction::Mul) {
      // umulh/smulh shape: mul (zext/sext W->2W x), C.
      for (Value *Op : BI->operands()) {
        if (auto *CI = dyn_cast<CastInst>(Op)) {
          if ((CI->getOpcode() == Instruction::ZExt ||
               CI->getOpcode() == Instruction::SExt) &&
              CI->getSrcTy()->getIntegerBitWidth() * 2 == W &&
              CI->getDestTy()->getIntegerBitWidth() == W)
            WideMul = true;
        }
      }
    }
    APInt A128(128, 0), B128(128, 0);
    if (!evalOperand(BI->getOperand(0), W, A128))
      return false;
    if (!evalOperand(BI->getOperand(1), W, B128))
      return false;
    APInt A = A128.trunc(W), B = B128.trunc(W);
    APInt R(W, 0);
    switch (BI->getOpcode()) {
    case Instruction::Add:
      R = A + B;
      break;
    case Instruction::Sub:
      R = A - B;
      break;
    case Instruction::Mul:
      R = A * B;
      break;
    case Instruction::UDiv:
      if (B.isZero())
        return false;
      R = A.udiv(B);
      break;
    case Instruction::SDiv: {
      if (B.isZero())
        return false;
      // INT_MIN / -1 is poison; bail.
      if (A.isMinSignedValue() && B.isAllOnes())
        return false;
      R = A.sdiv(B);
      break;
    }
    case Instruction::URem:
      if (B.isZero())
        return false;
      R = A.urem(B);
      break;
    case Instruction::SRem:
      if (B.isZero())
        return false;
      R = A.srem(B);
      break;
    case Instruction::Shl:
      if (B.uge(W))
        return false;
      R = A.shl(B.getZExtValue());
      break;
    case Instruction::LShr:
      if (B.uge(W))
        return false;
      R = A.lshr(B.getZExtValue());
      break;
    case Instruction::AShr:
      if (B.uge(W))
        return false;
      R = A.ashr(B.getZExtValue());
      break;
    case Instruction::And:
      R = A & B;
      break;
    case Instruction::Or:
      R = A | B;
      break;
    case Instruction::Xor:
      R = A ^ B;
      break;
    default:
      return false;
    }
    Out128 = R.zext(128);
    OutW = W;
    return true;
  }
  if (auto *CI = dyn_cast<CastInst>(I)) {
    unsigned FW = CI->getSrcTy()->getIntegerBitWidth();
    unsigned TW = CI->getDestTy()->getIntegerBitWidth();
    if (FW < 1 || FW > 64 || TW < 8 || TW > 64)
      return false;
    APInt V128(128, 0);
    if (!evalOperand(CI->getOperand(0), FW, V128))
      return false;
    APInt V = V128.trunc(FW);
    APInt R(TW, 0);
    // Wrap-around evaluation: nuw/nsw/exact on the cast do not change
    // the computed bits.
    switch (CI->getOpcode()) {
    case Instruction::Trunc:
      R = V.zextOrTrunc(TW);
      break;
    case Instruction::ZExt:
      R = V.zextOrTrunc(TW);
      break;
    case Instruction::SExt:
      R = V.sextOrTrunc(TW);
      break;
    default:
      return false;
    }
    Out128 = R.zext(128);
    OutW = TW;
    return true;
  }
  if (auto *SI = dyn_cast<SelectInst>(I)) {
    unsigned W = SI->getType()->getIntegerBitWidth();
    APInt C(128, 0);
    if (!evalOperand(SI->getCondition(), 1, C))
      return false;
    APInt V(128, 0);
    if (!evalOperand(C.isZero() ? SI->getFalseValue() : SI->getTrueValue(),
                     W, V))
      return false;
    Out128 = V;
    OutW = W;
    return true;
  }
  if (auto *IC = dyn_cast<ICmpInst>(I)) {
    unsigned W =
        IC->getOperand(0)->getType()->getScalarType()->getIntegerBitWidth();
    APInt A128(128, 0), B128(128, 0);
    if (!evalOperand(IC->getOperand(0), W, A128))
      return false;
    if (!evalOperand(IC->getOperand(1), W, B128))
      return false;
    APInt A = A128.trunc(W), B = B128.trunc(W);
    bool R = false;
    switch (IC->getPredicate()) {
    case ICmpInst::ICMP_EQ:
      R = A.eq(B);
      break;
    case ICmpInst::ICMP_NE:
      R = A.ne(B);
      break;
    case ICmpInst::ICMP_ULT:
      R = A.ult(B);
      break;
    case ICmpInst::ICMP_ULE:
      R = A.ule(B);
      break;
    case ICmpInst::ICMP_UGT:
      R = A.ugt(B);
      break;
    case ICmpInst::ICMP_UGE:
      R = A.uge(B);
      break;
    case ICmpInst::ICMP_SLT:
      R = A.slt(B);
      break;
    case ICmpInst::ICMP_SLE:
      R = A.sle(B);
      break;
    case ICmpInst::ICMP_SGT:
      R = A.sgt(B);
      break;
    case ICmpInst::ICMP_SGE:
      R = A.sge(B);
      break;
    default:
      return false;
    }
    Out128 = APInt(128, R ? 1 : 0);
    OutW = 1;
    return true;
  }
  return false;
}

struct Quotient {
  Instruction *Root = nullptr;
  Value *Dividend = nullptr;
  bool Signed = false;
  int64_t D = 0;
  unsigned W = 0;
  SmallVector<Value *, 8> Members;
};

// Deterministic witness inputs covering every quotient step boundary.
// All values are masked to W bits at construction (APInt asserts on
// out-of-range values without implicitTrunc; the Rust reference masks
// with um() at the same points).
std::vector<APInt> witnesses(unsigned W, int64_t D) {
  std::vector<APInt> Ws;
  uint64_t Mask = (W >= 64) ? ~0ULL : ((1ULL << W) - 1);
  auto push64 = [&](uint64_t V) { Ws.emplace_back(W, V & Mask); };
  auto push = [&](int64_t V) { push64((uint64_t)V); };
  for (int64_t V : {0, 1, 2, 3, 4, 5})
    push(V);
  int64_t AD = D < 0 ? -D : D;
  for (int64_t K : {1, 2, 3})
    for (int64_t J : {-1, 0, 1}) {
      push(K * AD + J);
      push(-K * AD + J);
    }
  if (W < 64) {
    uint64_t UMax = Mask;
    for (uint64_t V : {UMax, UMax - 1, UMax - 2})
      push64(V);
    uint64_t KM = UMax / (uint64_t)AD;
    for (uint64_t V :
         {KM * (uint64_t)AD - 1, KM * (uint64_t)AD, KM * (uint64_t)AD + 1})
      push64(V);
    int64_t SMax = (W == 64) ? INT64_MAX : (int64_t)((1ULL << (W - 1)) - 1);
    int64_t SMin = (W == 64) ? INT64_MIN : -(int64_t)(1ULL << (W - 1));
    push(SMin);
    push(SMin + 1);
    push(SMax);
    push(SMax - 1);
  }
  uint64_t S = 0x9E3779B97F4A7C15ULL;
  for (int I = 0; I < 24; ++I) {
    S = S * 6364136223846793005ULL + 1442695040888963407ULL;
    push64((S >> 11) & Mask);
  }
  llvm::sort(Ws, [](const APInt &A, const APInt &B) { return A.ult(B); });
  Ws.erase(std::unique(Ws.begin(), Ws.end()), Ws.end());
  return Ws;
}

std::optional<APInt> evalRoot(Instruction *Root, const APInt &X, unsigned W,
                              Value *&Dividend, bool &WideMul,
                              bool &WidthConflict,
                              SmallVectorImpl<Value *> *Members,
                              Value *Input = nullptr) {
  DagEval E;
  E.XVal = X.zextOrTrunc(W);
  E.Input = Input;
  APInt Out(128, 0);
  unsigned OutW = 0;
  if (!E.evalInst(Root, Out, OutW))
    return std::nullopt;
  Dividend = E.Dividend;
  WideMul = E.WideMul;
  WidthConflict = E.WidthConflict;
  if (Members)
    for (Value *V : E.Visited)
      if (V != Root)
        Members->push_back(V);
  return Out.trunc(OutW).zextOrTrunc(W).zext(128);
}

// Try to prove Root computes x/d. Mirrors MagicDiv::analyze.
std::optional<Quotient> analyzeQuotient(Instruction *Root) {
  auto *Ty = dyn_cast<IntegerType>(Root->getType());
  if (!Ty)
    return std::nullopt;
  unsigned W = Ty->getBitWidth();
  if (W < 8 || W > 64)
    return std::nullopt;
  if (!isa<BinaryOperator>(Root) && !isa<CastInst>(Root) &&
      !isa<SelectInst>(Root))
    return std::nullopt;
  if (hasOpaque(Root))
    return std::nullopt;

  // The numerator may itself be an instruction, such as (a - b) or (a | b).
  // Treat the source of a W->2W extension as the symbolic dividend instead
  // of requiring the full arithmetic DAG to have a single argument leaf.
  SmallPtrSet<Value *, 4> CandidateSet;
  SmallPtrSet<Value *, 32> Seen;
  SmallVector<Value *, 32> Work{Root};
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    auto *I = dyn_cast<Instruction>(V);
    if (!I || !Seen.insert(V).second)
      continue;
    if (auto *CI = dyn_cast<CastInst>(I)) {
      auto *SrcTy = dyn_cast<IntegerType>(CI->getSrcTy());
      auto *DstTy = dyn_cast<IntegerType>(CI->getDestTy());
      if (SrcTy && DstTy && SrcTy->getBitWidth() == W &&
          DstTy->getBitWidth() == 2 * W)
        CandidateSet.insert(CI->getOperand(0));
    }
    for (Value *Operand : I->operands())
      Work.push_back(Operand);
  }
  if (CandidateSet.size() > 1)
    return std::nullopt;
  Value *DivInput = CandidateSet.empty() ? nullptr : *CandidateSet.begin();

  APInt Zero(W, 0);
  Value *Div = nullptr;
  bool Wide = false, Conflict = false;
  auto V0 = evalRoot(Root, Zero, W, Div, Wide, Conflict, nullptr, DivInput);
  if (!V0 || !V0->trunc(W).isZero() || Conflict || !Wide || !Div)
    return std::nullopt;
  if (Div->getType() && Div->getType()->isIntegerTy() &&
      Div->getType()->getIntegerBitWidth() != W)
    return std::nullopt;

  // |d| = smallest positive x with q(x) != 0. Exponential probe then
  // binary search (quotient-like functions are 0 up to |d|).
  std::optional<uint64_t> Hi;
  for (unsigned K = 0; K < W; ++K) {
    APInt X(W, 0);
    X.setBit(K);
    Value *D2 = nullptr;
    bool W2 = false, C2 = false;
    auto V = evalRoot(Root, X, W, D2, W2, C2, nullptr, DivInput);
    if (!V)
      return std::nullopt;
    if (!V->trunc(W).isZero()) {
      Hi = (K >= 64) ? ~0ULL : (1ULL << K);
      break;
    }
  }
  if (!Hi)
    return std::nullopt;
  uint64_t Lo = *Hi >> 1, R = *Hi;
  while (R - Lo > 1) {
    uint64_t Mid = (Lo + R) / 2;
    Value *D2 = nullptr;
    bool W2 = false, C2 = false;
    auto V = evalRoot(Root, APInt(W, Mid), W, D2, W2, C2, nullptr, DivInput);
    if (!V)
      return std::nullopt;
    if (!V->trunc(W).isZero())
      R = Mid;
    else
      Lo = Mid;
  }
  if (R <= 1)
    return std::nullopt;
  int64_t D = (int64_t)R;

  // Small domains are cheap enough to check exhaustively. This makes the
  // accepted i8/i16 forms proofs rather than heuristic matches. Wider
  // integers use the quotient boundaries and signed/unsigned extrema above.
  std::vector<APInt> Inputs;
  if (W <= 16) {
    uint32_t Limit = 1u << W;
    Inputs.reserve(Limit);
    for (uint32_t X = 0; X < Limit; ++X)
      Inputs.emplace_back(W, X);
  } else {
    Inputs = witnesses(W, D);
  }

  // Candidates: udiv d, sdiv +d, sdiv -d. First fully verified wins.
  for (int Cand = 0; Cand < 3; ++Cand) {
    bool Signed = Cand != 0;
    int64_t DD = (Cand == 2) ? -D : D;
    bool Ok = true;
    for (const APInt &WX : Inputs) {
      APInt Expected(W, 0);
      if (Signed) {
        int64_t SVal = WX.getSExtValue();
        if (SVal == INT64_MIN && DD == -1 && W == 64) {
          Ok = false;
          break;
        }
        int64_t QV = SVal / DD;
        Expected = APInt(W, (uint64_t)QV, true);
      } else {
        Expected = WX.udiv(APInt(W, (uint64_t)D));
      }
      Value *D2 = nullptr;
      bool W2 = false, C2 = false;
      auto Got = evalRoot(Root, WX, W, D2, W2, C2, nullptr, DivInput);
      if (!Got || Got->trunc(W) != Expected) {
        Ok = false;
        break;
      }
    }
    if (Ok) {
      Quotient Q;
      Q.Root = Root;
      Q.Dividend = Div;
      Q.Signed = Signed;
      Q.D = DD;
      Q.W = W;
      Value *D3 = nullptr;
      bool W3 = false, C3 = false;
      evalRoot(Root, APInt(W, (uint64_t)D), W, D3, W3, C3, &Q.Members,
               DivInput);
      return Q;
    }
  }
  return std::nullopt;
}

// `x - q*d` / `x + q*-d` (either order for add) -> rem. q must already be
// a udiv/sdiv by a constant (possibly just rewritten this pass).
std::optional<Quotient> analyzeRem(BinaryOperator *Root) {
  if (Root->getOpcode() != Instruction::Sub &&
      Root->getOpcode() != Instruction::Add)
    return std::nullopt;
  auto *Ty = dyn_cast<IntegerType>(Root->getType());
  if (!Ty)
    return std::nullopt;
  unsigned W = Ty->getBitWidth();
  auto isMulByConst = [](Value *V, Value *&Q, APInt &C) -> bool {
    auto *M = dyn_cast<BinaryOperator>(V);
    if (!M || M->getOpcode() != Instruction::Mul)
      return false;
    if (auto *CI = dyn_cast<ConstantInt>(M->getOperand(1))) {
      Q = M->getOperand(0);
      C = CI->getValue();
      return true;
    }
    if (auto *CI = dyn_cast<ConstantInt>(M->getOperand(0))) {
      Q = M->getOperand(1);
      C = CI->getValue();
      return true;
    }
    return false;
  };
  Value *X = nullptr, *M = nullptr;
  Value *Qv = nullptr;
  APInt C(64, 0);
  if (Root->getOpcode() == Instruction::Sub) {
    if (!isMulByConst(Root->getOperand(1), Qv, C))
      return std::nullopt;
    X = Root->getOperand(0);
    M = Root->getOperand(1);
  } else {
    if (isMulByConst(Root->getOperand(1), Qv, C)) {
      X = Root->getOperand(0);
      M = Root->getOperand(1);
    } else if (isMulByConst(Root->getOperand(0), Qv, C)) {
      X = Root->getOperand(1);
      M = Root->getOperand(0);
    } else
      return std::nullopt;
  }
  auto *Div = dyn_cast<BinaryOperator>(Qv);
  if (!Div || (Div->getOpcode() != Instruction::UDiv &&
               Div->getOpcode() != Instruction::SDiv))
    return std::nullopt;
  auto *DC = dyn_cast<ConstantInt>(Div->getOperand(1));
  if (!DC || Div->getOperand(0) != X)
    return std::nullopt;
  bool Signed = Div->getOpcode() == Instruction::SDiv;
  int64_t D = DC->getValue().getSExtValue();
  APInt Want(W, Root->getOpcode() == Instruction::Sub ? (uint64_t)D
                                                     : (uint64_t)-D,
             true);
  if (C.sextOrTrunc(W) != Want)
    return std::nullopt;
  Quotient Q;
  Q.Root = Root;
  Q.Dividend = X;
  Q.Signed = Signed;
  Q.D = D;
  Q.W = W;
  Q.Members.push_back(Qv);
  Q.Members.push_back(M);
  return Q;
}

// (c ? -x : x) / (c ? x : -x) with a sign-test condition -> x.
bool matchAbsFromSelect(SelectInst *SI, Value *&X) {
  auto *IC = dyn_cast<ICmpInst>(SI->getCondition());
  if (!IC)
    return false;
  ICmpInst::Predicate P = IC->getPredicate();
  Value *A = IC->getOperand(0), *B = IC->getOperand(1);
  Value *T = SI->getTrueValue(), *F = SI->getFalseValue();
  auto isNeg = [](Value *E, Value *V) {
    return match(E, m_Sub(m_Zero(), m_Specific(V)));
  };
  auto isZero = [](Value *V) {
    if (auto *C = dyn_cast<ConstantInt>(V))
      return C->isZero();
    return false;
  };
  auto isNegOne = [](Value *V) {
    if (auto *C = dyn_cast<ConstantInt>(V))
      return C->isMinusOne();
    return false;
  };
  // x < 0 ? -x : x  and  x <= -1 ? -x : x.
  if (((P == ICmpInst::ICMP_SLT && isZero(B)) ||
       (P == ICmpInst::ICMP_SLE && isNegOne(B))) &&
      isNeg(T, A) && F == A) {
    X = A;
    return true;
  }
  // x >= 0 ? x : -x  and  x > -1 ? x : -x.
  if (((P == ICmpInst::ICMP_SGE && isZero(B)) ||
       (P == ICmpInst::ICMP_SGT && isNegOne(B))) &&
      T == A && isNeg(F, A)) {
    X = A;
    return true;
  }
  return false;
}

// Recover the compare-with-flags idiom emitted for a signed three-way
// comparison: sign(x-y) plus an overflow-corrected nonzero bit.
bool matchSignedCompare(Value *V, Value *&LHS, Value *&RHS) {
  auto *Add = dyn_cast<BinaryOperator>(V);
  if (!Add || Add->getOpcode() != Instruction::Add)
    return false;
  for (unsigned I = 0; I != 2; ++I) {
    auto *Sign = dyn_cast<SExtInst>(Add->getOperand(I));
    auto *Nonzero = dyn_cast<ZExtInst>(Add->getOperand(1 - I));
    auto *Less = Sign ? dyn_cast<ICmpInst>(Sign->getOperand(0)) : nullptr;
    auto *And = Nonzero ? dyn_cast<BinaryOperator>(Nonzero->getOperand(0))
                        : nullptr;
    if (!Less || !And || And->getOpcode() != Instruction::And ||
        Less->getPredicate() != ICmpInst::ICMP_SLT)
      continue;
    auto *GreaterOrEqual =
        dyn_cast<ICmpInst>(And->getOperand(0));
    auto *Unequal = dyn_cast<ICmpInst>(And->getOperand(1));
    if (!GreaterOrEqual || !Unequal) {
      GreaterOrEqual = dyn_cast<ICmpInst>(And->getOperand(1));
      Unequal = dyn_cast<ICmpInst>(And->getOperand(0));
    }
    if (!GreaterOrEqual || !Unequal ||
        GreaterOrEqual->getPredicate() != ICmpInst::ICMP_SGT ||
        Unequal->getPredicate() != ICmpInst::ICMP_NE)
      continue;
    auto *NegOne = dyn_cast<ConstantInt>(GreaterOrEqual->getOperand(1));
    if (!NegOne || !NegOne->isMinusOne())
      continue;
    Value *X = Less->getOperand(0);
    Value *Y = Less->getOperand(1);
    if (Unequal->getOperand(0) != X || Unequal->getOperand(1) != Y)
      continue;
    auto *Sub = dyn_cast<BinaryOperator>(GreaterOrEqual->getOperand(0));
    if (!Sub || Sub->getOpcode() != Instruction::Xor)
      continue;
    auto matchXor = [](Value *V, Value *A, Value *B) {
      auto *Xor = dyn_cast<BinaryOperator>(V);
      return Xor && Xor->getOpcode() == Instruction::Xor &&
             ((Xor->getOperand(0) == A && Xor->getOperand(1) == B) ||
              (Xor->getOperand(0) == B && Xor->getOperand(1) == A));
    };
    Value *Difference = nullptr;
    Value *Overflow = nullptr;
    for (unsigned K = 0; K != 2; ++K) {
      auto *Candidate = dyn_cast<BinaryOperator>(Sub->getOperand(K));
      if (Candidate && Candidate->getOpcode() == Instruction::Sub &&
          Candidate->getOperand(0) == X &&
          Candidate->getOperand(1) == Y) {
        Difference = Candidate;
        Overflow = Sub->getOperand(1 - K);
        break;
      }
    }
    auto *OverflowAnd = dyn_cast<BinaryOperator>(Overflow);
    if (!Difference || !OverflowAnd ||
        OverflowAnd->getOpcode() != Instruction::And)
      continue;
    if (!matchXor(OverflowAnd->getOperand(0), X, Y) ||
        !matchXor(OverflowAnd->getOperand(1), X, Difference))
      continue;
    LHS = X;
    RHS = Y;
    return true;
  }
  return false;
}
bool recoverKernighanPopcount(Function &F) {
  DominatorTree DT(F);
  LoopInfo LI(DT);
  SmallVector<Loop *, 4> Loops(LI.begin(), LI.end());
  for (Loop *L : Loops) {
    if (!L->getSubLoops().empty() || L->getNumBlocks() != 2)
      continue;
    BasicBlock *Header = L->getHeader();
    BasicBlock *Preheader = L->getLoopPreheader();
    BasicBlock *Latch = L->getLoopLatch();
    auto *Branch = dyn_cast<BranchInst>(Header->getTerminator());
    if (!Preheader || !Latch || Latch == Header || !Branch ||
        !Branch->isConditional())
      continue;
    BasicBlock *Continue = L->contains(Branch->getSuccessor(0))
                               ? Branch->getSuccessor(0)
                               : Branch->getSuccessor(1);
    BasicBlock *Exit = Branch->getSuccessor(0) == Continue
                           ? Branch->getSuccessor(1)
                           : Branch->getSuccessor(0);
    if (Continue != Latch || L->contains(Exit))
      continue;
    auto *LatchBranch = dyn_cast<BranchInst>(Latch->getTerminator());
    if (!LatchBranch || !LatchBranch->isUnconditional() ||
        LatchBranch->getSuccessor(0) != Header)
      continue;
    auto *Test = dyn_cast<ICmpInst>(Branch->getCondition());
    if (!Test || (Test->getPredicate() != ICmpInst::ICMP_NE &&
                  Test->getPredicate() != ICmpInst::ICMP_EQ))
      continue;
    auto *X = dyn_cast<PHINode>(Test->getOperand(0));
    Value *Zero = Test->getOperand(1);
    if (!X) {
      X = dyn_cast<PHINode>(Test->getOperand(1));
      Zero = Test->getOperand(0);
    }
    auto *ZeroC = dyn_cast<ConstantInt>(Zero);
    if (!X || !ZeroC || !ZeroC->isZero() ||
        !X->getType()->isIntegerTy() || X->getParent() != Header)
      continue;
    bool LoopOnTrue = Continue == Branch->getSuccessor(0);
    if ((Test->getPredicate() == ICmpInst::ICMP_NE) != LoopOnTrue)
      continue;
    int PreIndex = X->getBasicBlockIndex(Preheader);
    int BackIndex = X->getBasicBlockIndex(Latch);
    if (PreIndex < 0 || BackIndex < 0 || X->getNumIncomingValues() != 2)
      continue;
    Value *Input = X->getIncomingValue(PreIndex);
    Value *XNext = X->getIncomingValue(BackIndex);
    auto *And = dyn_cast<BinaryOperator>(XNext);
    if (!And || And->getOpcode() != Instruction::And)
      continue;
    Value *MinusOne = nullptr;
    if (And->getOperand(0) == X)
      MinusOne = And->getOperand(1);
    else if (And->getOperand(1) == X)
      MinusOne = And->getOperand(0);
    else
      continue;
    auto *Sub = dyn_cast<BinaryOperator>(MinusOne);
    auto *One = Sub ? dyn_cast<ConstantInt>(Sub->getOperand(1)) : nullptr;
    if (!Sub || Sub->getOpcode() != Instruction::Sub ||
        Sub->getOperand(0) != X || !One || !One->isOne())
      continue;

    PHINode *Count = nullptr;
    BinaryOperator *CountNext = nullptr;
    for (PHINode &Phi : Header->phis()) {
      if (&Phi == X || !Phi.getType()->isIntegerTy() ||
          Phi.getType() != X->getType())
        continue;
      PreIndex = Phi.getBasicBlockIndex(Preheader);
      BackIndex = Phi.getBasicBlockIndex(Latch);
      if (PreIndex < 0 || BackIndex < 0 || Phi.getNumIncomingValues() != 2)
        continue;
      auto *Init = dyn_cast<ConstantInt>(Phi.getIncomingValue(PreIndex));
      auto *Inc = dyn_cast<BinaryOperator>(Phi.getIncomingValue(BackIndex));
      if (!Init || !Init->isZero() || !Inc ||
          Inc->getOpcode() != Instruction::Add)
        continue;
      ConstantInt *IncOne = dyn_cast<ConstantInt>(Inc->getOperand(1));
      Value *Counter = Inc->getOperand(0);
      if (!IncOne || !IncOne->isOne()) {
        IncOne = dyn_cast<ConstantInt>(Inc->getOperand(0));
        Counter = Inc->getOperand(1);
      }
      if (!IncOne || !IncOne->isOne() || Counter != &Phi)
        continue;
      Count = &Phi;
      CountNext = Inc;
      break;
    }
    if (!Count || !CountNext)
      continue;

    bool Safe = true;
    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        if (&I == X || &I == Count || &I == Test || &I == And ||
            &I == Sub || &I == CountNext || &I == Branch ||
            &I == LatchBranch || isa<DbgInfoIntrinsic>(&I))
          continue;
        Safe = false;
        break;
      }
      if (!Safe)
        break;
    }
    if (!Safe)
      continue;
    for (BasicBlock *BB : L->blocks())
      for (Instruction &I : *BB)
        for (User *U : I.users())
          if (auto *UseI = dyn_cast<Instruction>(U))
            if (!L->contains(UseI->getParent()) && &I != Count)
              Safe = false;
    if (!Safe)
      continue;

    SmallVector<WeakTrackingVH, 16> Dead;
    for (BasicBlock *BB : L->blocks())
      for (Instruction &I : *BB)
        Dead.push_back(&I);
    IRBuilder<NoFolder> B(Branch);
    Function *Ctpop = Intrinsic::getOrInsertDeclaration(
        F.getParent(), Intrinsic::ctpop, {X->getType()});
    Value *Pop = B.CreateCall(Ctpop, {Input});
    auto *PopI = cast<Instruction>(Pop);
    copyProvenance(PopI, Count);
    PopI->copyMetadata(*Count);
    Count->replaceAllUsesWith(Pop);
    BranchInst::Create(Exit, Branch);
    Branch->eraseFromParent();
    SmallVector<BasicBlock *, 2> DeadBlocks;
    for (BasicBlock *BB : L->blocks())
      if (BB != Header)
        DeadBlocks.push_back(BB);
    DeleteDeadBlocks(DeadBlocks);
    RecursivelyDeleteTriviallyDeadInstructionsPermissive(Dead);
    return true;
  }
  return false;
}
} // namespace

PreservedAnalyses revenant::IdiomsPass::run(Function &F,
                                            FunctionAnalysisManager &) {
  bool Changed = false;
  LLVMContext &Ctx = F.getContext();

  // Phase 1: verified magic division/modulo -> udiv/sdiv/urem/srem.
  // Scan each block in reverse so an outer root is matched before the
  // inner nodes of its own DAG.
  {
    std::vector<Quotient> Quotients;
    SmallPtrSet<Value *, 16> Consumed;
    for (BasicBlock &BB : F) {
      for (auto It = BB.rbegin(); It != BB.rend(); ++It) {
        Instruction *I = &*It;
        if (!isa<BinaryOperator>(I) && !isa<CastInst>(I) &&
            !isa<SelectInst>(I))
          continue;
        if (!I->getType()->isIntegerTy())
          continue;
        if (Consumed.count(I))
          continue;
        if (auto *BO = dyn_cast<BinaryOperator>(I)) {
          auto Op = BO->getOpcode();
          if (Op == Instruction::UDiv || Op == Instruction::SDiv ||
              Op == Instruction::URem || Op == Instruction::SRem)
            continue;
        }
        if (auto Q = analyzeQuotient(I)) {
          for (Value *M : Q->Members)
            Consumed.insert(M);
          Quotients.push_back(std::move(*Q));
        }
      }
    }
    // NoFolder: the rewrites below cast the result to an Instruction, which a
    // constant-folding builder would not return for constant operands.
    IRBuilder<NoFolder> B(Ctx);
    for (Quotient &Q : Quotients) {
      B.SetInsertPoint(Q.Root);
      Instruction::BinaryOps Op =
          Q.Signed ? Instruction::SDiv : Instruction::UDiv;
      Value *D = ConstantInt::get(Q.Root->getType(), (uint64_t)Q.D, true);
      Value *Div = B.CreateBinOp(Op, Q.Dividend, D);
      auto *DI = cast<Instruction>(Div);
      copyProvenance(DI, Q.Root);
      DI->copyMetadata(*Q.Root);
      Q.Root->replaceAllUsesWith(Div);
      // Recycle the slot: keep the name on the new divide for FileCheck.
      Div->takeName(Q.Root);
      Q.Root->eraseFromParent();
      Changed = true;
    }
    // Phase 2: rem formation on the rewritten quotients (plus any
    // pre-existing udiv/sdiv + mul shapes).
    std::vector<Quotient> Rems;
    for (Instruction &I : instructions(F)) {
      auto *BO = dyn_cast<BinaryOperator>(&I);
      if (!BO)
        continue;
      if (auto Q = analyzeRem(BO))
        Rems.push_back(std::move(*Q));
    }
    for (Quotient &Q : Rems) {
      B.SetInsertPoint(Q.Root);
      Instruction::BinaryOps Op =
          Q.Signed ? Instruction::SRem : Instruction::URem;
      Value *D = ConstantInt::get(Q.Root->getType(), (uint64_t)Q.D, true);
      Value *Rem = B.CreateBinOp(Op, Q.Dividend, D);
      auto *RI = cast<Instruction>(Rem);
      copyProvenance(RI, Q.Root);
      RI->copyMetadata(*Q.Root);
      Q.Root->replaceAllUsesWith(Rem);
      Rem->takeName(Q.Root);
      Q.Root->eraseFromParent();
      Changed = true;
    }
    if (!Quotients.empty() || !Rems.empty()) {
      SmallVector<WeakTrackingVH, 16> Dead;
      for (auto &Q : Quotients)
        for (Value *M : Q.Members)
          if (auto *I = dyn_cast<Instruction>(M))
            if (I->getParent() && I->use_empty())
              Dead.push_back(I);
      for (auto &Q : Rems)
        for (Value *M : Q.Members)
          if (auto *I = dyn_cast<Instruction>(M))
            if (I->getParent() && I->use_empty())
              Dead.push_back(I);
      RecursivelyDeleteTriviallyDeadInstructionsPermissive(Dead);
    }
  }

  // Phase 3: single-instruction idioms, iterate to a fixpoint (bounded).
  for (int Iter = 0; Iter < 4; ++Iter) {
    (void)Iter;
    bool LocalChanged = false;
    IRBuilder<NoFolder> B(Ctx);
    for (Instruction &I : make_early_inc_range(instructions(F))) {
      if (hasOpaque(&I))
        continue;
      Value *CmpLHS = nullptr;
      Value *CmpRHS = nullptr;
      if (matchSignedCompare(&I, CmpLHS, CmpRHS)) {
        SmallVector<WeakTrackingVH, 16> Dead;
        SmallVector<Value *, 16> Work{&I};
        SmallPtrSet<Value *, 16> Seen;
        while (!Work.empty()) {
          Value *V = Work.pop_back_val();
          auto *Node = dyn_cast<Instruction>(V);
          if (!Node || !Seen.insert(V).second)
            continue;
          Dead.push_back(Node);
          for (Value *Operand : Node->operands())
            Work.push_back(Operand);
        }
        B.SetInsertPoint(&I);
        Function *Scmp = Intrinsic::getOrInsertDeclaration(
            F.getParent(), Intrinsic::scmp,
            {CmpLHS->getType(), CmpLHS->getType()});
        Value *Call = B.CreateCall(Scmp, {CmpLHS, CmpRHS});
        auto *CI = cast<Instruction>(Call);
        copyProvenance(CI, &I);
        CI->copyMetadata(I);
        I.replaceAllUsesWith(Call);
        Call->takeName(&I);
        I.eraseFromParent();
        RecursivelyDeleteTriviallyDeadInstructionsPermissive(Dead);
        LocalChanged = true;
        continue;
      }
      // -- mul by power of two -> shl. Mul by any other constant is
      //    already the canonical readable form and stays.
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        if (BO->getOpcode() == Instruction::Mul) {
          ConstantInt *C = dyn_cast<ConstantInt>(BO->getOperand(1));
          Value *X = BO->getOperand(0);
          if (!C || !C->getValue().isPowerOf2()) {
            C = dyn_cast<ConstantInt>(BO->getOperand(0));
            X = BO->getOperand(1);
          }
          if (C && C->getValue().isPowerOf2() && !C->getValue().isOne()) {
            B.SetInsertPoint(BO);
            Value *Sh = ConstantInt::get(
                BO->getType(), C->getValue().exactLogBase2());
            Value *S = B.CreateShl(X, Sh);
            auto *SI2 = cast<Instruction>(S);
            copyProvenance(SI2, BO);
            SI2->copyMetadata(*BO);
            BO->replaceAllUsesWith(S);
            S->takeName(BO);
            BO->eraseFromParent();
            LocalChanged = true;
            continue;
          }
        }
      }
      // -- select-based abs/min/max/clamp -> intrinsics.
      if (auto *SI = dyn_cast<SelectInst>(&I)) {
        Value *X = nullptr;
        if (matchAbsFromSelect(SI, X)) {
          B.SetInsertPoint(SI);
          Function *Abs = Intrinsic::getOrInsertDeclaration(
              F.getParent(), Intrinsic::abs, {SI->getType()});
          Value *Call =
              B.CreateCall(Abs, {X, ConstantInt::getFalse(Ctx)});
          auto *CI = cast<Instruction>(Call);
          copyProvenance(CI, SI);
          CI->copyMetadata(*SI);
          SI->replaceAllUsesWith(Call);
          Call->takeName(SI);
          SI->eraseFromParent();
          LocalChanged = true;
          continue;
        }
        // Select forms of unsigned saturation produced by ordinary C/C++
        // arithmetic. Keep the operands tied to the exact add/sub node.
        if (auto *IC = dyn_cast<ICmpInst>(SI->getCondition())) {
          Intrinsic::ID ID = Intrinsic::not_intrinsic;
          Value *LHS = nullptr;
          Value *RHS = nullptr;
          auto *Sub = dyn_cast<BinaryOperator>(SI->getFalseValue());
          auto *Zero = dyn_cast<ConstantInt>(SI->getTrueValue());
          if (Sub && Sub->getOpcode() == Instruction::Sub && Zero &&
              Zero->isZero() &&
              IC->getPredicate() == ICmpInst::ICMP_ULT &&
              IC->getOperand(0) == Sub->getOperand(0) &&
              IC->getOperand(1) == Sub->getOperand(1)) {
            ID = Intrinsic::usub_sat;
            LHS = Sub->getOperand(0);
            RHS = Sub->getOperand(1);
          }
          auto *Add = dyn_cast<BinaryOperator>(SI->getFalseValue());
          auto *Max = dyn_cast<ConstantInt>(SI->getTrueValue());
          if (Add && Add->getOpcode() == Instruction::Add && Max &&
              Max->isMinusOne() &&
              IC->getPredicate() == ICmpInst::ICMP_ULT &&
              IC->getOperand(0) == Add &&
              (IC->getOperand(1) == Add->getOperand(0) ||
               IC->getOperand(1) == Add->getOperand(1))) {
            ID = Intrinsic::uadd_sat;
            LHS = Add->getOperand(0);
            RHS = Add->getOperand(1);
          }
          if (ID != Intrinsic::not_intrinsic) {
            B.SetInsertPoint(SI);
            Function *Decl = Intrinsic::getOrInsertDeclaration(
                F.getParent(), ID, {SI->getType()});
            Value *Call = B.CreateCall(Decl, {LHS, RHS});
            auto *CI = cast<Instruction>(Call);
            copyProvenance(CI, SI);
            CI->copyMetadata(*SI);
            SI->replaceAllUsesWith(Call);
            Call->takeName(SI);
            SI->eraseFromParent();
            LocalChanged = true;
            continue;
          }
        }
        // icmp+select min/max (signed and unsigned, either order).
        if (auto *IC = dyn_cast<ICmpInst>(SI->getCondition())) {
          ICmpInst::Predicate P = IC->getPredicate();
          Value *A = IC->getOperand(0), *Bv = IC->getOperand(1);
          Value *T = SI->getTrueValue(), *Fv = SI->getFalseValue();
          bool TIsA = (T == A && Fv == Bv);
          bool FIsA = (T == Bv && Fv == A);
          if (TIsA || FIsA) {
            Intrinsic::ID ID = Intrinsic::not_intrinsic;
            switch (P) {
            case ICmpInst::ICMP_SLT:
            case ICmpInst::ICMP_SLE:
              ID = TIsA ? Intrinsic::smin : Intrinsic::smax;
              break;
            case ICmpInst::ICMP_SGT:
            case ICmpInst::ICMP_SGE:
              ID = TIsA ? Intrinsic::smax : Intrinsic::smin;
              break;
            case ICmpInst::ICMP_ULT:
            case ICmpInst::ICMP_ULE:
              ID = TIsA ? Intrinsic::umin : Intrinsic::umax;
              break;
            case ICmpInst::ICMP_UGT:
            case ICmpInst::ICMP_UGE:
              ID = TIsA ? Intrinsic::umax : Intrinsic::umin;
              break;
            default:
              break;
            }
            if (ID != Intrinsic::not_intrinsic) {
              B.SetInsertPoint(SI);
              Function *Decl = Intrinsic::getOrInsertDeclaration(
                  F.getParent(), ID, {SI->getType()});
              Value *Call = B.CreateCall(Decl, {A, Bv});
              auto *CI = cast<Instruction>(Call);
              copyProvenance(CI, SI);
              CI->copyMetadata(*SI);
              SI->replaceAllUsesWith(Call);
              Call->takeName(SI);
              SI->eraseFromParent();
              LocalChanged = true;
              continue;
            }
          }
        }
      }
      // -- rotates: (x << c) | (x >> (w-c)) -> llvm.fshl. The ror mirror
      //    (operands swapped) matches the same pattern; the frontend
      //    prints whichever direction the shift amounts describe.
      {
        // Match (x<<c)|(x>>(w-c)) in either or-operand order; the ror
        // mirror (x>>c)|(x<<(w-c)) matches with the shift roles swapped
        // and folds to fshl(x,x,w-c) (== fshr(x,x,c)).
        Value *ShlX = nullptr, *ShlC = nullptr;
        Value *ShrX = nullptr, *ShrC = nullptr;
        bool Rot = false;
        Value *Amt = nullptr;
        auto tryPair = [&](Value *A, Value *B) {
          ShlX = ShlC = ShrX = ShrC = nullptr;
          if (match(A, m_Shl(m_Value(ShlX), m_Value(ShlC))) &&
              match(B, m_LShr(m_Value(ShrX), m_Value(ShrC))) &&
              ShlX == ShrX && I.getType()->isIntegerTy()) {
            unsigned W = I.getType()->getIntegerBitWidth();
            Value *SubA = nullptr, *SubB = nullptr;
            if (match(ShrC, m_Sub(m_Value(SubA), m_Value(SubB)))) {
              if (auto *CW = dyn_cast<ConstantInt>(SubA))
                if (CW->getZExtValue() == W && SubB == ShlC) {
                  Rot = true; // rol: fshl(x,x,c).
                  Amt = ShlC;
                  return;
                }
            }
            if (match(ShlC, m_Sub(m_Value(SubA), m_Value(SubB)))) {
              if (auto *CW = dyn_cast<ConstantInt>(SubA))
                if (CW->getZExtValue() == W && SubB == ShrC) {
                  Rot = true; // ror: fshl(x,x,w-c).
                  Amt = ShlC;
                  return;
                }
            }
          }
        };
        if (auto *BO = dyn_cast<BinaryOperator>(&I);
            BO && BO->getOpcode() == Instruction::Or) {
          tryPair(BO->getOperand(0), BO->getOperand(1));
          if (!Rot)
            tryPair(BO->getOperand(1), BO->getOperand(0));
          if (Rot) {
            B.SetInsertPoint(&I);
            Function *Fshl = Intrinsic::getOrInsertDeclaration(
                F.getParent(), Intrinsic::fshl, {I.getType()});
            Value *Call = B.CreateCall(Fshl, {ShlX, ShlX, Amt});
            auto *CI = cast<Instruction>(Call);
            copyProvenance(CI, &I);
            CI->copyMetadata(I);
            I.replaceAllUsesWith(Call);
            Call->takeName(&I);
            I.eraseFromParent();
            LocalChanged = true;
            continue;
          }
        }
      }
      // -- byte-swap: shift-and-or byte shuffle -> llvm.bswap.
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        if (BO->getOpcode() == Instruction::Or) {
          // Collect the OR tree leaves and prove that they select each input
          // byte exactly once, in reverse order. Merely seeing shifts of one
          // base value is not sufficient to establish a byte swap.
          SmallVector<Value *, 8> Leaves;
          SmallVector<Value *, 8> Stack{BO->getOperand(0),
                                        BO->getOperand(1)};
          while (!Stack.empty()) {
            Value *V = Stack.pop_back_val();
            if (auto *O = dyn_cast<BinaryOperator>(V);
                O && O->getOpcode() == Instruction::Or) {
              Stack.push_back(O->getOperand(0));
              Stack.push_back(O->getOperand(1));
            } else {
              Leaves.push_back(V);
            }
          }
          if (auto *IntTy = dyn_cast<IntegerType>(I.getType())) {
            unsigned W = IntTy->getBitWidth();
            unsigned ByteCount = W / 8;
            if ((W == 16 || W == 32 || W == 64) &&
                Leaves.size() == ByteCount) {
              Value *Base = nullptr;
              bool Ok = true;
              SmallVector<bool, 8> DestSeen(ByteCount, false);
              SmallVector<bool, 8> SourceSeen(ByteCount, false);
              for (Value *L : Leaves) {
                Value *Shifted = L;
                APInt Mask(W, 0);
                bool HasMask = false;
                if (auto *And = dyn_cast<BinaryOperator>(L);
                    And && And->getOpcode() == Instruction::And) {
                  auto *MC = dyn_cast<ConstantInt>(And->getOperand(1));
                  if (!MC)
                    MC = dyn_cast<ConstantInt>(And->getOperand(0));
                  if (!MC) {
                    Ok = false;
                    break;
                  }
                  Mask = MC->getValue();
                  HasMask = true;
                  Shifted = And->getOperand(0) == MC ? And->getOperand(1)
                                                      : And->getOperand(0);
                }
                auto *Shift = dyn_cast<BinaryOperator>(Shifted);
                if (!Shift || (Shift->getOpcode() != Instruction::Shl &&
                               Shift->getOpcode() != Instruction::LShr)) {
                  Ok = false;
                  break;
                }
                auto *Amount = dyn_cast<ConstantInt>(Shift->getOperand(1));
                if (!Amount || Amount->getValue().uge(W)) {
                  Ok = false;
                  break;
                }
                unsigned ShiftBy = Amount->getZExtValue();
                bool IsRight = Shift->getOpcode() == Instruction::LShr;
                unsigned DestShift = 0;
                if (HasMask) {
                  unsigned First = Mask.countTrailingZeros();
                  APInt ExpectedMask(W, 255);
                  ExpectedMask <<= First;
                  if ((First % 8) != 0 || Mask != ExpectedMask) {
                    Ok = false;
                    break;
                  }
                  DestShift = First;
                } else {
                  APInt NaturalMask(W, 0);
                  NaturalMask.setAllBits();
                  if (IsRight)
                    NaturalMask = NaturalMask.lshr(ShiftBy);
                  else
                    NaturalMask = NaturalMask.shl(ShiftBy);
                  DestShift = IsRight ? 0 : ShiftBy;
                  if (NaturalMask != (APInt(W, 255) << DestShift)) {
                    Ok = false;
                    break;
                  }
                }
                if (DestShift >= W || (DestShift % 8) != 0) {
                  Ok = false;
                  break;
                }
                APInt ByteMask(W, 255);
                ByteMask <<= DestShift;
                if (HasMask && Mask != ByteMask) {
                  Ok = false;
                  break;
                }
                int SourceShift = IsRight
                                      ? static_cast<int>(ShiftBy + DestShift)
                                      : static_cast<int>(DestShift) -
                                            static_cast<int>(ShiftBy);
                if (SourceShift < 0 || (SourceShift % 8) != 0 ||
                    SourceShift + 8 > static_cast<int>(W)) {
                  Ok = false;
                  break;
                }
                unsigned DestByte = DestShift / 8;
                unsigned SourceByte = static_cast<unsigned>(SourceShift) / 8;
                if (DestSeen[DestByte] || SourceSeen[SourceByte] ||
                    SourceByte != ByteCount - DestByte - 1) {
                  Ok = false;
                  break;
                }
                DestSeen[DestByte] = true;
                SourceSeen[SourceByte] = true;
                Value *SX = Shift->getOperand(0);
                if (!Base)
                  Base = SX;
                else if (Base != SX)
                  Ok = false;
              }
              if (Ok && Base &&
                  llvm::all_of(DestSeen, [](bool Seen) { return Seen; })) {
                B.SetInsertPoint(BO);
                Function *Bswap = Intrinsic::getOrInsertDeclaration(
                    F.getParent(), Intrinsic::bswap, {I.getType()});
                Value *Call = B.CreateCall(Bswap, {Base});
                auto *CI = cast<Instruction>(Call);
                copyProvenance(CI, BO);
                CI->copyMetadata(*BO);
                BO->replaceAllUsesWith(Call);
                Call->takeName(BO);
                BO->eraseFromParent();
                LocalChanged = true;
                continue;
              }
            }
          }
        }
      }
      // -- xor(x,-1)+1 -> sub(0,x), the abs building block.
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        if (BO->getOpcode() == Instruction::Add) {
          for (int K = 0; K < 2; ++K) {
            auto *XorV =
                dyn_cast<BinaryOperator>(BO->getOperand(K));
            auto *One = dyn_cast<ConstantInt>(BO->getOperand(1 - K));
            if (!XorV || XorV->getOpcode() != Instruction::Xor || !One ||
                !One->isOne())
              continue;
            Value *Xx = nullptr;
            if (match(XorV, m_Xor(m_Value(Xx), m_AllOnes()))) {
              B.SetInsertPoint(BO);
              Value *S = B.CreateSub(
                  ConstantInt::getNullValue(BO->getType()), Xx);
              auto *SI2 = cast<Instruction>(S);
              copyProvenance(SI2, BO);
              SI2->copyMetadata(*BO);
              BO->replaceAllUsesWith(S);
              S->takeName(BO);
              BO->eraseFromParent();
              LocalChanged = true;
              break;
            }
          }
          if (LocalChanged)
            continue;
        }
      }
    }
    Changed |= LocalChanged;
    if (!LocalChanged)
      break;
  }

  Changed |= recoverKernighanPopcount(F);
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

llvm::PassPluginLibraryInfo getPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantIdioms", "0.1",
          [](llvm::PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](llvm::StringRef Name, llvm::FunctionPassManager &FPM,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "revenant-idioms") {
                    FPM.addPass(revenant::IdiomsPass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
