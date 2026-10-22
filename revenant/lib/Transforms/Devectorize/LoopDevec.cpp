//===- LoopDevec.cpp - explode vector reduction loops --------------------===//
//
// Part of the Revenant LLVM fork. See LoopDevec.h.
//
// v1 scope: self-loop single-block headers, integer reduction PHIs sharing
// one opcode/element type, splat-constant inits, 128-bit vectors. The loop
// in `cpp-clang-o2` `sum_vec` (two `<4 x i32>` acc PHIs, vector loads,
// lane-wise adds, exit `llvm.vector.reduce.add`) is the reference shape.
//
// Two-pass structure (no half-mutated loops): validate the whole header
// plus outside uses first, then emit. New values are created with
// PHINode::Create / new LoadInst / BinaryOperator::Create /
// GetElementPtrInst directly into their block; IRBuilder is not used here.
// Float lane zeros use UndefValue: ConstantFP::get on plugin-loaded
// contexts faults the APFloat printer in this build config.
//
//===----------------------------------------------------------------------===//

#include "LoopDevec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

namespace revenant {
namespace loopdevec {

using namespace llvm;

namespace {

constexpr char VA_MD[] = "revenant.va";
constexpr char REGION_MD[] = "revenant.region";
constexpr char OPAQUE_MD[] = "revenant.opaque";

void inheritMeta(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
  if (MDNode *N = Src->getMetadata(REGION_MD))
    Dst->setMetadata(REGION_MD, N);
  if (MDNode *N = Src->getMetadata(OPAQUE_MD))
    Dst->setMetadata(OPAQUE_MD, N);
  Dst->setDebugLoc(Src->getDebugLoc());
}

// Integer fixed-vector gate (reference `int_vec`): any element width, any
// lane count that divides the width. Wide AVX2 accumulators
// (<8 x i32>, rebuilt from i128 halves) fold exactly like the 128-bit ones,
// so no width cap belongs here; the caller compares Lanes/Elem against the
// first acc it found.
bool isIntVec(Type *T, unsigned &Lanes, Type *&Elem) {
  auto *VT = dyn_cast<FixedVectorType>(T);
  if (!VT || !VT->getElementType()->isIntegerTy())
    return false;
  Lanes = VT->getNumElements();
  Elem = VT->getElementType();
  return true;
}

// Flat lane count of T in units of Unit-bit lanes (the accumulator element
// width): an i128 half of an <8 x i32> acc is four lanes, a <4 x i64>
// regrouping of it eight, a <2 x i128> split eight, an i256 rebuild eight.
// Returns 0 when T is not an integer type whose width divides evenly.
unsigned flatLanes(Type *T, unsigned Unit) {
  if (!Unit)
    return 0;
  unsigned Bits = 0;
  if (auto *VT = dyn_cast<FixedVectorType>(T))
    Bits = VT->getPrimitiveSizeInBits();
  else if (auto *IT = dyn_cast<IntegerType>(T))
    Bits = IT->getBitWidth();
  else
    return 0;
  if (Bits % Unit)
    return 0;
  return Bits / Unit;
}

// Reduction opcodes with a lane-wise scalar form and a matching
// llvm.vector.reduce.* intrinsic (reference `red_op`: no sub).
Intrinsic::ID reduceIntrinsic(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Add:
    return Intrinsic::vector_reduce_add;
  case Instruction::Mul:
    return Intrinsic::vector_reduce_mul;
  case Instruction::And:
    return Intrinsic::vector_reduce_and;
  case Instruction::Or:
    return Intrinsic::vector_reduce_or;
  case Instruction::Xor:
    return Intrinsic::vector_reduce_xor;
  default:
    return Intrinsic::not_intrinsic;
  }
}

bool isLaneBinop(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    return true;
  default:
    return false;
  }
}

// One lane of an integer splat-ish constant as an Elem-typed scalar.
// Splat vectors and zeroinitializer fold; per-lane constants of a
// non-splat vector fold per lane. getAggregateElement answers for both
// ConstantVector and ConstantDataVector, which is what ConstantVector::get
// now returns for integer-element vectors, so a non-zero constant init
// (splat or per-lane) still yields its per-lane values.
Value *laneConst(Constant *C, Type *Elem, unsigned Lane) {
  if (isa<UndefValue>(C))
    return UndefValue::get(Elem);
  if (isa<ConstantAggregateZero>(C))
    return ConstantInt::get(Elem, 0);
  if (isa<FixedVectorType>(C->getType())) {
    if (Constant *Splat = C->getSplatValue())
      C = Splat;
    else {
      auto *CI = dyn_cast<ConstantInt>(C->getAggregateElement(Lane));
      if (!CI)
        return nullptr;
      return ConstantInt::get(
          Elem, CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits()));
    }
  }
  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    APInt V = CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits());
    return ConstantInt::get(Elem, V);
  }
  return nullptr;
}

// Splat init shared by every lane (reference `splat_init`).
Value *splatInit(Value *Init, Type *Elem, unsigned Lanes) {
  auto *C = dyn_cast<Constant>(Init);
  if (!C)
    return nullptr;
  Value *L0 = laneConst(C, Elem, 0);
  if (!L0)
    return nullptr;
  for (unsigned j = 1; j < Lanes; ++j) {
    Value *Lj = laneConst(C, Elem, j);
    if (Lj != L0)
      return nullptr;
  }
  return L0;
}

// Per-lane init values for scalar seeding. Constants fold per lane via
// laneConst; an insertelement seed (AVX2 epilogue: scalar in lane 0, zero
// constants elsewhere) yields the scalar for lane 0 and zero constants
// after. Returns false for anything else.
bool initLanes(Value *Init, Type *Elem, unsigned Lanes,
               SmallVector<Value *, 4> &Out) {
  if (auto *C = dyn_cast<Constant>(Init)) {
    for (unsigned j = 0; j < Lanes; ++j) {
      Value *Lj = laneConst(C, Elem, j);
      if (!Lj)
        return false;
      Out.push_back(Lj);
    }
    return true;
  }
  if (auto *IE = dyn_cast<InsertElementInst>(Init)) {
    auto *Idx = dyn_cast<ConstantInt>(IE->getOperand(2));
    if (!Idx || !Idx->isZero())
      return false;
    auto *CV = dyn_cast<Constant>(IE->getOperand(0));
    if (!CV)
      return false;
    // Lanes 1..N must be zero constants (undef accepted as zero; poison
    // bails: it would poison a defined sum).
    for (unsigned j = 1; j < Lanes; ++j) {
      Value *Lj = laneConst(CV, Elem, j);
      if (!isa<UndefValue>(Lj)) {
        auto *CI = dyn_cast<ConstantInt>(Lj);
        if (!CI || !CI->isZero())
          return false;
      }
    }
    Value *Lane0 = IE->getOperand(1);
    if (!Lane0->getType()->isIntegerTy(Elem->getScalarSizeInBits()))
      return false;
    Out.push_back(Lane0);
    for (unsigned j = 1; j < Lanes; ++j)
      Out.push_back(ConstantInt::get(Elem, 0));
    return true;
  }
  return false;
}

struct AccPhi {
  PHINode *Phi = nullptr;
  Value *Init = nullptr;
  BasicBlock *InitBB = nullptr;
  Instruction *Back = nullptr; // update binop in H
  Value *Stream = nullptr; // other operand of Back
};

