//===- LiftNormalize.cpp - machine state to compiler IR ------------------===//
#include "revenant/Transforms/LiftNormalize/LiftNormalize.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
using namespace llvm;
namespace revenant {
namespace {
void provenance(Value *V, const Instruction &From) {
  if (auto *I = dyn_cast<Instruction>(V)) {
    for (StringRef Kind : {"revenant.va", "revenant.opaque", "revenant.region"})
      if (MDNode *MD = From.getMetadata(Kind))
        I->setMetadata(Kind, MD);
    I->setDebugLoc(From.getDebugLoc());
  }
}
struct Access {
  Instruction *I;
  int64_t Offset;
  uint64_t Size;
  Type *Ty;
};
// The lifter has already resolved StackPtr's instruction-dependent deltas
// into constant byte GEPs. Only split a closed, nonescaping frame. Replacing
// an escaped byte range with independent objects would change aliasing.
bool collectFrame(Value *P, int64_t Offset, const DataLayout &DL,
                  SmallVectorImpl<Access> &Accesses,
                  SmallPtrSetImpl<Value *> &Seen) {
  if (!Seen.insert(P).second)
    return false;
  for (User *U : P->users()) {
    if (auto *G = dyn_cast<GetElementPtrInst>(U)) {
      APInt O(DL.getIndexTypeSizeInBits(G->getType()), 0);
      if (!G->accumulateConstantOffset(DL, O))
        return false;
      int64_t Next;
      if (AddOverflow(Offset, O.getSExtValue(), Next) ||
          !collectFrame(G, Next, DL, Accesses, Seen))
        return false;
      continue;
    }
    Type *Ty = nullptr;
    if (auto *L = dyn_cast<LoadInst>(U)) {
      if (L->getPointerOperand() != P || L->isVolatile() || L->isAtomic())
        return false;
      Ty = L->getType();
    } else if (auto *S = dyn_cast<StoreInst>(U)) {
      if (S->getPointerOperand() != P || S->getValueOperand() == P ||
          S->isVolatile() || S->isAtomic())
        return false;
      Ty = S->getValueOperand()->getType();
    } else {
      return false;
    }
    TypeSize Size = DL.getTypeStoreSize(Ty);
    if (Size.isScalable() || !Size.getFixedValue())
      return false;
    Accesses.push_back({cast<Instruction>(U), Offset, Size.getFixedValue(), Ty});
  }
  return true;
}
bool splitFrame(AllocaInst &Frame, const DataLayout &DL) {
  auto *AT = dyn_cast<ArrayType>(Frame.getAllocatedType());
  if (!AT || !AT->getElementType()->isIntegerTy(8) ||
      !Frame.getName().contains("frame") || !Frame.isStaticAlloca())
    return false;
  SmallVector<Access, 32> A;
  SmallPtrSet<Value *, 32> Seen;
  if (!collectFrame(&Frame, 0, DL, A, Seen) || A.empty())
    return false;
  for (const Access &X : A)
    if (X.Offset < 0 || uint64_t(X.Offset) > AT->getNumElements() ||
        X.Size > AT->getNumElements() - uint64_t(X.Offset))
      return false;
  llvm::sort(A, [](const Access &X, const Access &Y) {
    return X.Offset < Y.Offset;
  });
  IRBuilder<> Entry(&Frame);
  for (size_t Begin = 0; Begin < A.size();) {
    size_t End = Begin + 1;
    uint64_t Limit = uint64_t(A[Begin].Offset) + A[Begin].Size;
    while (End < A.size() && uint64_t(A[End].Offset) < Limit) {
      Limit = std::max(Limit, uint64_t(A[End].Offset) + A[End].Size);
      ++End;
    }
    // Overlapping accesses remain one byte object. Exact, consistently typed
    // slots become scalar allocas, ready for mem2reg. Do not invent structs.
    Type *Ty = A[Begin].Ty;
    for (size_t J = Begin; J < End; ++J)
      if (A[J].Offset != A[Begin].Offset || A[J].Ty != Ty)
        Ty = nullptr;
    if (!Ty)
      Ty = ArrayType::get(Entry.getInt8Ty(), Limit - A[Begin].Offset);
    auto *Slot = Entry.CreateAlloca(Ty, nullptr,
                                  "stack_" + Twine(A[Begin].Offset));
    Slot->setAlignment(Frame.getAlign());
    provenance(Slot, Frame);
    for (size_t J = Begin; J < End; ++J) {
      IRBuilder<> B(A[J].I);
      Value *P = Slot;
      if (A[J].Offset != A[Begin].Offset)
        P = B.CreateGEP(B.getInt8Ty(), Slot,
                       B.getInt64(A[J].Offset - A[Begin].Offset));
      provenance(P, *A[J].I);
      if (auto *L = dyn_cast<LoadInst>(A[J].I))
        L->setOperand(0, P);
      else
        cast<StoreInst>(A[J].I)->setOperand(1, P);
    }
    Begin = End;
  }
  // Delete the old address tree bottom-up once all memory users moved.
  SmallVector<Instruction *, 32> Dead;
  for (Value *V : Seen)
    if (V != &Frame)
      Dead.push_back(cast<Instruction>(V));
  while (!Dead.empty()) {
    bool Progress = false;
    for (size_t J = 0; J < Dead.size();)
      if (Dead[J]->use_empty()) {
        Dead[J]->eraseFromParent();
        Dead.erase(Dead.begin() + J);
        Progress = true;
      } else {
        ++J;
      }
    if (!Progress)
      break;
  }
  if (Frame.use_empty())
    Frame.eraseFromParent();
  return true;
}
// Integral pointers have the same modular address arithmetic as machine
// registers. Never add inbounds/no-wrap: machine arithmetic can wrap.
Value *pointerFor(Value *V, Type *PT, IRBuilder<> &B, const DataLayout &DL,
                  DenseMap<Value *, PHINode *> &PointerPhis,
                  unsigned Depth = 0) {
  if (Depth > 16 || !V->getType()->isIntegerTy(DL.getPointerTypeSizeInBits(PT)))
    return nullptr;
  // PtrToIntOperator also covers the folded ConstantExpr form, so a
  // constant `ptrtoint @g` inedge unwraps to the global directly.
  if (auto *P = dyn_cast<PtrToIntOperator>(V))
    return P->getPointerOperand()->getType() == PT ? P->getPointerOperand()
                                                 : nullptr;
  if (auto *P = dyn_cast<PtrToAddrOperator>(V))
    return P->getPointerOperand()->getType() == PT ? P->getPointerOperand()
                                                 : nullptr;
  // No other constant is pointer-derivable: lifting knows nothing about
  // absolute values, so wrapping a bare ConstantInt sentinel (-1, -2,
  // whatever the register held) in inttoptr invents a pointer from nothing
  // and hands downstream passes a ConstantExpr they must treat as opaque.
  // Refuse; the caller keeps the value integer-typed instead.
  if (isa<Constant>(V))
    return nullptr;
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    auto It = PointerPhis.find(Phi);
    if (It != PointerPhis.end())
      return It->second;
    for (unsigned J = 0; J < Phi->getNumIncomingValues(); ++J) {
      BasicBlock *Pred = Phi->getIncomingBlock(J);
      if (isa<InvokeInst>(Pred->getTerminator()) || Pred->isEHPad())
        return nullptr;
      // A constant inedge must already hold a pointer-derived value
      // (ptrtoint of a symbol). Anything else, most often a -1 sentinel,
      // would arrive here only through the inttoptr fallback, which folds
      // to a ConstantExpr. The integer phi keeps the original semantics,
      // so decline before P exists: an abandoned phi could not be erased
      // once a cyclic partner already references it.
      if (isa<Constant>(Phi->getIncomingValue(J)) &&
          !pointerFor(Phi->getIncomingValue(J), PT, B, DL, PointerPhis,
                      Depth + 1))
        return nullptr;
    }
    auto *P = PHINode::Create(PT, Phi->getNumIncomingValues(),
                             Phi->getName() + ".ptr", Phi->getIterator());
    PointerPhis[Phi] = P;
    provenance(P, *Phi);
    for (unsigned J = 0; J < Phi->getNumIncomingValues(); ++J) {
      BasicBlock *Pred = Phi->getIncomingBlock(J);
      int Prior = P->getBasicBlockIndex(Pred);
      if (Prior >= 0) {
        P->addIncoming(P->getIncomingValue(Prior), Pred);
        continue;
      }
      IRBuilder<> Edge(Pred->getTerminator());
      Value *Incoming = Phi->getIncomingValue(J);
      Value *Ptr = pointerFor(Incoming, PT, Edge, DL, PointerPhis, Depth + 1);
      if (!Ptr) {
        if (isa<Constant>(Incoming)) {
          // Unreachable per the inedge scan; keep a non-folding cast so a
          // contract drift can never smuggle in a ConstantExpr.
          Ptr = CastInst::Create(Instruction::IntToPtr, Incoming, PT,
                                 Incoming->getName() + ".ptr",
                                 Pred->getTerminator()->getIterator());
        } else {
          Ptr = Edge.CreateIntToPtr(Incoming, PT);
        }
        provenance(Ptr, *Phi);
      }
      P->addIncoming(Ptr, Pred);
    }
    return P;
  }
  auto *Op = dyn_cast<BinaryOperator>(V);
  if (!Op || (Op->getOpcode() != Instruction::Add &&
              Op->getOpcode() != Instruction::Sub) ||
      Op->hasNoUnsignedWrap() || Op->hasNoSignedWrap())
    return nullptr;
  Value *Base = pointerFor(Op->getOperand(0), PT, B, DL, PointerPhis, Depth + 1);
  Value *Offset = Op->getOperand(1);
  if (!Base && Op->getOpcode() == Instruction::Add) {
    Base = pointerFor(Op->getOperand(1), PT, B, DL, PointerPhis, Depth + 1);
    Offset = Op->getOperand(0);
  }
  if (!Base) {
    // A constant first operand is not pointer-derivable either; converting
    // it here would only restate the value as an inttoptr ConstantExpr.
    if (isa<Constant>(Op->getOperand(0)))
      return nullptr;
    Base = B.CreateIntToPtr(Op->getOperand(0), PT, "base.ptr");
    provenance(Base, *Op);
    Offset = Op->getOperand(1);
  }
  if (Op->getOpcode() == Instruction::Sub) {
    Offset = B.CreateNeg(Offset);
    provenance(Offset, *Op);
  }
  Value *G = B.CreateGEP(B.getInt8Ty(), Base, Offset, "address");
  provenance(G, *Op);
  return G;
}
// Load/store types are the same evidence TypeInference collects for DI.
// A byte displacement may become an element index only when exactly divisible.
bool typeGEP(GetElementPtrInst &G, const DataLayout &DL) {
  if (!G.getSourceElementType()->isIntegerTy(8) || G.getNumIndices() != 1)
    return false;
  Type *Ty = nullptr;
  for (User *U : G.users()) {
    Type *T = nullptr;
    if (auto *L = dyn_cast<LoadInst>(U))
      T = L->getType();
    else if (auto *S = dyn_cast<StoreInst>(U)) {
      if (S->getPointerOperand() != &G)
        return false;
      T = S->getValueOperand()->getType();
    } else
      return false;
    if (Ty && Ty != T)
      return false;
    Ty = T;
  }
  if (!Ty || Ty->isIntegerTy(8))
    return false;
  TypeSize Size = DL.getTypeAllocSize(Ty);
  if (Size.isScalable() || !Size.getFixedValue())
    return false;
  uint64_t Bytes = Size.getFixedValue();
  Value *Index = G.getOperand(1);
  if (auto *C = dyn_cast<ConstantInt>(Index)) {
    if (C->getSExtValue() % int64_t(Bytes))
      return false;
    Index = ConstantInt::get(Index->getType(), C->getSExtValue() / int64_t(Bytes));
  } else if (auto *Op = dyn_cast<BinaryOperator>(Index)) {
    if (G.getNoWrapFlags() != GEPNoWrapFlags::none() ||
        (Op->getOpcode() != Instruction::Mul &&
         Op->getOpcode() != Instruction::Shl) ||
        !Index->getType()->isIntegerTy(DL.getIndexTypeSizeInBits(G.getType())) ||
        Op->hasNoUnsignedWrap() || Op->hasNoSignedWrap())
      return false;
    auto *C = dyn_cast<ConstantInt>(Op->getOperand(1));
    if (!C)
      return false;
    if (Op->getOpcode() == Instruction::Mul && C->equalsInt(Bytes))
      Index = Op->getOperand(0);
    else if (Op->getOpcode() == Instruction::Shl && C->getZExtValue() < 63 &&
             (uint64_t(1) << C->getZExtValue()) == Bytes)
      Index = Op->getOperand(0);
    else
      return false;
  } else
    return false;
  IRBuilder<> B(&G);
  Value *Typed = B.CreateGEP(Ty, G.getPointerOperand(), Index, "element");
  if (auto *TG = dyn_cast<GetElementPtrInst>(Typed))
    TG->setNoWrapFlags(G.getNoWrapFlags());
  provenance(Typed, G);
  G.replaceAllUsesWith(Typed);
  G.eraseFromParent();
  return true;
}
bool narrow(TruncInst &T) {
  auto *Op = dyn_cast<BinaryOperator>(T.getOperand(0));
  if (!Op || !Op->getType()->isIntegerTy(64))
    return false;
  switch (Op->getOpcode()) {
  case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
    if (Op->hasNoUnsignedWrap() || Op->hasNoSignedWrap())
      return false;
    break;
  case Instruction::And: case Instruction::Or: case Instruction::Xor:
    break;
  default:
    return false;
  }
  // Low N bits of modular arithmetic depend only on low N operand bits.
  // Flags must not carry over: wide nsw/nuw says nothing about narrow overflow.
  IRBuilder<> B(&T);
  Value *L = B.CreateTrunc(Op->getOperand(0), T.getType());
  Value *R = B.CreateTrunc(Op->getOperand(1), T.getType());
  provenance(L, *Op);
  provenance(R, *Op);
  Value *V = B.CreateBinOp(Instruction::BinaryOps(Op->getOpcode()), L, R,
                         "narrow");
  provenance(V, T);
  T.replaceAllUsesWith(V);
  T.eraseFromParent();
  return true;
}
bool narrowPhi(PHINode &P) {
  if (!P.getType()->isIntegerTy(64))
    return false;
  unsigned Extension = 0;
  for (Value *V : P.incoming_values()) {
    if (V == &P || isa<ConstantInt>(V))
      continue;
    auto *C = dyn_cast<CastInst>(V);
    if (!C || !C->getSrcTy()->isIntegerTy(32) ||
        (C->getOpcode() != Instruction::ZExt &&
         C->getOpcode() != Instruction::SExt))
      return false;
    if (Extension && Extension != C->getOpcode())
      return false;
    Extension = C->getOpcode();
  }
  if (!Extension)
    return false;
  for (Value *V : P.incoming_values())
    if (auto *C = dyn_cast<ConstantInt>(V))
      if ((Extension == Instruction::ZExt && !C->getValue().isIntN(32)) ||
          (Extension == Instruction::SExt && !C->getValue().isSignedIntN(32)))
        return false;
  auto *N = PHINode::Create(Type::getInt32Ty(P.getContext()),
                            P.getNumIncomingValues(), P.getName() + ".i32",
                            P.getIterator());
  provenance(N, P);
  for (unsigned J = 0; J < P.getNumIncomingValues(); ++J) {
    Value *V = P.getIncomingValue(J);
    if (V == &P)
      V = N;
    else if (auto *C = dyn_cast<ConstantInt>(V))
      V = ConstantInt::get(N->getType(), C->getValue().trunc(32));
    else
      V = cast<CastInst>(V)->getOperand(0);
    N->addIncoming(V, P.getIncomingBlock(J));
  }
  IRBuilder<> B(&*P.getParent()->getFirstInsertionPt());
  Value *Wide = B.CreateCast(Instruction::CastOps(Extension), N, P.getType());
  provenance(Wide, P);
  P.replaceAllUsesWith(Wide);
  P.eraseFromParent();
  return true;
}
// Integer phi -> pointer phi of the same block that holds the same address
// on every edge. Lifting gives a register that holds a pointer across a
// loop a pointer phi next to its integer phi (`rax.0.ptr` next to `rax.0`)
// while the integer one still feeds a compare, and both step every
// iteration (`add i64 %rax.0, 8` and `getelementptr i8, ptr %rax.0.ptr, 8`).
using TwinPairs = DenseSet<std::pair<PHINode *, PHINode *>>;

// Whether integer A and pointer B hold the same address, given the twin
// pairs: B converts A, A converts B, both are twin phis, or both step
// same-address bases by one byte offset.
bool sameAddress(Value *A, Value *B, const TwinPairs &Pairs,
                 const DataLayout &DL, unsigned Depth = 0) {
  if (auto *IP = dyn_cast<PHINode>(A))
    if (auto *PP = dyn_cast<PHINode>(B))
      return Pairs.count({IP, PP});
  if (auto *C = dyn_cast<IntToPtrInst>(B))
    if (C->getOperand(0) == A)
      return true;
  if (auto *P = dyn_cast<PtrToIntOperator>(A))
    if (P->getPointerOperand() == B)
      return true;
  if (Depth > 4)
    return false;
  auto *Step = dyn_cast<BinaryOperator>(A);
  auto *G = dyn_cast<GEPOperator>(B);
  if (!Step || !G || Step->getOpcode() != Instruction::Add)
    return false;
  for (unsigned K = 0; K < 2; ++K) {
    Value *Offset = Step->getOperand(1 - K);
    if (!sameAddress(Step->getOperand(K), G->getPointerOperand(), Pairs, DL,
                     Depth + 1))
      continue;
    // `getelementptr i8, ptr B, Offset`, or a typed form with the same
    // constant byte offset.
    if (G->getNumIndices() == 1 && G->getSourceElementType()->isIntegerTy(8) &&
        G->getOperand(1) == Offset)
      return true;
    APInt Bytes(DL.getIndexTypeSizeInBits(G->getType()), 0);
    auto *CI = dyn_cast<ConstantInt>(Offset);
    if (CI && G->accumulateConstantOffset(DL, Bytes) &&
        Bytes.getSExtValue() == CI->getSExtValue())
      return true;
  }
  return false;
}

// The twin pairs of F: every pointer-sized integer phi with uses against
// the pointer phis of its block (only the one this run built, when there
// is one), then pairs whose edges disagree are dropped until the remaining
// pairs hold each other up (twin phis feed twin phis across blocks).
MapVector<PHINode *, PHINode *>
findTwins(Function &F, const DataLayout &DL,
          const DenseMap<Value *, PHINode *> &PointerPhis) {
  SmallVector<std::pair<PHINode *, PHINode *>, 16> Order;
  TwinPairs Pairs;
  for (BasicBlock &BB : F) {
    SmallVector<PHINode *, 8> Ptrs;
    for (PHINode &Phi : BB.phis())
      if (Phi.getType()->isPointerTy() &&
          Phi.getType()->getPointerAddressSpace() == 0)
        Ptrs.push_back(&Phi);
    if (Ptrs.empty())
      continue;
    for (PHINode &Phi : BB.phis()) {
      if (Phi.use_empty() || !Phi.getType()->isIntegerTy(
                                 DL.getPointerTypeSizeInBits(Ptrs[0]->getType())))
        continue;
      if (PHINode *Own = PointerPhis.lookup(&Phi)) {
        Order.push_back({&Phi, Own});
        continue;
      }
      for (PHINode *P : Ptrs)
        if (P->getNumIncomingValues() == Phi.getNumIncomingValues())
          Order.push_back({&Phi, P});
    }
  }
  Pairs.insert(Order.begin(), Order.end());
  auto EdgesAgree = [&](PHINode *I, PHINode *P) {
    for (unsigned J = 0; J < I->getNumIncomingValues(); ++J) {
      int K = P->getBasicBlockIndex(I->getIncomingBlock(J));
      if (K < 0 || !sameAddress(I->getIncomingValue(J), P->getIncomingValue(K),
                                Pairs, DL))
        return false;
    }
    return true;
  };
  for (bool Dropped = true; Dropped;) {
    Dropped = false;
    for (auto &Pair : Order)
      if (Pairs.count(Pair) && !EdgesAgree(Pair.first, Pair.second)) {
        Pairs.erase(Pair);
        Dropped = true;
      }
  }
  MapVector<PHINode *, PHINode *> Twins;
  for (auto &Pair : Order)
    if (Pairs.count(Pair))
      Twins.insert(Pair);
  return Twins;
}

// Integer twin webs go when nothing but compares reads them: the compares
// take the pointers, so a loop keeps one pointer variable, as the source
// has. The web is the twin phis plus `add`/`sub` steps of a twin phi by
// an offset. A member stays while any reader outside the web is not a
// compare that can take a pointer (a store, a call, a constant bound).
bool mergeTwins(Function &F, const DataLayout &DL,
                DenseMap<Value *, PHINode *> &PointerPhis) {
  MapVector<PHINode *, PHINode *> Twins = findTwins(F, DL, PointerPhis);
  if (Twins.empty())
    return false;
  // Step -> the twin phi it steps, its offset, and the pointer twin the
  // step already has when it feeds a twin phi (that phi's incoming).
  struct StepOf {
    PHINode *Base;
    Value *Offset;
    Value *Twin = nullptr;
  };
  MapVector<Instruction *, StepOf> Steps;
  for (auto &[I, P] : Twins)
    for (User *U : I->users()) {
      auto *S = dyn_cast<BinaryOperator>(U);
      if (!S || (S->getOpcode() != Instruction::Add &&
                 S->getOpcode() != Instruction::Sub))
        continue;
      Value *Offset = S->getOperand(S->getOperand(0) == I ? 1 : 0);
      if (S->getOpcode() == Instruction::Sub && S->getOperand(0) != I)
        continue;
      auto *OffsetPhi = dyn_cast<PHINode>(Offset);
      if (Offset == I || (OffsetPhi && Twins.count(OffsetPhi)))
        continue;
      Steps.insert({S, {I, Offset}});
    }
  for (auto &[I, P] : Twins)
    for (unsigned J = 0; J < I->getNumIncomingValues(); ++J)
      if (auto *S = dyn_cast<Instruction>(I->getIncomingValue(J))) {
        auto It = Steps.find(S);
        if (It != Steps.end() && !It->second.Twin)
          It->second.Twin = P->getIncomingValueForBlock(I->getIncomingBlock(J));
      }
  auto IsMember = [&](Value *V) {
    auto *Phi = dyn_cast<PHINode>(V);
    return (Phi && Twins.count(Phi)) ||
           (isa<Instruction>(V) && Steps.count(cast<Instruction>(V)));
  };
  // A compare can take pointers unless one side is a constant other than
  // null: that would invent a pointer.
  auto PointerCompare = [&](User *U) {
    auto *C = dyn_cast<ICmpInst>(U);
    if (!C)
      return false;
    for (Value *Op : C->operands())
      if (!IsMember(Op) && isa<Constant>(Op) && !isa<PtrToIntOperator>(Op) &&
          !(isa<ConstantInt>(Op) && cast<ConstantInt>(Op)->isZero()))
        return false;
    return true;
  };
  SetVector<Instruction *> Dead;
  for (auto &[I, P] : Twins)
    Dead.insert(I);
  for (auto &[S, Of] : Steps)
    Dead.insert(S);
  for (bool Dropped = true; Dropped;) {
    SmallVector<Instruction *, 8> Drop;
    for (Instruction *M : Dead)
      if (any_of(M->users(), [&](User *U) {
            return !Dead.count(cast<Instruction>(U)) && !PointerCompare(U);
          }))
        Drop.push_back(M);
    for (Instruction *M : Drop)
      Dead.remove(M);
    Dropped = !Drop.empty();
  }
  if (Dead.empty())
    return false;
  // The pointer form of member M at compare C. A twin phi sits next to its
  // integer phi, which dominates C. A step reuses its twin when that is
  // before C or can move up to C (it was built at the end of its block),
  // and is rebuilt from its base's twin otherwise.
  auto Available = [&](Value *Twin, ICmpInst *C) {
    auto *TI = dyn_cast<Instruction>(Twin);
    if (!TI)
      return true;
    if (TI->getParent() != C->getParent() || isa<PHINode>(TI))
      return false;
    if (TI->comesBefore(C))
      return true;
    for (Value *Op : TI->operands())
      if (auto *OI = dyn_cast<Instruction>(Op))
        if (OI->getParent() == C->getParent() && !isa<PHINode>(OI) &&
            !OI->comesBefore(C))
          return false;
    TI->moveBefore(C->getIterator());
    return true;
  };
  auto TwinAt = [&](Value *M, ICmpInst *C) -> Value * {
    if (auto *Phi = dyn_cast<PHINode>(M))
      return Twins.lookup(Phi);
    const StepOf &Of = Steps.find(cast<Instruction>(M))->second;
    if (Of.Twin && Available(Of.Twin, C))
      return Of.Twin;
    IRBuilder<> B(C);
    Value *Offset = Of.Offset;
    if (cast<BinaryOperator>(M)->getOpcode() == Instruction::Sub)
      Offset = B.CreateNeg(Offset);
    Value *G = B.CreateGEP(B.getInt8Ty(), Twins.lookup(Of.Base), Offset,
                           "address");
    provenance(Offset, *cast<Instruction>(M));
    provenance(G, *cast<Instruction>(M));
    return G;
  };
  SmallVector<ICmpInst *, 8> Cmps;
  for (Instruction *M : Dead)
    for (User *U : M->users())
      if (auto *C = dyn_cast<ICmpInst>(U))
        if (!is_contained(Cmps, C))
          Cmps.push_back(C);
  for (ICmpInst *C : Cmps) {
    Type *PT = Twins.front().second->getType();
    IRBuilder<> B(C);
    Value *Ops[2];
    for (unsigned K = 0; K < 2; ++K) {
      Value *Op = C->getOperand(K);
      if (IsMember(Op)) {
        Ops[K] = TwinAt(Op, C);
      } else if (isa<ConstantInt>(Op)) {
        Ops[K] = ConstantPointerNull::get(cast<PointerType>(PT));
      } else if (Value *Ptr = pointerFor(Op, PT, B, DL, PointerPhis)) {
        Ops[K] = Ptr;
      } else {
        Ops[K] = B.CreateIntToPtr(Op, PT, Op->getName() + ".ptr");
        provenance(Ops[K], *C);
      }
    }
    Value *New = B.CreateICmp(C->getPredicate(), Ops[0], Ops[1]);
    provenance(New, *C);
    New->takeName(C);
    C->replaceAllUsesWith(New);
    C->eraseFromParent();
  }
  for (Instruction *M : Dead)
    M->replaceAllUsesWith(PoisonValue::get(M->getType()));
  for (Instruction *M : Dead) {
    PointerPhis.erase(M);
    M->eraseFromParent();
  }
  return true;
}
} // namespace
PreservedAnalyses LiftNormalizePass::run(Function &F,
                                         FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  const DataLayout &DL = F.getDataLayout();
  bool Changed = false;
  DenseMap<Value *, PHINode *> PointerPhis;
  SmallVector<AllocaInst *, 16> Frames;
  for (Instruction &I : F.getEntryBlock())
    if (auto *A = dyn_cast<AllocaInst>(&I))
      Frames.push_back(A);
  for (AllocaInst *A : Frames)
    Changed |= splitFrame(*A, DL);
  for (Instruction &I : make_early_inc_range(instructions(F)))
    if (auto *P = dyn_cast<PHINode>(&I))
      Changed |= narrowPhi(*P);
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    if (auto *P = dyn_cast<IntToPtrInst>(&I)) {
      if (DL.isNonIntegralPointerType(P->getType()))
        continue;
      IRBuilder<> B(P);
      if (Value *V = pointerFor(P->getOperand(0), P->getType(), B, DL,
                                PointerPhis)) {
        P->replaceAllUsesWith(V);
        P->eraseFromParent();
        Changed = true;
      }
    } else if (auto *T = dyn_cast<TruncInst>(&I))
      Changed |= narrow(*T);
  }
  // Twins this run made, and those an earlier run left (cleanup runs this
  // pass three times).
  Changed |= mergeTwins(F, DL, PointerPhis);
  // A register phi whose value only feeds its own step (`rax.0` and
  // `add i64 %rax.0, 8` after its address uses moved to the pointer twin)
  // is a dead cycle that DCE keeps; storage passes would give it a local.
  SmallVector<WeakTrackingVH, 16> Phis;
  for (BasicBlock &BB : F)
    for (PHINode &Phi : BB.phis())
      Phis.push_back(&Phi);
  for (WeakTrackingVH &VH : Phis)
    if (auto *Phi = dyn_cast_or_null<PHINode>(VH))
      Changed |= RecursivelyDeleteDeadPHINode(Phi);
  for (Instruction &I : make_early_inc_range(instructions(F)))
    if (auto *G = dyn_cast<GetElementPtrInst>(&I))
      Changed |= typeGEP(*G, DL);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // namespace revenant
