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
// - abs/min/max/clamp, three-way comparisons and unsigned saturation
//   intrinsics return to source-level compare/subtract/select operations.
//   Signed subtraction-flag chains also become comparisons/selects, never
//   llvm.scmp.
// - Kernighan popcount loops, rotate idioms and plain-C byte swaps remain
//   loops or bitwise arithmetic. Builtin intrinsics such as llvm.bswap pass
//   through unchanged.
// - Bit tests already use the source-level `and`/`icmp` IR form.
// - Multiplication strength reduction is not reversed speculatively:
//   multiply and shift spellings can both be source-level C.
// - Memset/memcpy loop recovery belongs to revenant-patterns, not this pass.

#include "revenant/Transforms/Idioms/Idioms.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
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

// Argument count the phase-2 intrinsic rewrites read for `ID`. An intrinsic
// outside this set takes no rewrite at all; one with fewer operands than its
// rewrite needs is left alone. Lifted code meets zero-argument intrinsics
// such as `llvm.x86.rdrand.32()`.
unsigned idiomArity(Intrinsic::ID ID) {
  switch (ID) {
  case Intrinsic::scmp:
  case Intrinsic::ucmp:
  case Intrinsic::smin:
  case Intrinsic::smax:
  case Intrinsic::umin:
  case Intrinsic::umax:
  case Intrinsic::usub_sat:
  case Intrinsic::uadd_sat:
    return 2;
  case Intrinsic::abs:
    return 1;
  default:
    return ~0u;
  }
}

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
  if (Input && !isa<ConstantInt>(V) && !isa<Instruction>(V) && V != Input)
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
    // A comparison of a function argument against a constant is
    // source-level control flow (`x == 42`), never compiler fixup: no
    // witness set can prove it equivalent to a divide. Both
    // witness-hole fixtures key on it (one via select, one via
    // zext+add). End the proof before evaluating either side.
    Value *LHS = IC->getOperand(0), *RHS = IC->getOperand(1);
    auto isArgCompare = [&](Value *V) {
      if (isa<ConstantInt>(V))
        return false;
      if (auto *A = dyn_cast<Argument>(V))
        return true;
      if (V == Dividend || V == Input)
        return true;
      return false;
    };
    if ((isa<ConstantInt>(RHS) && isArgCompare(LHS)) ||
        (isa<ConstantInt>(LHS) && isArgCompare(RHS))) {
      Dividend = nullptr;
      return false;
    }
    APInt A128(128, 0), B128(128, 0);
    unsigned W =
        IC->getOperand(0)->getType()->getScalarType()->getIntegerBitWidth();
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
  // The dividend must be the function's input (or the pinned W->2W
  // source): any other leaf is not a quotient of the input.
  if (DivInput) {
    if (Div != DivInput)
      return std::nullopt;
  } else if (!isa<Argument>(Div)) {
    return std::nullopt;
  }
  // Soundness over witnesses: the rewrite replaces the root with a pure
  // divide of the dividend, so the DAG must be a pure function of that
  // leaf. Any icmp in the function comparing a dividend-derived value
  // against a constant is a data-dependent branch (both witness-hole
  // fixtures key on `x == 42`); witnesses cannot prove it away. Walk
  // every icmp in the function, not just the evaluated DAG: the
  // arithmetic fixture's compare never enters the trunc's DAG, it
  // observes the leaf from outside it.
  for (BasicBlock &BB : *Root->getParent()->getParent()) {
    for (Instruction &I : BB) {
      auto *IC = dyn_cast<ICmpInst>(&I);
      if (!IC)
        continue;
      Value *LHS = IC->getOperand(0), *RHS = IC->getOperand(1);
      if (!isa<ConstantInt>(LHS) && !isa<ConstantInt>(RHS))
        continue;
      Value *Side = isa<ConstantInt>(LHS) ? RHS : LHS;
      SmallPtrSet<const Value *, 16> Seen;
      SmallVector<const Value *, 16> Work{Side};
      bool OnDividend = false;
      while (!Work.empty()) {
        const Value *V = Work.pop_back_val();
        if (!Seen.insert(V).second)
          continue;
        if (V == Div || V == DivInput) {
          OnDividend = true;
          break;
        }
        if (auto *LI = dyn_cast<Instruction>(V)) {
          if (auto *A = dyn_cast<Argument>(LI)) {
            if (A == Div)
              OnDividend = true;
            break;
          }
          for (Value *Operand : LI->operands())
            Work.push_back(Operand);
        }
      }
      if (OnDividend)
        return std::nullopt;
    }
  }
  // The rewrite replaces the root value, so an outside observer is only
  // dangerous when it branches on the raw dividend around the DAG: the
  // witness-hole select takes the trunc as one arm and `q + 1` as the
  // other under `x == 42`. No witness set can prove that branch away.
  // Bail when a live select in the same function takes a member of this
  // DAG as an arm while its condition reads the dividend. Pure
  // arithmetic reuses (the urem7 quotient feeding `q * -7`) stay legal:
  // they compute from the quotient, not around it.
  {
    Value *D3 = nullptr;
    bool W3 = false, C3 = false;
    SmallVector<Value *, 8> Reachable;
    evalRoot(Root, Zero, W, D3, W3, C3, &Reachable, DivInput);
    SmallPtrSet<const Value *, 32> InDag;
    InDag.insert(Root);
    for (Value *M : Reachable)
      InDag.insert(M);
    InDag.insert(Div);
    for (BasicBlock &BB : *Root->getParent()->getParent()) {
      for (Instruction &I : BB) {
        auto *SI = dyn_cast<SelectInst>(&I);
        if (!SI)
          continue;
        bool ArmInDag = InDag.count(SI->getTrueValue()) ||
                        InDag.count(SI->getFalseValue());
        if (!ArmInDag)
          continue;
        SmallPtrSet<const Value *, 16> SeenC;
        SmallVector<const Value *, 16> WorkC{SI->getCondition()};
        bool CondOnDiv = false;
        while (!WorkC.empty()) {
          const Value *C = WorkC.pop_back_val();
          if (!SeenC.insert(C).second)
            continue;
          if (C == Div) {
            CondOnDiv = true;
            break;
          }
          if (auto *CI = dyn_cast<Instruction>(C))
            for (Value *Operand : CI->operands())
              WorkC.push_back(Operand);
        }
        if (CondOnDiv)
          return std::nullopt;
      }
    }
  }

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


