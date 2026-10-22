//===- StraightDevec.cpp - straight-line SLP explosion -------------------===//
//
// Part of the Revenant LLVM fork. See StraightDevec.h.
//
// v1: blocks with no vector PHIs and no vector-reading terminators
// explode integer/float element-wise vector producers (load,
// add/sub/mul/and/or/xor/shl/lshr/ashr, fadd/fsub/fmul/fdiv, same-type
// bitcast, shufflevector lane select) into per-lane scalar ops emitted at
// the producer's own position, in block order. Scalar consumers
// (extractelement lane select, vector stores split per lane) resolve
// through the lane map. Mirrors `crates/unopt/src/devectorize.rs`
// (`Straight`).
//
// Strictly intra-block (like the reference): a vector operand defined
// outside the block bails the block, except constants. Loads materialize
// at their own position, so a clobbering call between a load and a later
// consumer is sound: the lanes were read before the clobber. A *different*
// lane layout read after a clobber still bails (nothing memoized at that
// width). Reductions, masked/gather ops, vector calls, and insertelement
// producers bail the block. Killed values must have no uses outside the
// block. The CFG never changes.
//
// New values use direct Create/new calls into their block (no IRBuilder).
//
//===----------------------------------------------------------------------===//

#include "StraightDevec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace revenant {
namespace straight {
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
// Alignment guaranteed at a lane's byte offset from a vector access with
// alignment A: the common alignment of A and the offset. Lane 0 keeps A;
// lane j > 0 is only as aligned as its offset allows (e.g. align 16 at
// offset 8 is really align 8). Never exceeds the element ABI alignment.
static Align laneAlign(MaybeAlign A, uint64_t Off, unsigned ElemABI) {
  uint64_t Known = A ? A->value() : 1;
  uint64_t Common = std::gcd(Known, Off);
  unsigned Clamped = (unsigned)std::min<uint64_t>(Common, ElemABI);
  if (Clamped == 0)
    Clamped = 1;
  return Align(Clamped);
}

// A lane repack (split/join) materializes new ops: only do it when a
// same-block vector-typed instruction or vector store consumes the result
// (it unlocks further scalarization). A repack feeding only scalar extracts
// stays vector: the extracts already read what they need, and expansion
// with no downstream gain is churn (see lane0-mixed.ll).
static bool repackPays(Instruction *BI, BasicBlock *BB) {
  for (const Use &U : BI->uses()) {
    auto *UI = dyn_cast<Instruction>(U.getUser());
    if (!UI || UI->getParent() != BB)
      continue;
    if (isa<FixedVectorType>(UI->getType()))
      return true;
    if (isa<StoreInst>(UI))
      return true;
  }
  return false;
}

// Reinterpret integer lanes at a new element width (little-endian order):
// split each lane into narrower sub-lanes (trunc + lshr/trunc) or join
// lane pairs into wider lanes (zext + shl/or). Constant lanes fold at
// emission. Returns false when the widths do not divide evenly or either
// side is not integer. All new instructions inherit Tpl's metadata and are
// recorded into New for rollback/sweep.
static bool repackIntLanes(ArrayRef<Value *> In, Type *DstE,
                           Instruction *Tpl, BasicBlock *BB,
                           SmallVectorImpl<Value *> &Out,
                           SmallVectorImpl<Instruction *> &New) {
  if (In.empty() || !DstE->isIntegerTy())
    return false;
  Type *SrcE = In[0]->getType();
  if (!SrcE->isIntegerTy())
    return false;
  for (Value *Ln : In)
    if (Ln->getType() != SrcE)
      return false;
  unsigned SB = SrcE->getScalarSizeInBits();
  unsigned DB = DstE->getScalarSizeInBits();
  if (SB == DB) {
    // Same width, different type object: bit-preserving, forward as-is
    // only when identical (callers check type equality instead).
    return false;
  }
  LLVMContext &Ctx = BB->getContext();
  (void)Ctx;
  if (SB > DB && SB % DB == 0) {
    // Split each lane LE-first.
    for (Value *Ln : In) {
      if (auto *C = dyn_cast<ConstantInt>(Ln)) {
        APInt V = C->getValue();
        for (unsigned Off = 0; Off < SB; Off += DB)
          Out.push_back(ConstantInt::get(DstE, V.lshr(Off).trunc(DB)));
        continue;
      }
      for (unsigned Off = 0; Off < SB; Off += DB) {
        Value *Part = Ln;
        if (Off > 0) {
          Part = BinaryOperator::CreateLShr(
              Ln, ConstantInt::get(SrcE, Off), Tpl->getName() + ".rs",
              Tpl->getIterator());
          inheritMeta(Tpl, cast<Instruction>(Part));
          New.push_back(cast<Instruction>(Part));
        }
        auto *T = new TruncInst(Part, DstE, Tpl->getName() + ".rp",
                                Tpl->getIterator());
        inheritMeta(Tpl, T);
        New.push_back(T);
        Out.push_back(T);
      }
    }
    return true;
  }
  if (DB > SB && DB % SB == 0) {
    // Join lane groups LE-first.
    unsigned K = DB / SB;
    if (In.size() % K != 0)
      return false;
    for (size_t i = 0; i < In.size(); i += K) {
      Value *Acc = nullptr;
      for (unsigned k = 0; k < K; ++k) {
        Value *Ln = In[i + k];
        Value *W = Ln;
        if (auto *C = dyn_cast<ConstantInt>(Ln)) {
          W = ConstantInt::get(DstE, C->getValue().zext(DB));
        } else {
          W = new ZExtInst(Ln, DstE, Tpl->getName() + ".rj",
                           Tpl->getIterator());
          inheritMeta(Tpl, cast<Instruction>(W));
          New.push_back(cast<Instruction>(W));
        }
        if (k > 0) {
          auto *S = BinaryOperator::CreateShl(
              W, ConstantInt::get(DstE, (uint64_t)k * SB),
              Tpl->getName() + ".rj.s", Tpl->getIterator());
          inheritMeta(Tpl, S);
          New.push_back(S);
          W = S;
        }
        if (!Acc) {
          Acc = W;
        } else {
          auto *O = BinaryOperator::CreateOr(
              Acc, W, Tpl->getName() + ".rj.o", Tpl->getIterator());
          inheritMeta(Tpl, O);
          New.push_back(O);
          Acc = O;
        }
      }
      Out.push_back(Acc);
    }
    return true;
  }
  return false;
}

bool vecParts(Type *T, unsigned &Lanes, Type *&Elem) {
  auto *FVT = dyn_cast<FixedVectorType>(T);
  if (!FVT)
    return false; // v1: no scalable vectors
  Elem = FVT->getElementType();
  bool IsFloat = Elem->isFloatingPointTy();
  if (!Elem->isIntegerTy() && !IsFloat)
    return false;
  if (IsFloat && !Elem->isFloatTy() && !Elem->isDoubleTy() &&
      !Elem->isHalfTy())
    return false;
  unsigned Bits = Elem->getScalarSizeInBits();
  if (Bits < 8 || Bits > 64)
    return false;
  Lanes = FVT->getNumElements();
  return Lanes >= 2 && (Lanes * Bits) <= 512;
}

bool isLaneOp(unsigned Opcode) {
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
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::FMul:
  case Instruction::FDiv:
    return true;
  default:
    return false;
  }
}