// Manual horizontal-fold prover. A fold tree reduces accumulator lanes to one
// scalar through lane-wise adds, shuffles, width-changing bitcasts, logical
// shifts by whole lanes, and a final lane extract. Each lane of every tree
// value carries a multiset of root-lane occurrences (root r = acc*Lanes+lane,
// up to 64 roots). Adds sum the multisets; intermediate lanes may
// double-count roots that later shuffles drop (clang's hadd lowering sums
// [a+c b+d c+c d+d] then keeps only lane 1). The fold is valid iff the final
// extracted lane holds every root exactly once. Integer wrapping add is
// associative and commutative, so the final exact-once multiset is the whole
// legality proof for Add.
//
// Lane unit is the accumulator element type, and every tree value is
// flattened into unit-width lanes in bit order: the i128 half of an
// <8 x i32> acc is four lanes, a <4 x i64> regrouping of that acc is eight,
// a <2 x i128> split is eight. A bitcast between widths therefore moves no
// lane boundaries (a pure passthrough), and a shuffle selects whole elements
// of its own granularity inside the flat order.
struct FoldProver {
  const SmallVector<AccPhi, 4> *Accs = nullptr;
  unsigned Lanes = 0;
  unsigned Roots = 0;
  unsigned Unit = 0; // Elem scalar bits: the flat lane unit
  // Memo: flattened lanes*Roots counts per value.
  DenseMap<const Value *, SmallVector<unsigned, 16>> Memo;
  const SmallVector<unsigned, 16> *lanes(const Value *V, unsigned Depth) {
    if (Depth > 32)
      return nullptr;
    auto Hit = Memo.find(V);
    if (Hit != Memo.end())
      return &Hit->second;
    SmallVector<unsigned, 16> M;
    if (!build(const_cast<Value *>(V), M, Depth))
      return nullptr;
    Memo[V] = M;
    return &Memo.find(V)->second;
  }
  bool build(Value *V, SmallVector<unsigned, 16> &M, unsigned Depth) {
    unsigned R = Roots ? Roots : 1;
    auto pushZero = [&]() {
      for (unsigned k = 0; k < R; ++k)
        M.push_back(0);
    };
    auto pushOneHot = [&](unsigned Root) {
      for (unsigned k = 0; k < R; ++k)
        M.push_back(k == Root ? 1 : 0);
    };
    for (unsigned a = 0; Accs && a < Accs->size(); ++a) {
      if (V == (*Accs)[a].Phi || V == (*Accs)[a].Back) {
        for (unsigned j = 0; j < Lanes; ++j)
          pushOneHot(a * Lanes + j);
        return true;
      }
    }
    if (auto *C = dyn_cast<Constant>(V)) {
      unsigned NL = flatLanes(C->getType(), Unit);
      if (!NL)
        return false;
      // A fold tree sums accumulator lanes, so every constant lane must be
      // zero (a nonzero lane has no root to account for).
      bool Zero = isa<UndefValue>(C) || isa<PoisonValue>(C) ||
                  isa<ConstantAggregateZero>(C);
      if (!Zero) {
        auto *CI = dyn_cast<ConstantInt>(C);
        auto *CV = dyn_cast<ConstantVector>(C);
        if (CI) {
          Zero = CI->isZero();
        } else if (CV) {
          Zero = true;
          if (Constant *S = CV->getSplatValue()) {
            auto *SCI = dyn_cast<ConstantInt>(S);
            Zero = SCI && SCI->isZero();
          } else {
            for (Value *Op : CV->operands()) {
              auto *LCI = dyn_cast<ConstantInt>(Op);
              if (!LCI || !LCI->isZero()) {
                Zero = false;
                break;
              }
            }
          }
        }
      }
      if (!Zero)
        return false;
      for (unsigned j = 0; j < NL; ++j)
        pushZero();
      return true;
    }
    if (isa<UndefValue>(V) || isa<PoisonValue>(V)) {
      pushZero();
      return true;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(V)) {
      if (BO->getOpcode() == Instruction::Add) {
        auto *A = lanes(BO->getOperand(0), Depth + 1);
        auto *B = lanes(BO->getOperand(1), Depth + 1);
        if (!A || !B || A->size() != B->size())
          return false;
        for (unsigned j = 0; j < A->size(); ++j)
          M.push_back((*A)[j] + (*B)[j]);
        return true;
      }
      if (BO->getOpcode() == Instruction::LShr) {
        auto *CI = dyn_cast<ConstantInt>(BO->getOperand(1));
        if (!CI || !Unit || R == 0)
          return false;
        uint64_t Shift = CI->getZExtValue();
        // Whole-lane shift: the folded lanes move down, the vacated top
        // lanes read as zero (the shifted-in bits are not root lanes).
        if (Shift % Unit != 0)
          return false;
        unsigned K = (unsigned)(Shift / Unit);
        unsigned L = flatLanes(BO->getType(), Unit);
        if (!L || K >= L)
          return false;
        auto *A = lanes(BO->getOperand(0), Depth + 1);
        if (!A || A->size() != L * R)
          return false;
        for (unsigned j = 0; j < L; ++j)
          for (unsigned k = 0; k < R; ++k)
            M.push_back(j + K < L ? (*A)[(j + K) * R + k] : 0);
        return true;
      }
      return false;
    }
    if (auto *SV = dyn_cast<ShuffleVectorInst>(V)) {
      int NL = (int)SV->getShuffleMask().size();
      auto *A = lanes(SV->getOperand(0), Depth + 1);
      auto *B = lanes(SV->getOperand(1), Depth + 1);
      if (!A || !B || R == 0 || !Unit)
        return false;
      // The mask addresses whole elements; each element is G flat lanes
      // (<4 x i64> over an i32 acc is G=2).
      unsigned G = flatLanes(SV->getType()->getElementType(), Unit);
      unsigned AL = A->size() / R, BL = B->size() / R;
      if (!G || (AL % G) || (BL % G))
        return false;
      AL /= G;
      BL /= G;
      for (int j = 0; j < NL; ++j) {
        int Idx = SV->getShuffleMask()[j];
        if (Idx < 0) {
          for (unsigned g = 0; g < G; ++g)
            pushZero();
          continue;
        }
        unsigned U = (unsigned)Idx;
        const SmallVector<unsigned, 16> *S = nullptr;
        unsigned Base = 0;
        if (U < AL) {
          S = A;
          Base = U * G;
        } else if (U - AL < BL) {
          S = B;
          Base = (U - AL) * G;
        } else {
          return false;
        }
        for (unsigned g = 0; g < G; ++g)
          for (unsigned k = 0; k < R; ++k)
            M.push_back((*S)[(Base + g) * R + k]);
      }
      return true;
    }
    if (auto *BI = dyn_cast<BitCastInst>(V)) {
      // Bit-preserving recast: the flat lane order is unchanged, so the
      // source counts pass through (<8 x i32> to <4 x i64> regroups two
      // lanes per element; i128 halves and <2 x i128> splits likewise).
      unsigned L = flatLanes(BI->getDestTy(), Unit);
      if (!L)
        return false;
      auto *A = lanes(BI->getOperand(0), Depth + 1);
      if (!A || A->size() != L * R)
        return false;
      M.assign(A->begin(), A->end());
      return true;
    }
    if (auto *EI = dyn_cast<ExtractElementInst>(V)) {
      auto *Idx = dyn_cast<ConstantInt>(EI->getIndexOperand());
      if (!Idx || R == 0 || !Unit)
        return false;
      auto *A = lanes(EI->getVectorOperand(), Depth + 1);
      if (!A)
        return false;
      // The extracted element is itself G flat lanes.
      unsigned G = flatLanes(EI->getType(), Unit);
      unsigned AL = A->size() / R;
      if (!G || (AL % G))
        return false;
      if ((Idx->getZExtValue() + 1) * G > AL)
        return false;
      unsigned Base = (unsigned)Idx->getZExtValue() * G;
      for (unsigned g = 0; g < G; ++g)
        for (unsigned k = 0; k < R; ++k)
          M.push_back((*A)[(Base + g) * R + k]);
      return true;
    }
    return false;
  }
};