// Recover signed compare flags directly to source-level comparisons, never
// to llvm.scmp, which is an optimizer representation rather than C source IR.
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
    auto *Gt = dyn_cast<ICmpInst>(And->getOperand(0));
    auto *Ne = dyn_cast<ICmpInst>(And->getOperand(1));
    if (!Gt || !Ne) {
      Gt = dyn_cast<ICmpInst>(And->getOperand(1));
      Ne = dyn_cast<ICmpInst>(And->getOperand(0));
    }
    auto *MinusOne = Gt ? dyn_cast<ConstantInt>(Gt->getOperand(1)) : nullptr;
    if (!Gt || !Ne || Gt->getPredicate() != ICmpInst::ICMP_SGT ||
        Ne->getPredicate() != ICmpInst::ICMP_NE || !MinusOne ||
        !MinusOne->isMinusOne())
      continue;
    Value *A = Less->getOperand(0);
    Value *B = Less->getOperand(1);
    if (Ne->getOperand(0) != A || Ne->getOperand(1) != B)
      continue;
    auto *OverflowAdjusted = dyn_cast<BinaryOperator>(Gt->getOperand(0));
    if (!OverflowAdjusted || OverflowAdjusted->getOpcode() != Instruction::Xor)
      continue;
    auto matchesXor = [](Value *V, Value *X, Value *Y) {
      auto *Xor = dyn_cast<BinaryOperator>(V);
      return Xor && Xor->getOpcode() == Instruction::Xor &&
             ((Xor->getOperand(0) == X && Xor->getOperand(1) == Y) ||
              (Xor->getOperand(0) == Y && Xor->getOperand(1) == X));
    };
    Value *Difference = nullptr;
    Value *Overflow = nullptr;
    for (unsigned K = 0; K != 2; ++K) {
      auto *Sub = dyn_cast<BinaryOperator>(OverflowAdjusted->getOperand(K));
      if (Sub && Sub->getOpcode() == Instruction::Sub &&
          Sub->getOperand(0) == A && Sub->getOperand(1) == B) {
        Difference = Sub;
        Overflow = OverflowAdjusted->getOperand(1 - K);
        break;
      }
    }
    auto *OverflowAnd = dyn_cast<BinaryOperator>(Overflow);
    if (!Difference || !OverflowAnd ||
        OverflowAnd->getOpcode() != Instruction::And ||
        !matchesXor(OverflowAnd->getOperand(0), A, B) ||
        !matchesXor(OverflowAnd->getOperand(1), A, Difference))
      continue;
    LHS = A;
    RHS = B;
    return true;
  }
  return false;
}
} // namespace