// One lane of a vector-typed constant as an Elem-typed scalar. Reuses the
// existing constant values (never synthesizes float constants): a splat
// reuses its element, a per-lane vector reuses lane L. Poison lanes stay
// poison (not undef): uses that trap on poison must keep trapping.
Value *laneConst(Constant *C, Type *Elem, unsigned Lane) {
  if (isa<UndefValue>(C))
    return UndefValue::get(Elem);
  if (isa<PoisonValue>(C))
    return PoisonValue::get(Elem);
  if (isa<ConstantAggregateZero>(C)) {
    if (Elem->isFloatingPointTy())
      return ConstantFP::getZero(Elem);
    return ConstantInt::get(Elem, 0);
  }
  if (auto *CV = dyn_cast<ConstantVector>(C)) {
    if (Constant *Splat = CV->getSplatValue())
      C = Splat;
    else {
      if (Lane >= CV->getNumOperands())
        return nullptr;
      Constant *L = CV->getOperand(Lane);
      if (L->getType() == Elem)
        return L;
      if (auto *CI = dyn_cast<ConstantInt>(L)) {
        if (!Elem->isIntegerTy())
          return nullptr;
        return ConstantInt::get(
            Elem, CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits()));
      }
      if (auto *CF = dyn_cast<ConstantFP>(L)) {
        if (!Elem->isFloatingPointTy())
          return nullptr;
        if (CF->getType() == Elem)
          return CF;
        return nullptr;
      }
      return nullptr;
    }
  }
  if (auto *CDV = dyn_cast<ConstantDataVector>(C)) {
    Constant *Splat = CDV->getSplatValue();
    if (Splat && Splat->getType() == Elem)
      return Splat;
    if (Lane >= CDV->getNumElements())
      return nullptr;
    Constant *L = CDV->getElementAsConstant(Lane);
    if (L->getType() == Elem)
      return L;
    if (auto *CI = dyn_cast<ConstantInt>(L)) {
      if (!Elem->isIntegerTy())
        return nullptr;
      return ConstantInt::get(
          Elem, CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits()));
    }
    return nullptr;
  }
  if (auto *CF = dyn_cast<ConstantFP>(C)) {
    if (CF->getType() == Elem)
      return CF;
    return nullptr;
  }
  if (auto *CI = dyn_cast<ConstantInt>(C))
    return ConstantInt::get(
        Elem, CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits()));
  return nullptr;
}

// Per-block explosion state.
struct BlockCx {
  BasicBlock *BB;
  LLVMContext *Ctx = nullptr;
  // Values never to kill: vector defs used outside BB. They stay alive so
  // later blocks can read their lanes through extracts (see lanesOf).
  const DenseSet<const Value *> *Pinned = nullptr;
  // Emission sink for this block (rollback + sweep); anchor updated per
  // walked instruction so lane materialization lands in order.
  SmallVectorImpl<Instruction *> *Emitted = nullptr;
  Instruction *Anchor = nullptr;
  // Vector value -> its lane scalars (only for same-block defs exploded
  // at their own position, plus constants handled inline).
  DenseMap<const Value *, SmallVector<Value *, 4>> Lanes;
  // Scalar aliases for killed scalar defs (extract dsts).
  DenseMap<const Value *, Value *> Subs;
  SmallVector<Instruction *, 16> Kill;
  unsigned Changed = 0;

  void inheritMetaHere(const Instruction *Src, Instruction *Dst) {
    if (MDNode *N = Src->getMetadata(VA_MD))
      Dst->setMetadata(VA_MD, N);
    if (MDNode *N = Src->getMetadata(REGION_MD))
      Dst->setMetadata(REGION_MD, N);
    if (MDNode *N = Src->getMetadata(OPAQUE_MD))
      Dst->setMetadata(OPAQUE_MD, N);
    Dst->setDebugLoc(Src->getDebugLoc());
  }