// Fold-step opcode test for the outside-use walk: lane-wise integer adds,
// vector shuffles, bitcasts, and lane extracts. i128 shifts are
// BinaryOperators (LShr) and pass here; width checking happens in the prover.
bool isFoldStep(Instruction *I) {
  if (isa<ShuffleVectorInst>(I) || isa<BitCastInst>(I) ||
      isa<ExtractElementInst>(I))
    return true;
  if (auto *BO = dyn_cast<BinaryOperator>(I))
    return BO->getOpcode() == Instruction::Add ||
           BO->getOpcode() == Instruction::LShr;
  return false;
}

void eraseIfDead(Instruction *I) {
  if (I && I->getParent() && I->use_empty())
    I->eraseFromParent();
}

// A loop-carried i128 half: the low or high 128 bits of a wide vector.
struct HalfPhi {
  PHINode *Phi = nullptr;
  Value *Init = nullptr;
  BasicBlock *InitBB = nullptr;
  Value *Back = nullptr;
};

// Vector image of two half-PHI inits. Constant halves fold to the matching
// vector constant (the common all-zero seed); anything else rebuilds the
// chain in the init block, where it stays loop-invariant.
Value *halfPairInit(const HalfPhi &Lo, const HalfPhi &Hi, FixedVectorType *VT) {
  auto *LoC = dyn_cast<ConstantInt>(Lo.Init);
  auto *HiC = dyn_cast<ConstantInt>(Hi.Init);
  BasicBlock *InitBB = Lo.InitBB;
  LLVMContext &Ctx = InitBB->getContext();
  if (LoC && HiC) {
    unsigned EB = VT->getElementType()->getScalarSizeInBits();
    APInt All(256, 0);
    All |= LoC->getValue().zext(256);
    All |= HiC->getValue().zext(256).shl(128);
    SmallVector<Constant *, 8> Ls;
    bool Zero = true, Splat = true;
    for (unsigned j = 0; j < VT->getNumElements(); ++j) {
      auto *LC = ConstantInt::get(VT->getElementType(),
                                 All.extractBits(EB, j * EB));
      Ls.push_back(LC);
      if (!LC->isZeroValue())
        Zero = false;
      if (LC != Ls.front())
        Splat = false;
    }
    if (Zero)
      return ConstantAggregateZero::get(VT);
    if (Splat)
      return ConstantVector::getSplat(VT->getElementCount(), Ls.front());
    return ConstantVector::get(Ls);
  }
  auto *ZL = new ZExtInst(Lo.Init, Type::getIntNTy(Ctx, 256), "v.unopt.z",
                          InitBB->getTerminator());
  auto *ZH = new ZExtInst(Hi.Init, Type::getIntNTy(Ctx, 256), "v.unopt.z",
                          InitBB->getTerminator());
  auto *S = BinaryOperator::CreateShl(
      ZH, ConstantInt::get(Type::getIntNTy(Ctx, 256), 128), "v.unopt.h",
      InitBB->getTerminator());
  auto *O = BinaryOperator::CreateOr(S, ZL, "v.unopt.o",
                                     InitBB->getTerminator());
  return new BitCastInst(O, VT, "v.unopt", InitBB->getTerminator());
}

// Promote one low/high half pair to the wide vector it rebuilds. Matches
// the AVX2 carrying pattern:
//   %lo = phi i128 [ %lo.init, %pre ], [ %e0, %H ]
//   %hi = phi i128 [ %hi.init, %pre ], [ %e1, %H ]
//   %zlo = zext i128 %lo to i256
//   %zhi = zext i128 %hi to i256
//   %sh = shl i256 %zhi, 128
//   %or = or i256 %sh, %zlo
//   %bc = bitcast i256 %or to <N x iE>
//   %upd = add <N x iE> %bc, %stream
//   %split = bitcast <N x iE> %upd to <2 x i128>
//   %e0 = extractelement <2 x i128> %split, 0
//   %e1 = extractelement <2 x i128> %split, 1
// Every value in the chain has exactly one user and the round trip is
// bit-preserving, so carrying <N x iE> directly is equivalent. Returns
// false (leaving the IR untouched) on any deviation.
bool promoteHalfPair(BasicBlock *H, const HalfPhi &Lo, const HalfPhi &Hi) {
  auto *ELo = dyn_cast<ExtractElementInst>(Lo.Back);
  auto *EHi = dyn_cast<ExtractElementInst>(Hi.Back);
  if (!ELo || !EHi || ELo->getVectorOperand() != EHi->getVectorOperand())
    return false;
  auto *ILo = dyn_cast<ConstantInt>(ELo->getIndexOperand());
  auto *IHi = dyn_cast<ConstantInt>(EHi->getIndexOperand());
  if (!ILo || !IHi || !ILo->isZero() || !IHi->isOne())
    return false;
  auto *BS = dyn_cast<BitCastInst>(ELo->getVectorOperand());
  if (!BS || BS->getParent() != H)
    return false;
  auto *SplitTy = dyn_cast<FixedVectorType>(BS->getType());
  if (!SplitTy || SplitTy->getNumElements() != 2 ||
      !SplitTy->getElementType()->isIntegerTy(128))
    return false;
  auto *VT = dyn_cast<FixedVectorType>(BS->getSrcTy());
  if (!VT || !VT->getElementType()->isIntegerTy() ||
      VT->getPrimitiveSizeInBits() != 256)
    return false;
  auto *Add = dyn_cast<BinaryOperator>(BS->getOperand(0));
  if (!Add || Add->getParent() != H || Add->getOpcode() != Instruction::Add ||
      Add->getType() != VT)
    return false;
  Value *Rebuild = nullptr;
  for (unsigned k = 0; k < 2; ++k)
    if (isa<BitCastInst>(Add->getOperand(k)))
      Rebuild = Add->getOperand(k);
  if (!Rebuild)
    return false;
  auto *B = cast<BitCastInst>(Rebuild);
  if (B->getParent() != H || B->getDestTy() != VT ||
      !B->getSrcTy()->isIntegerTy(256) || !B->hasOneUse())
    return false;
  auto *O = dyn_cast<BinaryOperator>(B->getOperand(0));
  if (!O || O->getParent() != H || O->getOpcode() != Instruction::Or ||
      !O->getType()->isIntegerTy(256) || !O->hasOneUse())
    return false;
  BinaryOperator *Sh = nullptr;
  ZExtInst *ZLo = nullptr;
  for (unsigned k = 0; k < 2; ++k) {
    auto *Op0 = dyn_cast<BinaryOperator>(O->getOperand(k));
    auto *Op1 = dyn_cast<ZExtInst>(O->getOperand(1 - k));
    if (!Op0 || !Op1)
      continue;
    auto *Amt = dyn_cast<ConstantInt>(Op0->getOperand(1));
    if (Op0->getParent() == H && Op0->getOpcode() == Instruction::Shl && Amt &&
        Amt->getZExtValue() == 128 && Op0->hasOneUse() &&
        Op1->getType()->isIntegerTy(256)) {
      Sh = Op0;
      ZLo = Op1;
    }
  }
  if (!Sh || !ZLo)
    return false;
  auto *ZHi = dyn_cast<ZExtInst>(Sh->getOperand(0));
  if (!ZHi || ZHi->getParent() != H || !ZHi->hasOneUse() ||
      !ZHi->getType()->isIntegerTy(256) || !ZLo->hasOneUse())
    return false;
  if (ZLo->getOperand(0) != Lo.Phi || ZHi->getOperand(0) != Hi.Phi)
    return false;
  if (!Lo.Phi->hasOneUse() || !Hi.Phi->hasOneUse())
    return false;
  // The split extracts may have other users outside the header (the exit
  // fold reads the low half directly in the single-acc shape); they survive
  // the rewrite through eraseIfDead below.
  // Carry the vector directly; the half phis and their rebuild die here.
  auto *VP = PHINode::Create(VT, 2, "v.unopt", H->begin());
  VP->addIncoming(halfPairInit(Lo, Hi, VT), Lo.InitBB);
  VP->addIncoming(Add, H);
  inheritMeta(Lo.Phi, VP);
  for (unsigned k = 0; k < 2; ++k)
    if (Add->getOperand(k) == B)
      Add->setOperand(k, VP);
  eraseIfDead(B);
  eraseIfDead(O);
  eraseIfDead(Sh);
  eraseIfDead(ZLo);
  eraseIfDead(ZHi);
  eraseIfDead(Lo.Phi);
  eraseIfDead(Hi.Phi);
  eraseIfDead(ELo);
  eraseIfDead(EHi);
  eraseIfDead(BS);
  return true;
}