PreservedAnalyses revenant::IdiomsPass::run(Function &F,
                                            FunctionAnalysisManager &) {
  bool Changed = false;
  LLVMContext &Ctx = F.getContext();

  // Phase 0: the trap marker a compiler leaves behind a noreturn call
  // (int3 after a throw, __fastfail after a panic) is not part of the
  // source. The block already ends dead, so the interrupt call is noise
  // the compiler's own IR does not carry: dropping it brings the two
  // closer. An interrupt in the middle of a live block stays.
  {
    SmallVector<CallBase *, 4> Markers;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        const Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->getName() != "llvm.x86.int")
          continue;
        Instruction *Next = I.getNextNode();
        if (!Next || !isa<UnreachableInst>(Next))
          continue;
        Markers.push_back(CB);
      }
    }
    for (CallBase *CB : Markers)
      CB->eraseFromParent();
    Changed |= !Markers.empty();
  }

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
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        B.SetInsertPoint(Call);
        Function *Callee = Call->getCalledFunction();
        if (Callee && Callee->isIntrinsic() &&
            Call->arg_size() >= idiomArity(Callee->getIntrinsicID())) {
          Intrinsic::ID ID = Callee->getIntrinsicID();
          Value *A = Call->getArgOperand(0);
          Value *R = nullptr;
          Value *Cond = nullptr;
          if (ID == Intrinsic::scmp || ID == Intrinsic::ucmp) {
            Value *Bv = Call->getArgOperand(1);
            bool Signed = ID == Intrinsic::scmp;
            Value *Less = B.CreateICmp(
                Signed ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT, A, Bv);
            Value *Greater = B.CreateICmp(
                Signed ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_UGT, A, Bv);
            Value *One = ConstantInt::get(Call->getType(), 1);
            Value *MinusOne = ConstantInt::getSigned(Call->getType(), -1);
            Value *Zero = ConstantInt::getNullValue(Call->getType());
            R = B.CreateSelect(Less, MinusOne,
                               B.CreateSelect(Greater, One, Zero));
          } else if (ID == Intrinsic::smin || ID == Intrinsic::smax ||
                     ID == Intrinsic::umin || ID == Intrinsic::umax) {
            Value *Bv = Call->getArgOperand(1);
            bool Signed = ID == Intrinsic::smin || ID == Intrinsic::smax;
            bool Min = ID == Intrinsic::smin || ID == Intrinsic::umin;
            Cond = B.CreateICmp(
                Signed ? (Min ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_SGT)
                       : (Min ? ICmpInst::ICMP_ULT : ICmpInst::ICMP_UGT),
                A, Bv);
            R = B.CreateSelect(Cond, A, Bv);
          } else if (ID == Intrinsic::abs) {
            Cond = B.CreateICmpSLT(
                A, ConstantInt::getNullValue(A->getType()));
            Value *Neg = B.CreateSub(ConstantInt::getNullValue(A->getType()),
                                     A);
            R = B.CreateSelect(Cond, Neg, A);
          } else if (ID == Intrinsic::usub_sat) {
            Value *Bv = Call->getArgOperand(1);
            Cond = B.CreateICmpULT(A, Bv);
            Value *Difference = B.CreateSub(A, Bv);
            R = B.CreateSelect(Cond, ConstantInt::getNullValue(A->getType()),
                               Difference);
          } else if (ID == Intrinsic::uadd_sat) {
            Value *Bv = Call->getArgOperand(1);
            Value *Sum = B.CreateAdd(A, Bv);
            Cond = B.CreateICmpULT(Sum, A);
            R = B.CreateSelect(Cond, ConstantInt::getAllOnesValue(A->getType()),
                               Sum);
          }
          if (R) {
            auto *Replacement = cast<Instruction>(R);
            copyProvenance(Replacement, Call);
            Replacement->copyMetadata(*Call);
            Call->replaceAllUsesWith(R);
            R->takeName(Call);
            Call->eraseFromParent();
            LocalChanged = true;
            continue;
          }
        }
      }
      Value *CmpLHS = nullptr;
      Value *CmpRHS = nullptr;
      if (matchSignedCompare(&I, CmpLHS, CmpRHS)) {
        B.SetInsertPoint(&I);
        Value *Less = B.CreateICmpSLT(CmpLHS, CmpRHS);
        Value *Greater = B.CreateICmpSGT(CmpLHS, CmpRHS);
        Value *MinusOne = ConstantInt::getSigned(I.getType(), -1);
        Value *One = ConstantInt::get(I.getType(), 1);
        Value *Zero = ConstantInt::getNullValue(I.getType());
        Value *Result =
            B.CreateSelect(Less, MinusOne,
                           B.CreateSelect(Greater, One, Zero));
        auto *ResultI = cast<Instruction>(Result);
        copyProvenance(ResultI, &I);
        ResultI->copyMetadata(I);
        // The flag chain that produced the sign word (sub, the overflow
        // correction and their compares/extensions) feeds nothing else once
        // the compare/select replaces its root: collect it so the dead nodes
        // go away instead of lingering after the last idiom round.
        SmallVector<WeakTrackingVH, 16> Dead;
        SmallVector<Value *, 16> Work{I.operands()};
        SmallPtrSet<Value *, 16> Seen;
        while (!Work.empty()) {
          Value *V = Work.pop_back_val();
          if (!Seen.insert(V).second)
            continue;
          Dead.push_back(V);
          if (auto *Node = dyn_cast<Instruction>(V))
            for (Value *Operand : Node->operands())
              Work.push_back(Operand);
        }
        I.replaceAllUsesWith(Result);
        Result->takeName(&I);
        I.eraseFromParent();
        RecursivelyDeleteTriviallyDeadInstructionsPermissive(Dead);
        LocalChanged = true;
        continue;
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

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

namespace {
/// The intrinsic a compare-and-select computes, in the shapes `revenant-idioms`
/// expands them to and the orientations instcombine accepts; null when it is
/// none of them.
Value *refoldSelect(SelectInst &S, IRBuilder<> &B) {
  Type *Ty = S.getType();
  if (!Ty->isIntegerTy())
    return nullptr;
  Value *L = nullptr, *R = nullptr;
  Instruction::CastOps Cast = Instruction::CastOps(0);
  SelectPatternResult SPR = matchSelectPattern(&S, L, R, &Cast);
  if (!Cast && SelectPatternResult::isMinOrMax(SPR.Flavor) &&
      L->getType() == Ty && R->getType() == Ty) {
    Intrinsic::ID ID = getMinMaxIntrinsic(SPR.Flavor);
    return B.CreateBinaryIntrinsic(ID, L, R);
  }
  Value *A = nullptr, *Bv = nullptr;
  CmpPredicate P;
  // abs: a < 0 ? -a : a
  if (match(&S, m_Select(m_ICmp(P, m_Value(A), m_Zero()), m_Neg(m_Deferred(A)),
                         m_Deferred(A))) &&
      P == ICmpInst::ICMP_SLT)
    return B.CreateBinaryIntrinsic(Intrinsic::abs, A, B.getFalse());
  // usub.sat: a < b ? 0 : a - b
  if (match(&S, m_Select(m_ICmp(P, m_Value(A), m_Value(Bv)), m_Zero(),
                         m_Sub(m_Deferred(A), m_Deferred(Bv)))) &&
      P == ICmpInst::ICMP_ULT)
    return B.CreateBinaryIntrinsic(Intrinsic::usub_sat, A, Bv);
  // uadd.sat: a + b < a ? -1 : a + b
  Value *Sum = nullptr;
  if (match(&S, m_Select(m_ICmp(P, m_Value(Sum), m_Value(A)), m_AllOnes(),
                         m_Deferred(Sum))) &&
      P == ICmpInst::ICMP_ULT &&
      match(Sum, m_c_Add(m_Specific(A), m_Value(Bv))))
    return B.CreateBinaryIntrinsic(Intrinsic::uadd_sat, A, Bv);
  // scmp/ucmp: a < b ? -1 : (a > b ? 1 : 0)
  CmpPredicate Q;
  Value *A2 = nullptr, *B2 = nullptr;
  if (match(&S, m_Select(m_ICmp(P, m_Value(A), m_Value(Bv)), m_AllOnes(),
                         m_Select(m_ICmp(Q, m_Value(A2), m_Value(B2)), m_One(),
                                  m_Zero()))) &&
      A == A2 && Bv == B2 && A->getType()->isIntegerTy()) {
    if (P == ICmpInst::ICMP_SLT && Q == ICmpInst::ICMP_SGT)
      return B.CreateIntrinsic(Ty, Intrinsic::scmp, {A, Bv});
    if (P == ICmpInst::ICMP_ULT && Q == ICmpInst::ICMP_UGT)
      return B.CreateIntrinsic(Ty, Intrinsic::ucmp, {A, Bv});
  }
  return nullptr;
}
} // namespace

PreservedAnalyses revenant::RefoldPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasFnAttribute("revenant.o0"))
    return PreservedAnalyses::all();
  SmallVector<WeakTrackingVH, 16> Dead;
  bool Changed = false;
  IRBuilder<> B(F.getContext());
  // Outer selects first (the three-way compare nests one), in program order.
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *S = dyn_cast<SelectInst>(&I);
    if (!S || !S->getParent() || hasOpaque(S))
      continue;
    B.SetInsertPoint(S);
    Value *R = refoldSelect(*S, B);
    if (!R)
      continue;
    if (auto *RI = dyn_cast<Instruction>(R))
      copyProvenance(RI, S);
    R->takeName(S);
    S->replaceAllUsesWith(R);
    for (Value *Op : S->operands())
      Dead.push_back(Op);
    S->eraseFromParent();
    Changed = true;
  }
  RecursivelyDeleteTriviallyDeadInstructionsPermissive(Dead);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
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