  // Lane scalars for V (same-block def already exploded, or constant).
  // Null when V is defined outside BB (except constants). Copies into Out
  // (no shared-buffer aliasing across the two operand fetches).
  bool lanesOf(Value *V, unsigned NeedLanes, Type *NeedElem,
               SmallVectorImpl<Value *> &Out) {
    Out.clear();
    if (auto *C = dyn_cast<Constant>(V)) {
      for (unsigned j = 0; j < NeedLanes; ++j) {
        Value *Lj = laneConst(C, NeedElem, j);
        if (!Lj)
          return false;
        Out.push_back(Lj);
      }
      return true;
    }
    auto It = Lanes.find(V);
    if (It != Lanes.end()) {
      if (It->second.size() != NeedLanes)
        return false;
      Out.append(It->second.begin(), It->second.end());
      return true;
    }
    // Defined outside BB (loop-invariant broadcast, later-block value read
    // through dominance, or function argument): materialize lanes as
    // extractelements at the anchor. Extracts read the SSA value without
    // touching memory, so clobbers cannot disturb them; the pinned set
    // guarantees the defining block never erases V while these live.
    // Counts and lane types must match exactly.
    auto *VT = dyn_cast<FixedVectorType>(V->getType());
    if (!VT || VT->getNumElements() != NeedLanes ||
        VT->getElementType() != NeedElem)
      return false;
    if (!NeedElem->isIntegerTy() && !NeedElem->isFloatingPointTy())
      return false;
    auto *Def = dyn_cast<Instruction>(V);
    Instruction *Src = Def ? Def : Anchor;
    if (!Anchor || !Emitted || !Ctx)
      return false;
    SmallVector<Value *, 4> Ls;
    for (unsigned j = 0; j < NeedLanes; ++j) {
      auto *E = ExtractElementInst::Create(
          V, ConstantInt::get(Type::getInt32Ty(*Ctx), j),
          V->getName() + ".x.lane", Anchor->getIterator());
      inheritMetaHere(Src, E);
      Emitted->push_back(E);
      Ls.push_back(E);
    }
    Lanes[V] = Ls;
    Out.append(Ls.begin(), Ls.end());
    return true;
  }
};

