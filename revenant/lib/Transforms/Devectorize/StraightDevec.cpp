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
// New values use direct Create/new calls into their block (no IRBuilder);
// float lane constants are UndefValue (ConstantFP::get faults the APFloat
// printer under the plugin build).
//
//===----------------------------------------------------------------------===//

#include "StraightDevec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
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

Value *laneConst(Constant *C, Type *Elem, unsigned Lane) {
  if (isa<UndefValue>(C))
    return UndefValue::get(Elem);
  if (isa<ConstantAggregateZero>(C)) {
    if (Elem->isFloatingPointTy())
      return UndefValue::get(Elem);
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
      return nullptr;
    }
  }
  if (Elem->isFloatingPointTy())
    return UndefValue::get(Elem);
  if (auto *CI = dyn_cast<ConstantInt>(C))
    return ConstantInt::get(
        Elem, CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits()));
  return nullptr;
}

// Per-block explosion state.
struct BlockCx {
  BasicBlock *BB;
  // Vector value -> its lane scalars (only for same-block defs exploded
  // at their own position, plus constants handled inline).
  DenseMap<const Value *, SmallVector<Value *, 4>> Lanes;
  // Scalar aliases for killed scalar defs (extract dsts).
  DenseMap<const Value *, Value *> Subs;
  SmallVector<Instruction *, 16> Kill;
  SmallVector<Instruction *, 32> Emitted;
  unsigned Changed = 0;

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
    if (It == Lanes.end() || It->second.size() != NeedLanes)
      return false;
    Out.append(It->second.begin(), It->second.end());
    return true;
  }
};

bool explodeBlock(Function &F, BasicBlock *BB, unsigned &ChangedOut,
                    SmallVectorImpl<Instruction *> &EmittedOut) {
  BlockCx Cx;
  Cx.BB = BB;
  LLVMContext &Ctx = BB->getContext();
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);

  SmallVector<Instruction *, 16> Insns;
  for (Instruction &I : *BB)
    Insns.push_back(&I);

  // Failure inside the walk: roll back what THIS block emitted so far.
  // Emitted lane code is use-free on this path (rewire runs only after
  // the full walk), so reverse-erase is safe.
  SmallVector<Instruction *, 32> LocalEmitted;
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
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(LI->getType(), L, E))
        continue;
      if (LI->isVolatile() || !LI->isSimple())
        return Fail();
      unsigned Esz = E->getScalarSizeInBits() / 8;
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        auto *Off = ConstantInt::get(I64, (uint64_t)(j * Esz));
        auto *G = GetElementPtrInst::Create(
            I8, LI->getPointerOperand(), Off, LI->getName() + ".p",
            LI->getIterator());
        inheritMeta(LI, G);
        auto *Ln = new LoadInst(E, G, LI->getName() + ".l",
                                LI->isVolatile(), LI->getIterator());
        inheritMeta(LI, Ln);
        LocalEmitted.push_back(G);
        LocalEmitted.push_back(Ln);
        ++Cx.Changed;
        Ls.push_back(Ln);
      }
      Cx.Lanes[LI] = Ls;
      Cx.Kill.push_back(LI);
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
    if (auto *BI = dyn_cast<BitCastInst>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (!vecParts(BI->getDestTy(), L, E))
        continue;
      if (BI->getDestTy() != BI->getSrcTy())
        return Fail();
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
      SmallVector<Value *, 4> AS, BS;
      if (!Cx.lanesOf(SV->getOperand(0), L, E, AS))
        return Fail();
      if (!Cx.lanesOf(SV->getOperand(1), L, E, BS))
        return Fail();
      SmallVector<Value *, 4> Ls;
      for (unsigned j = 0; j < L; ++j) {
        int M = SV->getMaskValue(j);
        if (M < 0)
          Ls.push_back(UndefValue::get(E));
        else if ((unsigned)M < L)
          Ls.push_back(AS[M]);
        else if ((unsigned)M - L < L)
          Ls.push_back(BS[M - L]);
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
      for (unsigned j = 0; j < L; ++j) {
        auto *Off = ConstantInt::get(I64, (uint64_t)(j * Esz));
        auto *G = GetElementPtrInst::Create(I8, SI->getPointerOperand(),
                                            Off, SI->getName() + ".p",
                                            SI->getIterator());
        inheritMeta(SI, G);
        auto *NS = new StoreInst(AS[j], G, SI->isVolatile(), SI->getAlign(),
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
    unsigned IL = 0;
    Type *IE = nullptr;
    if (isa<InsertElementInst>(&I) && vecParts(I.getType(), IL, IE))
      return Fail();
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
  for (Instruction *K : Cx.Kill) {
    // Scalar aliases (extracts) may be used outside; vector defs may not.
    if (Cx.Subs.contains(K))
      continue;
    for (const Use &U : K->uses()) {
      auto *UI = dyn_cast<Instruction>(U.getUser());
      if (!UI || UI->getParent() != BB)
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
  // Aliases flow forward only (layout order after BB).
  bool Past = false;
  for (BasicBlock &JB : F) {
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

} // namespace

unsigned explodeStraight(Function &F) {
  unsigned Changed = 0;
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
    if (explodeBlock(F, BB, Changed, Emitted))
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