// Promote every i128 half pair in a self-loop block. Runs before the
// reduction scan so a wide accumulator shows up as one vector PHI.
unsigned tryPromoteHalfVectors(BasicBlock *H) {
  SmallVector<HalfPhi, 4> Halves;
  for (PHINode &P : H->phis()) {
    if (!P.getType()->isIntegerTy(128) || P.getNumIncomingValues() != 2)
      continue;
    HalfPhi Hl;
    Hl.Phi = &P;
    for (unsigned i = 0; i < 2; ++i) {
      if (P.getIncomingBlock(i) == H) {
        Hl.Back = P.getIncomingValue(i);
      } else {
        Hl.Init = P.getIncomingValue(i);
        Hl.InitBB = P.getIncomingBlock(i);
      }
    }
    if (Hl.Back && Hl.Init && Hl.InitBB)
      Halves.push_back(Hl);
  }
  unsigned Promoted = 0;
  // promoteHalfPair erases consumed half phis (eraseFromParent frees them),
  // so consumed pairs are tracked here instead of testing the phi's parent.
  SmallVector<bool, 4> Consumed(Halves.size(), false);
  for (unsigned i = 0; i < Halves.size(); ++i) {
    if (Consumed[i])
      continue; // consumed by an earlier pair
    for (unsigned j = i + 1; j < Halves.size(); ++j) {
      if (Consumed[j] || Halves[j].InitBB != Halves[i].InitBB)
        continue;
      // The low half is the phi whose backedge value is extract 0.
      if (promoteHalfPair(H, Halves[i], Halves[j]) ||
          promoteHalfPair(H, Halves[j], Halves[i])) {
        Consumed[i] = true;
        Consumed[j] = true;
        ++Promoted;
        break;
      }
    }
  }
  return Promoted;
}

} // namespace

static unsigned tryLoop(Function &F, BasicBlock *H);

unsigned devectorizeLoops(Function &F) {
  unsigned Changed = 0;
  SmallVector<BasicBlock *, 16> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);
  // Widen i128 half-carried accumulators first: the rebuild/split round trip
  // is exact, so the reduction scan below sees one vector PHI per acc.
  for (BasicBlock *H : Blocks)
    Changed += tryPromoteHalfVectors(H);
  for (BasicBlock *H : Blocks)
    Changed += tryLoop(F, H);
  return Changed;
}