bool explodeBlock(Function &F, BasicBlock *BB, unsigned &ChangedOut,
                    SmallVectorImpl<Instruction *> &EmittedOut,
                    const DenseSet<const Value *> &Pinned) {
  BlockCx Cx;
  Cx.BB = BB;
  Cx.Pinned = &Pinned;
  LLVMContext &Ctx = BB->getContext();
  Cx.Ctx = &Ctx;
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);

  SmallVector<Instruction *, 16> Insns;
  for (Instruction &I : *BB)
    Insns.push_back(&I);

  // Failure inside the walk: roll back what THIS block emitted so far.
  // Emitted lane code is use-free on this path (rewire runs only after
  // the full walk), so reverse-erase is safe.
  SmallVector<Instruction *, 32> LocalEmitted;
  Cx.Emitted = &LocalEmitted;
  auto Fail = [&]() {
    for (auto It = LocalEmitted.rbegin(); It != LocalEmitted.rend(); ++It) {
      Instruction *K = *It;
      if (K->getParent() == nullptr || !K->use_empty())
        continue;
      K->eraseFromParent();
    }
    return false;
  };

  for (Instruction *IP : Insns) {
    Instruction &I = *IP;
    Cx.Anchor = &I;
    // Pinned values stay alive for cross-block lane readers: never explode
    // or kill them. Their producers may still explode when use-free rules
    // allow, but nothing here demands lanes of a pinned value.
    if (Pinned.contains(&I))
      continue;
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      // Scalar i128 loads (clang SLP shape) model as 4 x i32 lanes
      // LE-first, but only when every user is a bitcast to a vector type:
      // loads feeding i128 stores are whole-object copies owned by the
      // pair-split phase and stay whole.
      bool IsI128Load =
          LI->getType()->isIntegerTy(128) && !LI->isVolatile() &&
          LI->isSimple() && LI->getPointerAddressSpace() == 0;
      if (IsI128Load) {
        for (const Use &U : LI->uses()) {
          auto *UI = dyn_cast<BitCastInst>(U.getUser());
          if (!UI || !isa<FixedVectorType>(UI->getDestTy()) ||
              UI->getParent() != BB) {
            IsI128Load = false;
            break;
          }
        }
      }
      if (IsI128Load) {
        Type *I32 = Type::getInt32Ty(Ctx);
        unsigned ElemABI =
            F.getParent()->getDataLayout().getABITypeAlign(I32).value();
        SmallVector<Value *, 4> Ls;
        for (unsigned j = 0; j < 4; ++j) {
          uint64_t ByteOff = (uint64_t)j * 4;
          auto *Off = ConstantInt::get(I64, ByteOff);
          auto *G = GetElementPtrInst::Create(
              I8, LI->getPointerOperand(), Off, LI->getName() + ".p",
              LI->getIterator());
          inheritMeta(LI, G);
          auto *Ln = new LoadInst(I32, G, LI->getName() + ".l",
                                  LI->isVolatile(), LI->getIterator());
          inheritMeta(LI, Ln);
          Ln->setAlignment(laneAlign(LI->getAlign(), ByteOff, ElemABI));
          LocalEmitted.push_back(G);
          LocalEmitted.push_back(Ln);
          ++Cx.Changed;
          Ls.push_back(Ln);
        }
        Cx.Lanes[LI] = Ls;
        Cx.Kill.push_back(LI);
        continue;
      }
      if (!vecParts(LI->getType(), L, E))
        continue;
      if (LI->isVolatile() || !LI->isSimple())
        return Fail();
      unsigned Esz = E->getScalarSizeInBits() / 8;
      unsigned ElemABI = F.getParent()->getDataLayout().getABITypeAlign(E).value();
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        uint64_t ByteOff = (uint64_t)j * Esz;
        auto *Off = ConstantInt::get(I64, ByteOff);
        auto *G = GetElementPtrInst::Create(
            I8, LI->getPointerOperand(), Off, LI->getName() + ".p",
            LI->getIterator());
        inheritMeta(LI, G);
        auto *Ln = new LoadInst(E, G, LI->getName() + ".l",
                                LI->isVolatile(), LI->getIterator());
        inheritMeta(LI, Ln);
        Ln->setAlignment(laneAlign(LI->getAlign(), ByteOff, ElemABI));
        LocalEmitted.push_back(G);
        LocalEmitted.push_back(Ln);
        ++Cx.Changed;
        Ls.push_back(Ln);
      }
      Cx.Lanes[LI] = Ls;
      Cx.Kill.push_back(LI);
      continue;
    }
    // Scalar i128 lane shift (GCC SLP horizontal-fold shape): an i128 that
    // already carries lanes (bitcast from a laned total-128-bit vector)
    // shifted right by a whole-lane multiple permutes lanes, zero-filling
    // the top. Only lane-multiple shifts model exactly; anything else, or
    // an unlaned operand, is left alone. Must run before the BinaryOperator
    // branch below (scalar i128 is not a vector type, so that branch would
    // skip it).
    if (I.getOpcode() == Instruction::LShr && I.getType()->isIntegerTy(128)) {
      auto *BO = cast<BinaryOperator>(&I);
      auto *Amt = dyn_cast<ConstantInt>(BO->getOperand(1));
      if (!Amt)
        continue;
      uint64_t Sh = Amt->getZExtValue();
      auto It = Cx.Lanes.find(BO->getOperand(0));
      if (It == Cx.Lanes.end() || It->second.empty())
        continue;
      Type *LaneTy = It->second[0]->getType();
      unsigned LaneBits = LaneTy->getScalarSizeInBits();
      for (Value *Ln : It->second)
        if (Ln->getType() != LaneTy)
          LaneBits = 0;
      if (LaneBits == 0 || Sh % LaneBits != 0 || Sh >= 128)
        continue;
      unsigned N = It->second.size();
      unsigned K = (unsigned)(Sh / LaneBits);
      Value *Zero = LaneTy->isFloatingPointTy()
                        ? (Value *)UndefValue::get(LaneTy)
                        : (Value *)ConstantInt::get(LaneTy, 0);
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < N; ++j)
        Ls.push_back(j + K < N ? It->second[j + K] : Zero);
      Cx.Lanes[BO] = Ls;
      Cx.Kill.push_back(BO);
      ++Cx.Changed;
      continue;
    }
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(BO->getType(), L, E))
        continue;
      if (!isLaneOp(BO->getOpcode()))
        return Fail();
      SmallVector<Value *, 4> AS, BS;
      if (!Cx.lanesOf(BO->getOperand(0), L, E, AS))
        return Fail();
      if (!Cx.lanesOf(BO->getOperand(1), L, E, BS))
        return Fail();
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        auto *N = BinaryOperator::Create(BO->getOpcode(), AS[j], BS[j],
                                         BO->getName() + ".s",
                                         BO->getIterator());
        inheritMeta(BO, N);
        LocalEmitted.push_back(N);
        ++Cx.Changed;
        Ls.push_back(N);
      }
      Cx.Lanes[BO] = Ls;
      Cx.Kill.push_back(BO);
      continue;
    }
    // Vector compare (zlib saturating-narrow shape): per-lane icmp with the
    // same predicate. Lanes are i1 scalars, consumed by vector selects and
    // sext-from-i1 below. Only integer compares; anything else bails.
    if (auto *CI = dyn_cast<ICmpInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(CI->getOperand(0)->getType(), L, E))
        continue;
      if (!CI->getType()->isVectorTy())
        continue;
      SmallVector<Value *, 4> AS, BS;
      if (!Cx.lanesOf(CI->getOperand(0), L, E, AS))
        return Fail();
      if (!Cx.lanesOf(CI->getOperand(1), L, E, BS))
        return Fail();
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        auto *N = new ICmpInst(CI->getIterator(), CI->getPredicate(), AS[j],
                               BS[j], CI->getName() + ".s");
        inheritMeta(CI, N);
        LocalEmitted.push_back(N);
        ++Cx.Changed;
        Ls.push_back(N);
      }
      Cx.Lanes[CI] = Ls;
      Cx.Kill.push_back(CI);
      continue;
    }
    // Vector select (saturating-narrow shape): per-lane select over laned
    // i1 conditions. The condition lanes come from the icmp rule above (or
    // an i1 constant); arms resolve through the lane map like binops.
    if (auto *SI = dyn_cast<SelectInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(SI->getType(), L, E))
        continue;
      auto *CondVT = dyn_cast<FixedVectorType>(SI->getCondition()->getType());
      if (!CondVT || CondVT->getNumElements() != L ||
          !CondVT->getElementType()->isIntegerTy(1))
        return Fail();
      Type *I1 = Type::getInt1Ty(Ctx);
      SmallVector<Value *, 4> CS, TS, FS;
      if (!Cx.lanesOf(SI->getCondition(), L, I1, CS))
        return Fail();
      if (!Cx.lanesOf(SI->getTrueValue(), L, E, TS))
        return Fail();
      if (!Cx.lanesOf(SI->getFalseValue(), L, E, FS))
        return Fail();
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        auto *N = SelectInst::Create(CS[j], TS[j], FS[j], SI->getName() + ".s",
                                     SI->getIterator());
        inheritMeta(SI, N);
        LocalEmitted.push_back(N);
        ++Cx.Changed;
        Ls.push_back(N);
      }
      Cx.Lanes[SI] = Ls;
      Cx.Kill.push_back(SI);
      continue;
    }
    // Same-count vector trunc/sext/zext (narrowing shape): per-lane casts.
    // sext from i1 lanes yields -1/0 masks, exactly like the vector form.
    // Wrap flags are preserved on trunc (the only one that carries them).
    // Bitcasts and scalar-destination casts (zext-to-i128 broadcast) have
    // dedicated branches below: this branch only fires for vector
    // trunc/sext/zext, and falls through otherwise.
    if (auto *CI = dyn_cast<CastInst>(&I)) {
      unsigned Op = CI->getOpcode();
      unsigned L = 0;
      Type *E = nullptr;
      auto *SrcVT = dyn_cast<FixedVectorType>(CI->getOperand(0)->getType());
      bool LaneCast =
          !isa<BitCastInst>(CI) &&
          (Op == Instruction::Trunc || Op == Instruction::SExt ||
           Op == Instruction::ZExt) &&
          vecParts(CI->getType(), L, E) && SrcVT &&
          SrcVT->getNumElements() == L &&
          (SrcVT->getElementType()->isIntegerTy() ||
           SrcVT->getElementType()->isFloatingPointTy());
      if (LaneCast) {
        Type *SE = SrcVT->getElementType();
        SmallVector<Value *, 4> AV;
        if (!Cx.lanesOf(CI->getOperand(0), L, SE, AV))
          return Fail();
        SmallVector<Value *, 4> Ls;
        for (unsigned j = 0; j < L; ++j) {
          auto *N = CastInst::Create((Instruction::CastOps)Op, AV[j], E,
                                     CI->getName() + ".s", CI->getIterator());
          if (auto *TI = dyn_cast<TruncInst>(N)) {
            TI->setHasNoSignedWrap(CI->hasNoSignedWrap());
            TI->setHasNoUnsignedWrap(CI->hasNoUnsignedWrap());
          }
          inheritMeta(CI, N);
          LocalEmitted.push_back(N);
          ++Cx.Changed;
          Ls.push_back(N);
        }
        Cx.Lanes[CI] = Ls;
        Cx.Kill.push_back(CI);
        continue;
      }
      // Other casts (fp, ptr, bitcast, scalar-dest) fall through to the
      // dedicated branches below; unrecognized vector producers still bail
      // at the tail of the walk.
    }
    if (auto *BI = dyn_cast<BitCastInst>(&I)) {
      // zext-broadcast shapes): a bitcast between i128 and a laned vector
      // whose primitive size is 128 bits carries the same lanes. Lane count
      // and lane types must match on the i128 -> vector direction.
      // Unlaned i128 operands keep the old behavior (skip / bail below).
      if (BI->getDestTy()->isIntegerTy(128)) {
        auto *SrcVT = dyn_cast<FixedVectorType>(BI->getSrcTy());
        if (!SrcVT || SrcVT->getPrimitiveSizeInBits() != 128)
          continue;
        unsigned SL = SrcVT->getNumElements();
        Type *SE = SrcVT->getElementType();
        SmallVector<Value *, 4> AV;
        if (!Cx.lanesOf(BI->getOperand(0), SL, SE, AV))
          return Fail();
        Cx.Lanes[BI] = AV;
        Cx.Kill.push_back(BI);
        ++Cx.Changed;
        continue;
      }
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(BI->getDestTy(), L, E))
        continue;
      if (BI->getSrcTy()->isIntegerTy(128)) {
        // i128 carries registered lanes at some integer width: forward them
        // directly on exact match, else reinterpret LE-first (split or join)
        // when total bits agree. An unregistered i128 defined outside BB by
        // a narrow zext models as [zext-to-i64, 0] synthesized in-home (pure
        // value function of a dominating scalar: no memory, no clobber).
        const SmallVector<Value *, 4> *Lanes = nullptr;
        SmallVector<Value *, 4> ZextLanes;
        auto It = Cx.Lanes.find(BI->getOperand(0));
        if (It != Cx.Lanes.end()) {
          Lanes = &It->second;
        } else {
          auto *ZI = dyn_cast<ZExtInst>(BI->getOperand(0));
          if (!ZI || ZI->getParent() == BB)
            return Fail();
          unsigned SW = ZI->getSrcTy()->getScalarSizeInBits();
          if (SW != 8 && SW != 16 && SW != 32)
            return Fail();
          Type *I64 = Type::getInt64Ty(Ctx);
          auto *Lo = new ZExtInst(ZI->getOperand(0), I64,
                                   BI->getName() + ".zl", BI->getIterator());
          inheritMeta(ZI, Lo);
          LocalEmitted.push_back(Lo);
          ++Cx.Changed;
          ZextLanes.push_back(Lo);
          ZextLanes.push_back(ConstantInt::get(I64, 0));
          Lanes = &ZextLanes;
        }
        if (Lanes->empty())
          return Fail();
        if (Lanes->size() == (size_t)L) {
          for (Value *Ln : *Lanes)
            if (Ln->getType() != E)
              return Fail();
          Cx.Lanes[BI] = *Lanes;
          Cx.Kill.push_back(BI);
          ++Cx.Changed;
          continue;
        }
        SmallVector<Value *, 4> Ls;
        unsigned Before = (unsigned)LocalEmitted.size();
        if (!repackPays(BI, BB) ||
            !repackIntLanes(*Lanes, E, BI, BB, Ls, LocalEmitted))
          return Fail();
        if (Ls.size() != L)
          return Fail();
        Cx.Changed += (unsigned)LocalEmitted.size() - Before;
        Cx.Lanes[BI] = Ls;
        Cx.Kill.push_back(BI);
        continue;
      }
      if (BI->getDestTy() != BI->getSrcTy()) {
        // Cross-width lane split (GCC dot4 shape): reinterpret integer
        // lanes LE-first at the destination width (split or join).
        auto *SrcVT = dyn_cast<FixedVectorType>(BI->getSrcTy());
        Type *SE = SrcVT ? SrcVT->getElementType() : nullptr;
        if (!SrcVT || !SE || !SE->isIntegerTy() || !E->isIntegerTy())
          return Fail();
        SmallVector<Value *, 4> AV;
        if (!Cx.lanesOf(BI->getOperand(0), SrcVT->getNumElements(), SE, AV))
          return Fail();
        SmallVector<Value *, 4> Ls;
        unsigned Before = (unsigned)LocalEmitted.size();
        if (!repackPays(BI, BB) ||
            !repackIntLanes(AV, E, BI, BB, Ls, LocalEmitted))
          return Fail();
        if (Ls.size() != L)
          return Fail();
        Cx.Changed += (unsigned)LocalEmitted.size() - Before;
        Cx.Lanes[BI] = Ls;
        Cx.Kill.push_back(BI);
        continue;
      }
      SmallVector<Value *, 4> AV;
      if (!Cx.lanesOf(BI->getOperand(0), L, E, AV))
        return Fail();
      Cx.Lanes[BI] = AV;
      Cx.Kill.push_back(BI);
      continue;
    }
    if (auto *SV = dyn_cast<ShuffleVectorInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(SV->getType(), L, E))
        return Fail();
      // Lane routing is width-agnostic: each result lane selects one
      // operand lane by mask index (widening shuffles like <4 x i32> to
      // <8 x i32> duplicate lanes; narrowing selects a subset). Operand
      // lanes resolve at their own widths; element types must match.
      auto *VT0 = dyn_cast<FixedVectorType>(SV->getOperand(0)->getType());
      auto *VT1 = dyn_cast<FixedVectorType>(SV->getOperand(1)->getType());
      if (!VT0 || !VT1 || VT0->getElementType() != E ||
          VT1->getElementType() != E)
        return Fail();
      unsigned N0 = VT0->getNumElements();
      unsigned N1 = VT1->getNumElements();
      SmallVector<Value *, 4> AS, BS;
      if (!Cx.lanesOf(SV->getOperand(0), N0, E, AS))
        return Fail();
      if (!Cx.lanesOf(SV->getOperand(1), N1, E, BS))
        return Fail();
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        int M = SV->getMaskValue(j);
        if (M < 0)
          Ls.push_back(UndefValue::get(E));
        else if ((unsigned)M < N0)
          Ls.push_back(AS[M]);
        else if ((unsigned)M - N0 < N1)
          Ls.push_back(BS[M - N0]);
        else
          return Fail();
      }
      Cx.Lanes[SV] = Ls;
      Cx.Kill.push_back(SV);
      continue;
    }
    if (auto *EI = dyn_cast<ExtractElementInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      bool SrcVec = vecParts(EI->getVectorOperand()->getType(), L, E);
      if (!SrcVec)
        continue;
      auto *Idx = dyn_cast<ConstantInt>(EI->getIndexOperand());
      if (!Idx || Idx->getZExtValue() >= L)
        return Fail();
      SmallVector<Value *, 4> AV;
      if (!Cx.lanesOf(EI->getVectorOperand(), L, E, AV))
        return Fail();
      Value *Lane = AV[Idx->getZExtValue()];
      // Same-type extract of an already-scalar lane: rewire uses directly
      // (no alias entry needed). Cross-type (extract i32 from <4 x float>
      // lanes, etc.) needs a cast: bail the block instead of guessing.
      if (Lane->getType() != EI->getType())
        return Fail();
      Cx.Subs[EI] = Lane;
      Cx.Kill.push_back(EI);
      ++Cx.Changed;
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(SI->getValueOperand()->getType(), L, E))
        continue;
      SmallVector<Value *, 4> AS;
      if (!Cx.lanesOf(SI->getValueOperand(), L, E, AS))
        return Fail();
      unsigned Esz = E->getScalarSizeInBits() / 8;
      unsigned ElemABI = F.getParent()->getDataLayout().getABITypeAlign(E).value();
      for (unsigned j = 0; j < L; ++j) {
        uint64_t ByteOff = (uint64_t)j * Esz;
        auto *Off = ConstantInt::get(I64, ByteOff);
        auto *G = GetElementPtrInst::Create(I8, SI->getPointerOperand(),
                                            Off, SI->getName() + ".p",
                                            SI->getIterator());
        inheritMeta(SI, G);
        auto *NS = new StoreInst(AS[j], G, SI->isVolatile(),
                                 laneAlign(SI->getAlign(), ByteOff, ElemABI),
                                 SI->getIterator());
        inheritMeta(SI, NS);
        LocalEmitted.push_back(G);
        LocalEmitted.push_back(NS);
        ++Cx.Changed;
      }
      Cx.Kill.push_back(SI);
      ++Cx.Changed;
      continue;
    }
    // Lane-0 broadcast (zlib SLP shape): a scalar zero-extended to i128 is
    // the low 64 bits with the high 64 bits exactly zero. Models as lanes
    // [s, 0] (exact zero, not undef: readers of the high lane must see 0)
    // so the later shuffle/add/store split scalarizes lane 0. Only narrow
    // integer sources; anything else is left alone.
    if (auto *ZI = dyn_cast<ZExtInst>(&I)) {
      if (!ZI->getDestTy()->isIntegerTy(128))
        continue;
      if (!ZI->getSrcTy()->isIntegerTy(8) &&
          !ZI->getSrcTy()->isIntegerTy(16) &&
          !ZI->getSrcTy()->isIntegerTy(32) &&
          !ZI->getSrcTy()->isIntegerTy(64))
        continue;
      Type *I64 = Type::getInt64Ty(Ctx);
      Value *Op = ZI->getOperand(0);
      Value *Lo = Op;
      if (!Op->getType()->isIntegerTy(64)) {
        Lo = new ZExtInst(Op, I64, ZI->getName() + ".b.lo",
                           ZI->getIterator());
        inheritMeta(ZI, cast<Instruction>(Lo));
        LocalEmitted.push_back(cast<Instruction>(Lo));
        ++Cx.Changed;
      }
      SmallVector<Value *, 4> Ls;
      Ls.push_back(Lo);
      Ls.push_back(ConstantInt::get(I64, 0));
      Cx.Lanes[ZI] = Ls;
      Cx.Kill.push_back(ZI);
      ++Cx.Changed;
      continue;
    }
    unsigned IL = 0;
    Type *IE = nullptr;
    // Lane-0 broadcast (zlib SLP shape): insertelement of a scalar into
    // lane 0 of a poison/undef/zero base models as lanes [s, undef, ...].
    // Other insertelement shapes still bail the block.
    if (auto *II = dyn_cast<InsertElementInst>(&I)) {
      if (!vecParts(II->getType(), IL, IE))
        continue;
      auto *Idx = dyn_cast<ConstantInt>(II->getOperand(2));
      auto *Base = dyn_cast<Constant>(II->getOperand(0));
      bool BaseEmpty =
          Base && (isa<UndefValue>(Base) || isa<PoisonValue>(Base) ||
                   isa<ConstantAggregateZero>(Base));
      if (!Idx || !Idx->isZero() || !BaseEmpty ||
          II->getOperand(1)->getType() != IE)
        return Fail();
      SmallVector<Value *, 4> Ls;
      Ls.push_back(II->getOperand(1));
      for (unsigned j = 1; j < IL; ++j)
        Ls.push_back(UndefValue::get(IE));
      Cx.Lanes[II] = Ls;
      Cx.Kill.push_back(II);
      ++Cx.Changed;
      continue;
    }
    if (auto *CI = dyn_cast<CallInst>(&I)) {
      if (isa<FixedVectorType>(CI->getType()))
        return Fail();
      if (auto *II = dyn_cast<IntrinsicInst>(CI)) {
        switch (II->getIntrinsicID()) {
        case Intrinsic::vector_reduce_add:
        case Intrinsic::vector_reduce_mul:
        case Intrinsic::vector_reduce_and:
        case Intrinsic::vector_reduce_or:
        case Intrinsic::vector_reduce_xor:
        case Intrinsic::vector_reduce_fadd:
        case Intrinsic::vector_reduce_fmul:
        case Intrinsic::masked_load:
        case Intrinsic::masked_store:
        case Intrinsic::masked_gather:
        case Intrinsic::masked_scatter:
          return Fail();
        default:
          break;
        }
      }
      continue;
    }
    if (isa<FixedVectorType>(I.getType()))
      return Fail(); // any other vector producer bails the block
  }

  // Commit precheck: killed values (and scalar aliases) must have no uses
  // outside BB; otherwise roll back. Uses *inside* later blocks are only
  // acceptable for scalar aliases (extract dsts): the function-wide pass
  // below rewires them. Vector defs used outside stay vector: fail.
  // Killed i128 lane carriers (fold bitcasts/shifts) must die whole: every
  // use must be another killed def in this block, else a live i128 consumer
  // (arithmetic, store, return) would dangle and the block rolls back.
  DenseSet<Instruction *> KillSet(Cx.Kill.begin(), Cx.Kill.end());
  for (Instruction *K : Cx.Kill) {
    // Scalar aliases (extracts) may be used outside; vector defs may not.
    if (Cx.Subs.contains(K))
      continue;
    bool IsI128 = K->getType()->isIntegerTy(128);
    for (const Use &U : K->uses()) {
      auto *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI || UI->getParent() != BB)
        return false;
      if (IsI128 && !KillSet.contains(UI))
        return false;
    }
  }
  for (auto &KV : Cx.Subs) {
    auto *KD = dyn_cast<Instruction>(const_cast<Value *>(KV.first));
    bool ScalarAlias = KD && !isa<FixedVectorType>(KD->getType());
    for (const Use &U : KV.first->uses()) {
      auto *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI)
        return false;
      if (UI->getParent() != BB && !ScalarAlias)
        return false;
    }
  }

  // Rewire same-block uses through lanes/aliases (type-guarded).
  // Rewire this block AND later blocks through this block's aliases:
  // scalar extract dsts may feed code below (the reference applies subs
  // function-wide). Only same-type scalar aliases propagate; vector defs
  // never leave their block (commit precheck guarantees no outside uses).
  // Aliases flow forward only (layout order after BB). Without aliases the
  // walk has nothing to rewire, and walking every later block per block
  // made the pass quadratic in function size.
  bool Past = false;
  for (BasicBlock &JB : F) {
    if (Cx.Subs.empty())
      break;
    if (&JB == BB)
      Past = true;
    if (!Past)
      continue;
    for (Instruction &J : JB) {
      for (unsigned o = 0, N = J.getNumOperands(); o < N; ++o) {
        Value *Op = J.getOperand(o);
        auto It = Cx.Subs.find(Op);
        if (It == Cx.Subs.end() || It->second == Op)
          continue;
        if (It->second->getType() != Op->getType())
          continue;
        // Scalar aliases only (extract dsts); vector lanes stay local.
        auto *OD = dyn_cast<Instruction>(Op);
        if (OD && isa<FixedVectorType>(OD->getType()))
          continue;
        J.setOperand(o, It->second);
      }
    }
  }
  // Erase dead vector defs in reverse (consumers before producers).
  for (auto It = Cx.Kill.rbegin(); It != Cx.Kill.rend(); ++It) {
    Instruction *K = *It;
    if (K->getParent() == nullptr || !K->use_empty())
      continue;
    K->eraseFromParent();
  }
  // Sweep emitted lane scalars left use-free (dead fold lanes, unselected
  // shuffle lanes). Stores are never swept: a store with no uses is still
  // observable. Reverse order erases consumers before their lane inputs.
  for (auto It = LocalEmitted.rbegin(); It != LocalEmitted.rend(); ++It) {
    Instruction *K = *It;
    if (K->getParent() == nullptr || !K->use_empty())
      continue;
    if (isa<StoreInst>(K) || K->isTerminator())
      continue;
    K->eraseFromParent();
    ++Cx.Changed;
  }
  ChangedOut += Cx.Changed;
  EmittedOut.append(LocalEmitted.begin(), LocalEmitted.end());
  return Cx.Changed > 0;
}

