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

bool isIntVec128(Type *T, unsigned &Lanes, Type *&Elem) {
  auto *VT = dyn_cast<FixedVectorType>(T);
  if (!VT || !VT->getElementType()->isIntegerTy())
    return false;
  if (VT->getPrimitiveSizeInBits() != 128)
    return false;
  Lanes = VT->getNumElements();
  Elem = VT->getElementType();
  return true;
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
// non-splat ConstantVector fold per lane.
Value *laneConst(Constant *C, Type *Elem, unsigned Lane) {
  if (isa<UndefValue>(C))
    return UndefValue::get(Elem);
  if (isa<ConstantAggregateZero>(C))
    return ConstantInt::get(Elem, 0);
  if (auto *CV = dyn_cast<ConstantVector>(C)) {
    if (Constant *Splat = CV->getSplatValue())
      C = Splat;
    else {
      auto *CI = dyn_cast<ConstantInt>(CV->getOperand(Lane));
      if (!CI)
        return nullptr;
      APInt V = CI->getValue().zextOrTrunc(Elem->getScalarSizeInBits());
      return ConstantInt::get(Elem, V);
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

struct AccPhi {
  PHINode *Phi = nullptr;
  Value *Init = nullptr;
  BasicBlock *InitBB = nullptr;
  Instruction *Back = nullptr; // update binop in H
  Value *Stream = nullptr;     // other operand of Back
};

} // namespace

static unsigned tryLoop(Function &F, BasicBlock *H);

unsigned devectorizeLoops(Function &F) {
  unsigned Changed = 0;
  SmallVector<BasicBlock *, 16> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);
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
    if (!isIntVec128(P.getType(), Lanes, Elem))
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
    if (!isIntVec128(Accs[0].Phi->getType(), L, E))
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
        !isIntVec128(A.Phi->getType(), L, E) || L != Lanes || E != Elem)
      return 0;
  }

  // Per-acc splat init check: every acc init must fold to one Elem lane
  // value. The emit stage seeds each scalar phi from its own acc's init
  // (no cross-acc merging: independent reductions keep independent sums).
  for (auto &A : Accs) {
    if (!splatInit(A.Init, Elem, Lanes))
      return 0;
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
    if (!isIntVec128(BO->getType(), L, E) || L != Lanes || E != Elem)
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
          isIntVec128(BO->getType(), CL, CE) && CL == Lanes && CE == Elem &&
          AccTree(BO->getOperand(0), 0) && AccTree(BO->getOperand(1), 0)) {
        Combines.push_back(BO);
        continue;
      }
      return 0;
    }
  }
  // Reduces over an exit-block combine: the combine lives outside H so the
  // loop above never visits its users. Every combine must feed at least one
  // matching reduce (else the scalar chains would not cover its value), and
  // no other outside use is allowed. (devectorize-mixed-reduce-roots.ll: the
  // second reduce over acc+acc was missed here, left on the severed vector
  // PHI, and returned 16 instead of 24.)
  DenseSet<const Instruction *> SeenReduce;
  for (Instruction *R : Reduces)
    SeenReduce.insert(R);
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

  // Header body precheck (no mutation yet).
  DenseSet<const Instruction *> AccBacks;
  for (auto &A : Accs)
    AccBacks.insert(A.Back);
  for (Instruction &I : *H) {
    if (isa<PHINode>(&I)) {
      unsigned L = 0;
      Type *E = nullptr;
      if (isIntVec128(I.getType(), L, E)) {
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
      if (!isIntVec128(LI->getType(), L, E) || L != Lanes || E != Elem)
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
      if (!isIntVec128(BO->getType(), L, E))
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
    if (AccBacks.contains(&I) || isa<PHINode>(&I))
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
    Value *L0 = splatInit(A.Init, Elem, Lanes);
    if (!L0)
      return 0; // pre-mutation: no scalar PHI created yet, nothing to clean
    Inits.push_back(L0);
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
        for (unsigned j = 0; j < Lanes && Ok; ++j) {
          auto *Off = ConstantInt::get(I64, (uint64_t)(j * Esz));
          auto *G = GetElementPtrInst::Create(
              I8, const_cast<Value *>(LI->getPointerOperand()), Off,
              LI->getName() + ".p", LI->getIterator());
          inheritMeta(LI, G);
          auto *L = new LoadInst(Elem, G, LI->getName() + ".l",
                                 LI->isVolatile(), LI->getIterator());
          inheritMeta(LI, L);
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
    auto AccIndexOf = [&](Value *V) -> int {
      for (unsigned a = 0; a < Accs.size(); ++a)
        if (V == Accs[a].Phi || V == Accs[a].Back)
          return (int)a;
      return -1;
    };
    Value *Repl = nullptr;
    if (!AddL) {
      int Idx = AccIndexOf(Root);
      if (Idx < 0)
        return 0; // precheck promised acc-rooted; defensive
      Repl = Chains[(unsigned)Idx];
    } else {
      // reduce distributes over the lane-wise combine (same opcode, checked
      // in the precheck): reduce(a+b) = reduce(a) OP reduce(b). With both
      // sides on one accumulator (devectorize-mixed-reduce-roots.ll) that is
      // Chain OP Chain, NOT Chain: reduce(x+x) doubles the sum.
      int L = AccIndexOf(AddL), RR = AccIndexOf(AddR);
      if (L < 0 || RR < 0)
        return 0;
      auto *N = BinaryOperator::Create((Instruction::BinaryOps)Opcode,
                                       Chains[(unsigned)L],
                                       Chains[(unsigned)RR], "s.unopt.m",
                                       R->getIterator());
      inheritMeta(R, N);
      Repl = N;
    }
    R->replaceAllUsesWith(Repl);
    if (R->use_empty()) {
      R->eraseFromParent();
      ++NReduce;
    }
  }
  for (BinaryOperator *C : Combines) {
    if (C->use_empty())
      continue;
    // Uses were rewired to Chain above (the combine fed only reduces).
    // Anything still using it blocks erasure; leave it.
    if (C->use_empty()) {
      C->eraseFromParent();
      ++NReduce;
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
      Instruction *K = *It;
      if (K->getParent() == nullptr || !K->use_empty())
        continue;
      K->eraseFromParent();
      Progress = true;
    }
  }
  for (auto &A : Accs) {
    if (A.Phi->getParent() != nullptr && A.Phi->use_empty())
      A.Phi->eraseFromParent();
  }

  return 1 + NReduce;
}

} // namespace loopdevec
} // namespace revenant

// Nested namespace + TU-local using: keep ADL names (std::, llvm::) out of
// the revenant::loopdevec scope so LLVM headers resolve cleanly.