static unsigned tryLoop(Function &F, BasicBlock *H) {
  // v1: self-loop single-block header only.
  auto *Br = dyn_cast<BranchInst>(H->getTerminator());
  if (!Br || !Br->isConditional())
    return 0;
  bool Self = false;
  for (BasicBlock *S : Br->successors())
    if (S == H)
      Self = true;
  if (!Self)
    return 0;

  // Classify vector PHIs.
  SmallVector<AccPhi, 4> Accs;
  for (PHINode &P : H->phis()) {
    unsigned Lanes = 0;
    Type *Elem = nullptr;
    if (!isIntVec(P.getType(), Lanes, Elem))
      continue;
    if (P.getNumIncomingValues() != 2)
      return 0;
    AccPhi A;
    A.Phi = &P;
    for (unsigned i = 0; i < 2; ++i) {
      if (P.getIncomingBlock(i) == H)
        A.Back = dyn_cast<Instruction>(P.getIncomingValue(i));
      else {
        A.Init = P.getIncomingValue(i);
        A.InitBB = P.getIncomingBlock(i);
      }
    }
    if (!A.Back || !A.Init || !A.InitBB)
      return 0;
    if (A.Back->getParent() != H)
      return 0;
    auto *BO = dyn_cast<BinaryOperator>(A.Back);
    if (!BO || reduceIntrinsic(BO->getOpcode()) == Intrinsic::not_intrinsic ||
        BO->getType() != P.getType())
      return 0;
    if (BO->getOperand(0) == &P)
      A.Stream = BO->getOperand(1);
    else if (BO->getOperand(1) == &P)
      A.Stream = BO->getOperand(0);
    else
      return 0;
    Accs.push_back(A);
  }
  if (Accs.empty())
    return 0;

  // One shared (opcode, lanes, elem type); one shared init block.
  unsigned Opcode = cast<BinaryOperator>(Accs[0].Back)->getOpcode();
  unsigned Lanes = 0;
  Type *Elem = nullptr;
  {
    unsigned L = 0;
    Type *E = nullptr;
    if (!isIntVec(Accs[0].Phi->getType(), L, E))
      return 0;
    Lanes = L;
    Elem = E;
  }
  BasicBlock *InitBB = Accs[0].InitBB;
  for (auto &A : Accs) {
    auto *BO = cast<BinaryOperator>(A.Back);
    unsigned L = 0;
    Type *E = nullptr;
    if (BO->getOpcode() != Opcode || A.InitBB != InitBB ||
        !isIntVec(A.Phi->getType(), L, E) || L != Lanes || E != Elem)
      return 0;
  }

  // Per-acc init check: every acc init must yield per-lane values (constants
  // via laneConst, or an insertelement scalar seed with zero rest for AVX2
  // epilogue loops). The emit stage seeds each scalar phi with the LANE SUM
  // of its own acc's init lanes (for zero seeds identical to the old lane-0
  // seed; for [seed,0,0,0] epilogues the seed itself). Lane-varying
  // induction bail: a vector PHI stepping by a splat constant from a
  // ramp-constant init is an index vector, not a reduction accumulator, so
  // the whole loop bails rather than threading it through scalar chains.
  for (auto &A : Accs) {
    SmallVector<Value *, 4> Lanes0;
    if (initLanes(A.Init, Elem, Lanes, Lanes0))
      continue;
    auto *InitC = dyn_cast<Constant>(A.Init);
    auto *StreamC = dyn_cast<Constant>(A.Stream);
    if (InitC && StreamC && StreamC->getSplatValue())
      return 0; // induction PHI: not a pure reduction
    return 0; // unknown init form: bail
  }
  // Outside-use precheck: every outside use of an acc phi, backedge, or
  // other vector def in H must feed a matching reduce (directly or
  // through one lane-wise add combining acc backs).
  DenseSet<const Value *> AccVals;
  for (auto &A : Accs) {
    AccVals.insert(A.Phi);
    AccVals.insert(A.Back);
  }
  DenseSet<const Value *> VecDefs;
  for (Instruction &I : *H)
    if (isa<FixedVectorType>(I.getType()))
      VecDefs.insert(&I);
  SmallVector<Instruction *, 4> Reduces;
  // Exit-block combine: the two acc backs merge through one lane-wise add
  // before the reduce (reference LoopCx merges acc next-vals the same way).
  SmallVector<BinaryOperator *, 2> Combines;
  std::function<bool(Value *, unsigned)> AccTree = [&](Value *V,
                                                       unsigned Depth) {
    if (AccVals.contains(V))
      return true;
    if (Depth >= 1)
      return false;
    auto *BO = dyn_cast<BinaryOperator>(V);
    if (!BO || BO->getOpcode() != Instruction::Add || BO->getParent() == H)
      return false;
    unsigned L = 0;
    Type *E = nullptr;
    if (!isIntVec(BO->getType(), L, E) || L != Lanes || E != Elem)
      return false;
    return AccTree(BO->getOperand(0), Depth + 1) &&
           AccTree(BO->getOperand(1), Depth + 1);
  };
  for (const Value *V : VecDefs) {
    for (const User *U : V->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() == H)
        continue;
      auto *CI = dyn_cast<CallInst>(const_cast<Instruction *>(UI));
      auto *II = CI ? dyn_cast<IntrinsicInst>(CI) : nullptr;
      if (II && II->getIntrinsicID() == reduceIntrinsic(Opcode) &&
          AccTree(II->getOperand(0), 0)) {
        Reduces.push_back(CI);
        continue;
      }
      // One lane-wise combine of acc backs in the exit block is part of the
      // reduction (merged_val in the reference); record it for erasure after
      // aliasing, still no other outside uses allowed. The combine opcode
      // must equal the reduction opcode: reduce distributes over a lane-wise
      // combine only when both apply the same operation (add over add); a
      // mul-reduction over an add-combine has no chain-end form.
      auto *BO = dyn_cast<BinaryOperator>(const_cast<Instruction *>(UI));
      unsigned CL = 0;
      Type *CE = nullptr;
      if (BO && BO->getOpcode() == Opcode &&
          isIntVec(BO->getType(), CL, CE) && CL == Lanes && CE == Elem &&
          AccTree(BO->getOperand(0), 0) && AccTree(BO->getOperand(1), 0)) {
        Combines.push_back(BO);
        continue;
      }
      // Fold-step candidate: lane-wise adds (other than the combine), vector
      // shuffles, same-width bitcasts, whole-lane i128 shifts, and lane
      // extracts outside H may belong to a manual horizontal-fold tree. They
      // are proven (or rejected) by the fold discovery below; anything else
      // bails here as before.
      if (isFoldStep(const_cast<Instruction *>(UI)))
        continue;
      return 0;
    }
  }
  // Reduces over an exit-block combine (only when a reduce intrinsic exists:
  // with no reduces the combines feed manual fold trees, proven below). The
  // combine lives outside H so the loop above never visits its users. Every
  // combine must feed at least one matching reduce (else the scalar chains
  // would not cover its value), and no other outside use is allowed.
  // (devectorize-mixed-reduce-roots.ll: the second reduce over acc+acc was
  // missed here, left on the severed vector PHI, and returned 16 instead
  // of 24.)
  DenseSet<const Instruction *> SeenReduce;
  for (Instruction *R : Reduces)
    SeenReduce.insert(R);
  if (!Reduces.empty())
  for (BinaryOperator *C : Combines) {
    bool FedReduce = false;
    for (User *U : C->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || UI->getParent() == H)
        return 0;
      auto *CI = dyn_cast<CallInst>(UI);
      auto *II = CI ? dyn_cast<IntrinsicInst>(CI) : nullptr;
      if (II && II->getIntrinsicID() == reduceIntrinsic(Opcode) &&
          AccTree(II->getOperand(0), 0)) {
        if (SeenReduce.insert(CI).second)
          Reduces.push_back(CI);
        FedReduce = true;
        continue;
      }
      return 0;
    }
    if (!FedReduce)
      return 0;
  }
  // Manual-fold exit discovery (no reduce intrinsic): when Reduces is empty
  // and the opcode is Add, each exit-block combine may instead feed a fold
  // tree (lane-wise adds, shuffles, same-width bitcasts, whole-lane i128
  // shifts, final lane extract) that the prover shows sums every root lane
  // exactly once. Fold-tree values and the extract are recorded for erasure
  // after scalar aliasing. The extract may feed scalar tail-loop PHIs or a
  struct FoldExit {
    ExtractElementInst *Extract = nullptr;
    SmallVector<Instruction *, 16> TreeVals;
    unsigned Roots = 0;
  };
  SmallVector<FoldExit, 2> Folds;
  // Fold-tree values, in scope for the header prechecks below: the
  // discovered trees (H-resident splits included) are not body values.
  DenseSet<const Value *> FoldSeen;
  // Fold roots: the exit-block combines when present, else the acc backedges
  // directly (single-accumulator i128 folds have no combine).
  SmallVector<Value *, 4> FoldRoots;
  for (auto *C : Combines)
    FoldRoots.push_back(C);
  if (FoldRoots.empty())
    for (auto &A : Accs)
      FoldRoots.push_back(A.Back);
  if (Reduces.empty() && Opcode == Instruction::Add && !FoldRoots.empty()) {
    FoldProver FP;
    FP.Accs = &Accs;
    FP.Lanes = Lanes;
    FP.Unit = Elem->getScalarSizeInBits();
    FP.Roots = Accs.size() * Lanes;
    SmallVector<ExtractElementInst *, 4> Exts;
    DenseSet<const Value *> SeenExt;
    for (Value *FR : FoldRoots) {
      SmallVector<Value *, 8> Work;
      DenseSet<const Value *> Seen;
      Work.push_back(FR);
      Seen.insert(FR);
      for (unsigned wi = 0; wi < Work.size() && wi < 64; ++wi) {
        for (User *U : Work[wi]->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          // A fold step may live in H: the backedge split (bitcast to
          // <N x i128> plus extractelement) sits in the header and feeds the
          // exit fold directly in the single-acc i128-half shape.
          if (!UI || !isFoldStep(UI))
            continue;
          if (auto *EI = dyn_cast<ExtractElementInst>(UI)) {
            if (SeenExt.insert(EI).second)
              Exts.push_back(EI);
            continue;
          }
          if (Seen.insert(UI).second)
            Work.push_back(UI);
        }
      }
    }
    auto ExactOnce = [&](const SmallVector<unsigned, 16> *M) {
      if (!M || M->size() != FP.Roots)
        return false;
      for (unsigned c : *M)
        if (c != 1)
          return false;
      return true;
    };
    for (ExtractElementInst *Ext : Exts) {
      if (!ExactOnce(FP.lanes(Ext, 0)))
        continue;
      // Backward slice through fold steps to the acc roots.
      SmallVector<Instruction *, 16> Slice;
      DenseSet<const Value *> SSeen;
      SmallVector<Value *, 8> Work;
      Work.push_back(Ext);
      SSeen.insert(Ext);
      bool Bad = false;
      for (unsigned wi = 0; wi < Work.size() && wi < 64 && !Bad; ++wi) {
        auto *I = cast<Instruction>(Work[wi]);
        Slice.push_back(I);
        for (Value *Op : I->operands()) {
          if (isa<Constant>(Op) || isa<UndefValue>(Op))
            continue;
          bool IsRoot = false;
          for (unsigned a = 0; a < Accs.size(); ++a)
            if (Op == Accs[a].Phi || Op == Accs[a].Back)
              IsRoot = true;
          if (IsRoot)
            continue;
          auto *OI = dyn_cast<Instruction>(Op);
          // H-resident fold steps are the backedge split of the same acc
          // (bitcast to <N x i128> plus extractelement): part of the tree,
          // proven and erased with it.
          if (!OI || !isFoldStep(OI)) {
            Bad = true;
            break;
          }
          if (SSeen.insert(OI).second)
            Work.push_back(OI);
        }
      }
      if (Bad)
        continue;
      FoldExit FE;
      FE.Extract = Ext;
      FE.Roots = FP.Roots;
      FE.TreeVals = Slice;
      for (auto *I : FE.TreeVals)
        FoldSeen.insert(I);
      Folds.push_back(FE);
    }
    // Every combine must be consumed by a proven fold; every fold value's
    // users must stay inside the folds except the extracts, which feed
    // scalar tail PHIs or a scalar return.
    for (BinaryOperator *C : Combines) {
      if (!FoldSeen.contains(C))
        return 0;
    }
    for (auto &FE : Folds) {
      for (Instruction *I : FE.TreeVals) {
        if (I == FE.Extract)
          continue;
        for (User *U : I->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || !FoldSeen.contains(UI))
            return 0;
        }
      }
      for (User *U : FE.Extract->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI)
          return 0;
        if (isa<PHINode>(UI) || isa<ReturnInst>(UI))
          continue;
        return 0;
      }
    }
    if (Folds.empty())
      return 0;
  }
  // exit consumers on severed vector PHIs (loop-manualfold-stays.ll).
  if (Reduces.empty() && Folds.empty())
    return 0;
  for (BasicBlock *S : Br->successors()) {
    if (S == H)
      continue;
    for (PHINode &P : S->phis()) {
      if (!isa<FixedVectorType>(P.getType()))
        continue;
      for (Value *Inc : P.incoming_values())
        if (VecDefs.contains(Inc))
          return 0;
    }
  }

  // Header body precheck (no mutation yet). Fold-tree values living in H
  // (the backedge split) are proven above and die with the tree, so they are
  // not body values to explode.
  DenseSet<const Instruction *> AccBacks;
  for (auto &A : Accs)
    AccBacks.insert(A.Back);
  for (Instruction &I : *H) {
    if (FoldSeen.contains(&I))
      continue;
    if (isa<PHINode>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (isIntVec(I.getType(), L, E)) {
        bool IsAcc = false;
        for (auto &A : Accs)
          if (A.Phi == &I)
            IsAcc = true;
        if (!IsAcc)
          return 0;
      }
      continue;
    }
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!isIntVec(LI->getType(), L, E) || L != Lanes || E != Elem)
        return 0;
      if (LI->isVolatile() || !LI->isSimple())
        return 0;
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (isa<FixedVectorType>(SI->getValueOperand()->getType()))
        return 0;
      continue;
    }
    if (AccBacks.contains(&I))
      continue;
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!isIntVec(BO->getType(), L, E))
        continue; // scalar body op
      if (L != Lanes || E != Elem || !isLaneBinop(BO->getOpcode()))
        return 0;
      continue;
    }
    if (auto *BI = dyn_cast<BitCastInst>(&I)) {
      if (isa<FixedVectorType>(BI->getSrcTy())) {
        if (BI->getDestTy() != BI->getSrcTy())
          return 0;
        continue;
      }
      continue;
    }
    if (auto *EI = dyn_cast<ExtractElementInst>(&I)) {
      auto *Idx = dyn_cast<ConstantInt>(EI->getIndexOperand());
      if (!Idx || Idx->getZExtValue() >= Lanes)
        return 0;
      // v1: only lane-0 extracts of acc phis (partial sums stay vector).
      bool OfAcc = false;
      for (auto &A : Accs)
        if (EI->getVectorOperand() == A.Phi && Idx->isZero())
          OfAcc = true;
      if (!OfAcc)
        return 0;
      continue;
    }
    if (isa<FixedVectorType>(I.getType()))
      return 0;
    // Kept scalar instruction: vector operands must be pass-C consumers
    // of acc phis (handled below) or none at all.
    for (Value *Op : I.operands()) {
      if (!isa<FixedVectorType>(Op->getType()))
        continue;
      auto *OI = dyn_cast<Instruction>(Op);
      bool Consumer = isa<ExtractElementInst>(&I) || isa<TruncInst>(&I) ||
                      isa<BitCastInst>(&I);
      if (!Consumer || !OI)
        return 0;
      bool OfAcc = false;
      for (auto &A : Accs)
        if (Op == A.Phi)
          OfAcc = true;
      if (!OfAcc)
        return 0;
    }
  }
  // Pre-mutation explodability check (multi-acc-dependency.ll): Ex below
  // has no PHI case, so any stream whose lane tree touches an accumulator
  // PHI/back would fail AFTER scalar PHIs exist and old edges are severed
  // (verifier abort on half-built s.unopt PHIs). Prove everything
  // explodable here, before creating any IR.
  std::function<bool(Value *, unsigned)> CanExplode =
      [&](Value *V, unsigned Depth) -> bool {
    if (Depth > 32)
      return false;
    if (AccVals.contains(V))
      return false;
    if (auto *C = dyn_cast<Constant>(V)) {
      for (unsigned j = 0; j < Lanes; ++j)
        if (!laneConst(C, Elem, j))
          return false;
      return true;
    }
    if (isa<UndefValue>(V))
      return true;
    if (auto *LI = dyn_cast<LoadInst>(V))
      return LI->getParent() == H;
    if (auto *BO = dyn_cast<BinaryOperator>(V)) {
      if (BO->getParent() != H || !isLaneBinop(BO->getOpcode()))
        return false;
      return CanExplode(BO->getOperand(0), Depth + 1) &&
             CanExplode(BO->getOperand(1), Depth + 1);
    }
    if (auto *BI = dyn_cast<BitCastInst>(V)) {
      if (BI->getDestTy() != BI->getSrcTy())
        return false;
      return CanExplode(BI->getOperand(0), Depth + 1);
    }
    return false;
  };
  for (auto &A : Accs)
    if (!CanExplode(A.Stream, 0))
      return 0;
  for (Instruction &I : *H) {
    if (AccBacks.contains(&I) || isa<PHINode>(&I) || FoldSeen.contains(&I))
      continue;
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (isa<FixedVectorType>(LI->getType()) && !CanExplode(LI, 0))
        return 0;
      continue;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (isa<FixedVectorType>(BO->getType()) && !CanExplode(BO, 0))
        return 0;
      continue;
    }
    if (auto *BI = dyn_cast<BitCastInst>(&I)) {
      if (isa<FixedVectorType>(BI->getDestTy()) &&
          !CanExplode(BI->getOperand(0), 0))
        return 0;
      continue;
    }
  }
  // Reduce-root shape check before mutation: each reduce aliases to one
  // acc chain end, or to one lane-wise exit-block add of two acc chain
  // ends. AccTree above already proved this shape; re-check the exact emit
  // cases so the defensive bail below never fires post-mutation.
  for (Instruction *R : Reduces) {
    Value *Root = cast<IntrinsicInst>(R)->getOperand(0);
    auto IsAcc = [&](Value *V) {
      for (auto &A : Accs)
        if (V == A.Phi || V == A.Back)
          return true;
      return false;
    };
    if (IsAcc(Root))
      continue;
    auto *BO = dyn_cast<BinaryOperator>(Root);
    if (!BO || BO->getOpcode() != Instruction::Add ||
        !isa<FixedVectorType>(BO->getType()) || BO->getParent() == H ||
        !IsAcc(BO->getOperand(0)) || !IsAcc(BO->getOperand(1)))
      return 0;
  }

  // Emit: one scalar acc phi per vector acc phi (init edge only; backedges
  // close below). Each vector reduction keeps its own accumulator: sharing
  // one chain across independent reductions would merge their sums and any
  // outer add of two reduces would double the total (multi-reduce.ll).
  LLVMContext &Ctx = F.getContext();
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  unsigned Esz = Elem->getScalarSizeInBits() / 8;
  SmallVector<Value *, 4> Inits;
  for (auto &A : Accs) {
    // Seed = lane sum of the init lanes: the scalar image of the vector
    // reduce of the entry value. Zero seeds (all observed cases) behave as
    // before; [seed,0,0,0] epilogue inits seed with the scalar itself.
    SmallVector<Value *, 4> IL;
    if (!initLanes(A.Init, Elem, Lanes, IL))
      return 0; // pre-mutation: no scalar PHI created yet, nothing to clean
    Value *Seed = IL[0];
    for (unsigned j = 1; j < Lanes; ++j) {
      // Zero lanes are neutral: skip them so zero seeds emit no seed
      // arithmetic (existing fixtures keep byte-identical output). Seed
      // arithmetic, when needed, goes before the InitBB terminator: it is
      // loop-invariant.
      if (auto *CI = dyn_cast<ConstantInt>(IL[j])) {
        if (CI->isZero())
          continue;
      } else if (isa<UndefValue>(IL[j])) {
        continue;
      }
      auto *N = BinaryOperator::Create((Instruction::BinaryOps)Opcode, Seed,
                                       IL[j], "s.unopt.seed",
                                       InitBB->getTerminator());
      inheritMeta(A.Phi, N);
      Seed = N;
    }
    Inits.push_back(Seed);
  }
  SmallVector<PHINode *, 4> SS;
  for (unsigned a = 0; a < Accs.size(); ++a) {
    PHINode *S = PHINode::Create(Elem, 2, "s.unopt", H->begin());
    S->addIncoming(Inits[a], InitBB);
    inheritMeta(Accs[a].Phi, S);
    SS.push_back(S);
  }

  DenseMap<const Value *, Value *> Subs;
  DenseMap<const Value *, SmallVector<Value *, 4>> LaneMemo;
  SmallVector<Instruction *, 16> Kill;
  auto Resolve = [&](Value *V) {
    unsigned Steps = 0;
    while (Value *N = Subs.lookup(V)) {
      V = N;
      if (++Steps > 32)
        break;
    }
    return V;
  };
  std::function<SmallVector<Value *, 4> *(Value *)> Explode = nullptr;
  SmallVector<Value *, 4> Empty;
  // Recursive explosion via explicit memo lookup (no std::function
  // re-entrancy hazards: depth is bounded by the def chain).
  std::function<SmallVector<Value *, 4> *(Value *)> Ex =
      [&](Value *V) -> SmallVector<Value *, 4> * {
    auto Hit = LaneMemo.find(V);
    if (Hit != LaneMemo.end())
      return &Hit->second;
    SmallVector<Value *, 4> Ls;
    bool Ok = false;
    if (auto *C = dyn_cast<Constant>(V)) {
      Ok = true;
      for (unsigned j = 0; j < Lanes; ++j) {
        Value *Lj = laneConst(C, Elem, j);
        if (!Lj) {
          Ok = false;
          break;
        }
        Ls.push_back(Lj);
      }
    } else if (isa<UndefValue>(V)) {
      Ok = true;
      for (unsigned j = 0; j < Lanes; ++j)
        Ls.push_back(UndefValue::get(Elem));
    } else if (auto *LI = dyn_cast<LoadInst>(V)) {
      if (LI->getParent() == H) {
        Ok = true;
        MaybeAlign BaseAlign = LI->getAlign();
        for (unsigned j = 0; j < Lanes && Ok; ++j) {
          auto *Off = ConstantInt::get(I64, (uint64_t)(j * Esz));
          auto *G = GetElementPtrInst::Create(
              I8, const_cast<Value *>(LI->getPointerOperand()), Off,
              LI->getName() + ".p", LI->getIterator());
          inheritMeta(LI, G);
          auto *L = new LoadInst(Elem, G, LI->getName() + ".l",
                                 LI->isVolatile(), LI->getIterator());
          inheritMeta(LI, L);
          // Lane alignment = MinAlign(base, lane offset): lane 0 keeps the
          // vector's alignment (fault-symmetric with the original on
          // under-aligned inputs), later lanes only what their offset
          // proves. Dropping alignment entirely would erase UB the source
          // program promised and diverge fault behavior (fuzz trial 1).
          if (BaseAlign)
            L->setAlignment(
                commonAlignment(*BaseAlign, (uint64_t)j * (uint64_t)Esz));
          Ls.push_back(L);
        }
      }
    } else if (auto *BO = dyn_cast<BinaryOperator>(V)) {
      if (BO->getParent() == H && isLaneBinop(BO->getOpcode())) {
        auto *A = Ex(BO->getOperand(0));
        auto *B = Ex(BO->getOperand(1));
        if (A && B && A->size() == Lanes && B->size() == Lanes) {
          Ok = true;
          for (unsigned j = 0; j < Lanes; ++j) {
            auto *N = BinaryOperator::Create(
                BO->getOpcode(), (*A)[j], (*B)[j], BO->getName() + ".s",
                BO->getIterator());
            inheritMeta(BO, N);
            Ls.push_back(N);
          }
        }
      }
    } else if (auto *BI = dyn_cast<BitCastInst>(V)) {
      if (BI->getDestTy() == BI->getSrcTy()) {
        auto *A = Ex(BI->getOperand(0));
        if (A && A->size() == Lanes) {
          Ls = *A;
          Ok = true;
        }
      }
    }
    if (!Ok)
      return nullptr;
    LaneMemo[V] = Ls;
    return &LaneMemo.find(V)->second;
  };
  (void)Explode;
  (void)Empty;

  // Subs for acc phis (own scalar phi each) and valid lane-0 acc extracts.
  for (unsigned a = 0; a < Accs.size(); ++a)
    Subs[Accs[a].Phi] = SS[a];
  for (Instruction &I : *H) {
    auto *EI = dyn_cast<ExtractElementInst>(&I);
    if (!EI)
      continue;
    for (unsigned a = 0; a < Accs.size(); ++a)
      if (EI->getVectorOperand() == Accs[a].Phi) {
        Subs[&I] = SS[a];
        Kill.push_back(&I);
      }
  }

  // Walk in order: explode streams, extend each acc's own scalar chain at
  // its update. Chain operations append before the update instruction so
  // intra-block SSA order holds.
  SmallVector<Value *, 4> Chains;
  for (auto *S : SS)
    Chains.push_back(S);
  SmallVector<Instruction *, 16> Walk;
  for (Instruction &I : *H)
    Walk.push_back(&I);
  for (Instruction *IP : Walk) {
    Instruction &I = *IP;
    if (isa<PHINode>(&I))
      continue;
    // Proven fold-tree values in H (the backedge split) die with the tree
    // after the extract aliases to the scalar sum; exploding them would
    // keep dead vector ops in the loop.
    if (FoldSeen.contains(&I))
      continue;
    if (AccBacks.contains(&I)) {
      // Acc update: explode the stream side FIRST, fold lanes into that
      // acc's own chain. The pre-mutation check above proved this explode
      // succeeds; even so, sever the old phi edge only after the explode
      // so a surprise failure leaves no half-built state for this update.
      // (Severing first is what stranded half-built s.unopt PHIs and
      // aborted the verifier on multi-acc-dependency.ll.)
      unsigned Idx = 0;
      for (unsigned a = 0; a < Accs.size(); ++a)
        if (Accs[a].Back == &I)
          Idx = a;
      Value *Stream = Accs[Idx].Stream;
      auto *Ls = Ex(Resolve(Stream));
      if (!Ls)
        return 0; // unreachable: pre-mutation CanExplode proved success
      for (unsigned k = 0, KN = Accs[Idx].Phi->getNumIncomingValues();
           k < KN; ++k)
        if (Accs[Idx].Phi->getIncomingBlock(k) == H)
          Accs[Idx].Phi->setIncomingValue(
              k, llvm::UndefValue::get(Accs[Idx].Phi->getType()));
      Value *Chain = Chains[Idx];
      for (unsigned j = 0; j < Lanes; ++j) {
        auto *N = BinaryOperator::Create((Instruction::BinaryOps)Opcode,
                                         Chain, (*Ls)[j],
                                         "s.unopt.n", I.getIterator());
        inheritMeta(&I, N);
        Chain = N;
      }
      Chains[Idx] = Chain;
      Subs[&I] = Chain;
      Kill.push_back(&I);
      continue;
    }
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!isa<FixedVectorType>(LI->getType()))
        continue;
      if (!Ex(&I))
        return 0;
      Kill.push_back(&I);
      continue;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (!isa<FixedVectorType>(BO->getType()))
        continue;
      if (!Ex(&I))
        return 0;
      Kill.push_back(&I);
      continue;
    }
    if (auto *BI = dyn_cast<BitCastInst>(&I)) {
      if (!isa<FixedVectorType>(BI->getDestTy()))
        continue;
      auto *Ls = Ex(BI->getOperand(0));
      if (!Ls || Ls->size() != Lanes)
        return 0;
      LaneMemo[&I] = *Ls;
      Kill.push_back(&I);
      continue;
    }
  }

  // Close each scalar backedge. The old vector acc phis keep their own
  // (vector-typed) backedge slots untouched: they die by erasure below
  // once use-free, never by type-breaking slot rewrite.
  for (unsigned a = 0; a < Accs.size(); ++a) {
    SS[a]->addIncoming(Chains[a], H);
    Subs[Accs[a].Back] = Chains[a];
  }
  // Each reduce aliases to its own accumulator's chain end. The precheck's
  // AccTree proves which acc(s) feed each reduce operand; with one acc per
  // chain the operand's acc set decides the replacement. A reduce over a
  // two-acc combine (Combines) keeps the old merged behavior: sum the two
  // chain ends with the same opcode.
  unsigned NReduce = 0;
  for (Instruction *R : Reduces) {
    auto *II = cast<IntrinsicInst>(R);
    Value *Root = II->getOperand(0);
    // Peel one lane-wise combine of acc backs (the exit-block merge). The
    // opcode must match the precheck's combine rule (== reduction Opcode).
    Value *AddL = nullptr, *AddR = nullptr;
    if (auto *BO = dyn_cast<BinaryOperator>(Root)) {
      if (BO->getOpcode() == Opcode &&
          isa<FixedVectorType>(BO->getType()) && BO->getParent() != H) {
        AddL = BO->getOperand(0);
        AddR = BO->getOperand(1);
      }
    }
    // Scalar image of an acc-rooted value: the backedge (post-update) maps
    // to the scalar chain end; the PHI (pre-update value on the exit edge)
    // maps to the scalar accumulator PHI itself, which holds the
    // start-of-iteration value. Conflating them includes the last body
    // update in a PHI-rooted reduce (devectorize-preupdate-reduce.ll:
    // one iteration over four ones returned 4 instead of 0).
    auto ImageOf = [&](Value *V) -> Value * {
      for (unsigned a = 0; a < Accs.size(); ++a) {
        if (V == Accs[a].Back)
          return Chains[a];
        if (V == Accs[a].Phi)
          return SS[a];
      }
      return nullptr;
    };
    Value *Repl = nullptr;
    if (!AddL) {
      Repl = ImageOf(Root);
      if (!Repl)
        return 0; // precheck promised acc-rooted; defensive
    } else {
      // reduce distributes over the lane-wise combine (same opcode, checked
      // in the precheck): reduce(a+b) = reduce(a) OP reduce(b). Each side
      // maps through its own scalar image (PHI->phi, Back->chain end), so a
      // combine of pre-update values stays pre-update.
      Value *IL = ImageOf(AddL), *IR = ImageOf(AddR);
      if (!IL || !IR)
        return 0;
      auto *N = BinaryOperator::Create((Instruction::BinaryOps)Opcode, IL, IR,
                                       "s.unopt.m", R->getIterator());
      inheritMeta(R, N);
      Repl = N;
    }
    R->replaceAllUsesWith(Repl);
    if (R->use_empty()) {
      R->eraseFromParent();
      ++NReduce;
    }
  }
  // Exit-block combines die with the manual-fold tree below when a fold
  // consumes them, and were rewired above when they only fed reduces.
  // Manual-fold exits: each fold's extract aliases to the scalar sum of the
  // chain ends (one opcode op per combine side, distributed like reduces).
  // The extract feeds scalar tail-loop PHIs (reseed: the tail now starts from
  // the scalar sum) or a scalar return. Trip-bound defs are untouched; only
  // users rewire. Fold-tree values die by erasure below.
  // eraseFromParent() frees the instruction, so every later sweep consults
  // this set instead of re-reading a dangling pointer (getParent() on a
  // freed node is a use-after-free, not an "already erased" test).
  DenseSet<const Value *> Gone;
  auto EraseIfDead = [&](Value *V) {
    auto *I = dyn_cast_or_null<Instruction>(V);
    if (!I || Gone.contains(V) || !I->getParent() || !I->use_empty())
      return false;
    I->eraseFromParent();
    Gone.insert(V);
    return true;
  };
  for (auto &FE : Folds) {
    // Scalar image of the fold: the tree provably sums every root lane
    // exactly once, so the extract aliases to the opcode applied across all
    // per-acc chain ends (each chain end is that acc's post-update lane sum;
    // all acc inits are the shared splat seed by the precheck... each acc
    // keeps its own seed: sum the chain ends directly).
    Value *Sum = nullptr;
    for (unsigned a = 0; a < Accs.size(); ++a) {
      Value *Img = Subs.lookup(Accs[a].Back);
      if (!Img)
        return 0; // precheck promised per-acc chains; defensive
      if (!Sum) {
        Sum = Img;
        continue;
      }
      auto *N = BinaryOperator::Create((Instruction::BinaryOps)Opcode, Sum,
                                       Img, "s.unopt.m", FE.Extract);
      inheritMeta(FE.Extract, N);
      Sum = N;
    }
    if (!Sum)
      return 0;
    FE.Extract->replaceAllUsesWith(Sum);
    // Erase the tree, terminal included, to a fixpoint. TreeVals is in
    // discovery order, so a value can be visited before the user that still
    // keeps it alive; erasing the terminal inside the loop is what lets the
    // cascade run (erasing it afterwards leaves one stale pass and dead
    // vector ops in the output, which the emitted source would print).
    bool TreeProgress = true;
    while (TreeProgress) {
      TreeProgress = false;
      for (Instruction *&I : FE.TreeVals) {
        if (!EraseIfDead(I))
          continue;
        I = nullptr;
        TreeProgress = true;
      }
    }
  }
  // Rewrite kept scalar operands through Subs (type-guarded).
  for (Instruction &I : *H) {
    if (isa<FixedVectorType>(I.getType()))
      continue;
    for (unsigned o = 0, N = I.getNumOperands(); o < N; ++o) {
      Value *Op = I.getOperand(o);
      if (!isa<FixedVectorType>(Op->getType()))
        continue;
      Value *R = Resolve(Op);
      if (R == Op || R->getType() != Elem)
        continue;
      I.setOperand(o, R);
    }
  }
  for (auto &KV : Subs) {
    Value *Old = const_cast<Value *>(KV.first);
    if (Gone.contains(Old))
      continue; // a fold-tree value erased above (its uses went to Sum)
    if (Old == KV.second || Old->getType() != KV.second->getType())
      continue;
    Old->replaceAllUsesWith(KV.second);
  }
  // Erase dead vector body. Kill order is emission order (loads before
  // their lane consumers); erase in reverse so consumers die first, then
  // repeat to fixpoint for chains the first sweep exposes. The old acc
  // updates (in Kill via Subs) die with the body; the old acc phis erase
  // last, once use-free.
  bool Progress = true;
  while (Progress) {
    Progress = false;
    for (auto It = Kill.rbegin(); It != Kill.rend(); ++It) {
      if (EraseIfDead(*It))
        Progress = true;
    }
  }
  for (auto &A : Accs) {
    if (EraseIfDead(A.Phi))
      continue;
  }

  return 1 + NReduce;
}

} // namespace loopdevec
} // namespace revenant

// Nested namespace + TU-local using: keep ADL names (std::, llvm::) out of
// the revenant::loopdevec scope so LLVM headers resolve cleanly.