// Rollback: erase emitted lane code in reverse (use-free by construction
// on the failure path: nothing rewired yet).
void rollbackEmitted(SmallVectorImpl<Instruction *> &Emitted) {
  for (auto It = Emitted.rbegin(); It != Emitted.rend(); ++It) {
    Instruction *K = *It;
    if (K->getParent() == nullptr || !K->use_empty())
      continue;
    K->eraseFromParent();
  }
}

// A straight-line AVX2 reduction may carry a 256-bit vector through an
// i128 pair solely to extract one 128-bit half and bitcast it back to the
// original lane width. Promote that exact half round-trip to a lane shuffle
// before the scalar lane map is built, so unsupported <2 x i128> carriers do
// not veto the entire block.
unsigned promoteI128ExtractHalves(Function &F) {
  if (!F.getParent()->getDataLayout().isLittleEndian())
    return 0;
  unsigned Changed = 0;
  for (BasicBlock &BB : F) {
    SmallVector<BitCastInst *, 8> BackCasts;
    for (Instruction &I : BB)
      if (auto *BC = dyn_cast<BitCastInst>(&I))
        if (BC->getSrcTy()->isIntegerTy(128) &&
            isa<FixedVectorType>(BC->getDestTy()))
          BackCasts.push_back(BC);

    for (BitCastInst *Back : BackCasts) {
      if (Back->getParent() != &BB)
        continue;
      auto *TargetTy = dyn_cast<FixedVectorType>(Back->getDestTy());
      auto *Extract = dyn_cast<ExtractElementInst>(Back->getOperand(0));
      auto *Index = Extract
                        ? dyn_cast<ConstantInt>(Extract->getIndexOperand())
                        : nullptr;
      auto *HalfView =
          Extract ? dyn_cast<BitCastInst>(Extract->getVectorOperand()) : nullptr;
      auto *HalfTy =
          HalfView ? dyn_cast<FixedVectorType>(HalfView->getDestTy()) : nullptr;
      auto *SourceTy =
          HalfView ? dyn_cast<FixedVectorType>(HalfView->getSrcTy()) : nullptr;
      if (!TargetTy || !Extract || Extract->getParent() != &BB || !Index ||
          Index->getZExtValue() > 1 || !HalfView ||
          HalfView->getParent() != &BB || !HalfTy || !SourceTy ||
          HalfTy->getNumElements() != 2 ||
          !HalfTy->getElementType()->isIntegerTy(128) ||
          HalfTy->getPrimitiveSizeInBits() != 256 ||
          SourceTy->getPrimitiveSizeInBits() != 256 ||
          TargetTy->getPrimitiveSizeInBits() != 128 ||
          !SourceTy->getElementType()->isIntegerTy() ||
          SourceTy->getElementType()->getScalarSizeInBits() < 8 ||
          SourceTy->getElementType()->getScalarSizeInBits() > 64 ||
          SourceTy->getElementType() != TargetTy->getElementType() ||
          TargetTy->getNumElements() * 2 != SourceTy->getNumElements() ||
          !HalfView->hasOneUse() || *HalfView->user_begin() != Extract ||
          !Extract->hasOneUse() || *Extract->user_begin() != Back)
        continue;

      auto *Source = dyn_cast<Instruction>(HalfView->getOperand(0));
      if (!Source || Source->getParent() != &BB)
        continue;
      SmallVector<int, 8> Mask;
      unsigned BaseLane =
          static_cast<unsigned>(Index->getZExtValue()) * TargetTy->getNumElements();
      for (unsigned Lane = 0; Lane < TargetTy->getNumElements(); ++Lane)
        Mask.push_back(static_cast<int>(BaseLane + Lane));
      auto *Shuffle = new ShuffleVectorInst(
          HalfView->getOperand(0), PoisonValue::get(SourceTy), Mask,
          Back->getName() + ".half", Back->getIterator());
      inheritMeta(Back, Shuffle);
      Back->replaceAllUsesWith(Shuffle);
      Back->eraseFromParent();
      Extract->eraseFromParent();
      HalfView->eraseFromParent();
      ++Changed;
    }
  }
  return Changed;
}

} // namespace

unsigned explodeStraight(Function &F) {
  unsigned Changed = promoteI128ExtractHalves(F);
  // Pin vector defs used outside their home block: their home block must
  // never erase them, because a later-processed block may read their lanes
  // through extracts (see lanesOf). Computed on entry; explosion never
  // creates new vector uses, so the set stays complete.
  DenseSet<const Value *> Pinned;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      if (!isa<FixedVectorType>(I.getType()) || I.use_empty())
        continue;
      for (const Use &U : I.uses()) {
        auto *UI = dyn_cast<Instruction>(U.getUser());
        if (UI && UI->getParent() != &BB) {
          Pinned.insert(&I);
          break;
        }
      }
    }
  SmallVector<BasicBlock *, 16> Order;
  for (BasicBlock &BB : F)
    Order.push_back(&BB);
  for (BasicBlock *BB : Order) {
    // No vector PHIs (loops own them).
    bool HasVecPhi = false;
    for (PHINode &P : BB->phis()) {
      unsigned L = 0;
      Type *E = nullptr;
      if (vecParts(P.getType(), L, E)) {
        HasVecPhi = true;
        break;
      }
    }
    if (HasVecPhi)
      continue;
    // Terminator-read vectors pin the block (like lane0's shrink).
    bool TermPinned = false;
    for (Value *Op : BB->getTerminator()->operands()) {
      unsigned L = 0;
      Type *E = nullptr;
      if (vecParts(Op->getType(), L, E)) {
        TermPinned = true;
        break;
      }
    }
    if (TermPinned)
      continue;
    SmallVector<Instruction *, 32> Emitted;
    if (explodeBlock(F, BB, Changed, Emitted, Pinned))
      ++Changed;
    else
      rollbackEmitted(Emitted);
  }

  // Sweep newly-dead pure vector producers left behind (fixpoint).
  bool Progress = true;
  while (Progress) {
    Progress = false;
    SmallVector<Instruction *, 8> Dead;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        if (!I.use_empty())
          continue;
        unsigned L = 0;
        Type *E = nullptr;
        if (!vecParts(I.getType(), L, E))
          continue;
        if (isa<BinaryOperator>(&I) || isa<CastInst>(&I) ||
            isa<LoadInst>(&I) || isa<SelectInst>(&I) ||
            isa<ExtractElementInst>(&I) || isa<InsertElementInst>(&I) ||
            isa<ShuffleVectorInst>(&I) || isa<PHINode>(&I) ||
            isa<GetElementPtrInst>(&I)) {
          Dead.push_back(&I);
        } else if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (auto *II = dyn_cast<IntrinsicInst>(CI))
            if (II->getIntrinsicID() != Intrinsic::not_intrinsic)
              Dead.push_back(&I);
        }
      }
    for (Instruction *I : Dead) {
      I->eraseFromParent();
      ++Changed;
      Progress = true;
    }
  }

  return Changed;
}

} // namespace straight
} // namespace revenant
