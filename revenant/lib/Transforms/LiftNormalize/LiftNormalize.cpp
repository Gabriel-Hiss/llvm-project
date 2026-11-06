//===- LiftNormalize.cpp - machine state to compiler IR ------------------===//
#include "revenant/Transforms/LiftNormalize/LiftNormalize.h"
#include "revenant/IR/Contract.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
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
// A use of a frame address that leaves the frame (a call argument, a
// ptrtoint, a stored pointer) at a constant byte offset.
struct Escape {
  Use *U;
  int64_t Offset;
};
// The lifter has already resolved StackPtr's instruction-dependent deltas
// into constant byte GEPs. Without `Escapes` only a closed, nonescaping
// frame qualifies: replacing an escaped byte range with independent objects
// changes aliasing. With it, escaping uses are recorded instead.
bool collectFrame(Value *P, int64_t Offset, const DataLayout &DL,
                  SmallVectorImpl<Access> &Accesses,
                  SmallPtrSetImpl<Value *> &Seen,
                  SmallVectorImpl<Escape> *Escapes = nullptr) {
  if (!Seen.insert(P).second)
    return false;
  for (Use &Use : P->uses()) {
    User *U = Use.getUser();
    if (auto *G = dyn_cast<GetElementPtrInst>(U)) {
      APInt O(DL.getIndexTypeSizeInBits(G->getType()), 0);
      if (!G->accumulateConstantOffset(DL, O))
        return false;
      int64_t Next;
      if (AddOverflow(Offset, O.getSExtValue(), Next) ||
          !collectFrame(G, Next, DL, Accesses, Seen, Escapes))
        return false;
      continue;
    }
    Type *Ty = nullptr;
    if (auto *L = dyn_cast<LoadInst>(U)) {
      if (L->getPointerOperand() != P || L->isVolatile() || L->isAtomic())
        return false;
      Ty = L->getType();
    } else if (auto *S = dyn_cast<StoreInst>(U);
               S && Use.getOperandNo() == StoreInst::getPointerOperandIndex()) {
      if (S->getValueOperand() == P || S->isVolatile() || S->isAtomic())
        return false;
      Ty = S->getValueOperand()->getType();
    } else if (Escapes && !isa<PHINode>(U) && !isa<SelectInst>(U)) {
      Escapes->push_back({&Use, Offset});
      continue;
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
// One object of the split frame: the bytes [Begin, Limit), the accesses
// and escapes that land in them, and its type when it is an array of
// write-only elements.
struct FrameObject {
  uint64_t Begin;
  uint64_t Limit;
  SmallVector<const Access *, 4> Accesses;
  SmallVector<const Escape *, 2> Escapes;
  Type *Ty = nullptr;
};
// Splits the lifted frame into one alloca per object. Overlapping accesses
// stay one byte object; exact, consistently typed slots become scalar
// allocas, ready for mem2reg; no structs are invented. With
// `AllowEscapes`, a frame whose addresses escape splits too: an escaped
// address starts an object that takes the write-only objects directly
// after it (the fields this function initializes before handing the
// object out) and reaches up to the next directly accessed offset (the
// callee may read anything up to there). Escaped objects never overlap.
// The object bounds are inferred, not proven: a callee that reads past the
// next accessed offset, or a field this function also reads, would land
// in another object. Only the ir-final storage tail asks for this.
// `ScalarSlots` receives the scalar allocas no escape reaches; `AllSlots`
// every alloca the split creates.
bool splitFrame(AllocaInst &Frame, const DataLayout &DL,
                bool AllowEscapes = false,
                SmallVectorImpl<AllocaInst *> *ScalarSlots = nullptr,
                SmallVectorImpl<AllocaInst *> *AllSlots = nullptr) {
  auto *AT = dyn_cast<ArrayType>(Frame.getAllocatedType());
  if (!AT || !AT->getElementType()->isIntegerTy(8) ||
      !Frame.getName().contains("frame") || !Frame.isStaticAlloca())
    return false;
  const uint64_t FrameSize = AT->getNumElements();
  SmallVector<Access, 32> A;
  SmallVector<Escape, 8> E;
  SmallPtrSet<Value *, 32> Seen;
  if (!collectFrame(&Frame, 0, DL, A, Seen, AllowEscapes ? &E : nullptr) ||
      (A.empty() && E.empty()))
    return false;
  for (const Access &X : A)
    if (X.Offset < 0 || uint64_t(X.Offset) > FrameSize ||
        X.Size > FrameSize - uint64_t(X.Offset))
      return false;
  for (const Escape &X : E)
    if (X.Offset < 0 || uint64_t(X.Offset) >= FrameSize)
      return false;
  llvm::stable_sort(A, [](const Access &X, const Access &Y) {
    return X.Offset < Y.Offset;
  });
  llvm::stable_sort(E, [](const Escape &X, const Escape &Y) {
    return X.Offset < Y.Offset;
  });
  // Objects from overlapping direct accesses.
  SmallVector<FrameObject, 16> Objects;
  for (const Access &X : A) {
    uint64_t End = uint64_t(X.Offset) + X.Size;
    if (!Objects.empty() && uint64_t(X.Offset) < Objects.back().Limit) {
      Objects.back().Limit = std::max(Objects.back().Limit, End);
    } else {
      Objects.push_back({uint64_t(X.Offset), End, {}, {}});
    }
    Objects.back().Accesses.push_back(&X);
  }
  if (!E.empty()) {
    // Each escape joins the object it points into, or starts one.
    for (const Escape &X : E) {
      uint64_t At = X.Offset;
      auto It = llvm::partition_point(Objects, [&](const FrameObject &O) {
        return O.Limit <= At;
      });
      if (It == Objects.end() || It->Begin > At)
        It = Objects.insert(It, FrameObject{At, At + 1, {}, {}});
      It->Escapes.push_back(&X);
    }
    // An escaped object takes the write-only objects that directly follow
    // it (fields it initializes before handing it out), then reaches up to
    // the next object.
    SmallVector<FrameObject, 16> Merged;
    for (size_t I = 0; I < Objects.size(); ++I) {
      FrameObject O = std::move(Objects[I]);
      if (!O.Escapes.empty()) {
        auto WriteOnly = [](const FrameObject &N) {
          return N.Escapes.empty() &&
                 llvm::all_of(N.Accesses, [](const Access *X) {
                   return isa<StoreInst>(X->I);
                 });
        };
        while (I + 1 < Objects.size() && Objects[I + 1].Begin <= O.Limit &&
               WriteOnly(Objects[I + 1])) {
          FrameObject &N = Objects[++I];
          O.Limit = std::max(O.Limit, N.Limit);
          O.Accesses.append(N.Accesses.begin(), N.Accesses.end());
        }
        O.Limit = std::max(O.Limit, I + 1 < Objects.size()
                                        ? Objects[I + 1].Begin
                                        : FrameSize);
      }
      Merged.push_back(std::move(O));
    }
    Objects = std::move(Merged);
    // A run of adjacent write-only slots of one type is an array this
    // function fills and something else reads (an initializer list): one
    // object, not dead scalars.
    Merged.clear();
    auto Element = [&](const FrameObject &O) -> Type * {
      if (!O.Escapes.empty() || O.Accesses.empty())
        return nullptr;
      Type *T = O.Accesses.front()->Ty;
      for (const Access *X : O.Accesses)
        if (!isa<StoreInst>(X->I) || X->Ty != T ||
            uint64_t(X->Offset) != O.Begin)
          return nullptr;
      return DL.getTypeStoreSize(T) == O.Limit - O.Begin ? T : nullptr;
    };
    for (size_t I = 0; I < Objects.size(); ++I) {
      FrameObject O = std::move(Objects[I]);
      Type *T = Element(O);
      size_t N = 1;
      while (T && I + 1 < Objects.size() &&
             Objects[I + 1].Begin == O.Limit && Element(Objects[I + 1]) == T) {
        FrameObject &Next = Objects[++I];
        O.Limit = Next.Limit;
        O.Accesses.append(Next.Accesses.begin(), Next.Accesses.end());
        ++N;
      }
      if (N > 1)
        O.Ty = ArrayType::get(T, N);
      Merged.push_back(std::move(O));
    }
    Objects = std::move(Merged);
  }
  IRBuilder<> Entry(&Frame);
  for (const FrameObject &O : Objects) {
    Type *Ty = O.Ty;
    if (!Ty && !O.Accesses.empty()) {
      Ty = O.Accesses.front()->Ty;
      for (const Access *X : O.Accesses)
        if (uint64_t(X->Offset) != O.Begin || X->Ty != Ty)
          Ty = nullptr;
      if (Ty && DL.getTypeStoreSize(Ty) != O.Limit - O.Begin)
        Ty = nullptr;
    }
    bool Scalar = Ty && !Ty->isAggregateType() && O.Escapes.empty();
    if (!Ty)
      Ty = ArrayType::get(Entry.getInt8Ty(), O.Limit - O.Begin);
    auto *Slot = Entry.CreateAlloca(Ty, nullptr, "stack_" + Twine(O.Begin));
    Slot->setAlignment(Frame.getAlign());
    provenance(Slot, Frame);
    if (Scalar && ScalarSlots)
      ScalarSlots->push_back(Slot);
    if (AllSlots)
      AllSlots->push_back(Slot);
    for (const Access *X : O.Accesses) {
      IRBuilder<> B(X->I);
      Value *P = Slot;
      if (uint64_t(X->Offset) != O.Begin)
        P = B.CreateGEP(B.getInt8Ty(), Slot,
                       B.getInt64(uint64_t(X->Offset) - O.Begin));
      provenance(P, *X->I);
      if (auto *L = dyn_cast<LoadInst>(X->I))
        L->setOperand(0, P);
      else
        cast<StoreInst>(X->I)->setOperand(1, P);
    }
    for (const Escape *X : O.Escapes) {
      Value *P = Slot;
      if (uint64_t(X->Offset) != O.Begin) {
        auto *UserI = cast<Instruction>(X->U->getUser());
        IRBuilder<> B(UserI);
        P = B.CreateGEP(B.getInt8Ty(), Slot,
                        B.getInt64(uint64_t(X->Offset) - O.Begin));
        provenance(P, *UserI);
      }
      X->U->set(P);
    }
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
// The allocation a frame address is derived from (through constant GEPs).
const AllocaInst *frameBase(const Value *P) {
  while (const auto *G = dyn_cast<GetElementPtrInst>(P))
    P = G->getPointerOperand();
  return dyn_cast<AllocaInst>(P);
}
bool isFrame(const AllocaInst *A) {
  return A && A->getName().contains("frame");
}
// An unoptimized build stores every register argument to the frame before
// anything else reads it: the lifted entry block reads the argument
// register once and stores that value to a frame slot (clang -O0 parameter
// allocas, MSVC /Od home slots). Optimized code keeps arguments in
// registers. Stack arguments already arrive in the frame. A function
// without arguments has no such evidence and stays optimized. The stores
// sit in the prologue: right after `sub rsp` (clang), before the
// callee-saved pushes (MSVC /Od) or after the /GS cookie. Optimized code
// that spills an argument (register pressure, a taken address) does it
// later. rustc -O0 spills only what it describes in debug info; the driver
// decides for a rustc binary as a whole (lib/CAPI/Unoptimized.h).
bool spillsArguments(Function &F) {
  if (F.arg_empty())
    return false;
  BasicBlock &Entry = F.getEntryBlock();
  // Only the raw lift has the frame byte array and the register slots; a
  // later run (after mem2reg, or with an unsplit escaping frame) would read
  // an ordinary store of an argument as its spill.
  bool HasFrame = false, HasRegisters = false;
  for (Instruction &I : Entry)
    if (auto *A = dyn_cast<AllocaInst>(&I)) {
      HasFrame |= isFrame(A);
      HasRegisters |= A->getName() == "rax";
    }
  if (!HasFrame || !HasRegisters)
    return false;
  // Position of each machine instruction of the entry block.
  DenseMap<uint64_t, unsigned> Position;
  for (Instruction &I : Entry)
    if (auto VA = ir::getInstructionVA(I))
      Position.try_emplace(*VA, Position.size());
  const unsigned PrologueLimit = F.arg_size() + 6;
  auto InPrologue = [&](Instruction &I) {
    auto VA = ir::getInstructionVA(I);
    if (!VA)
      return false;
    auto It = Position.find(*VA);
    return It != Position.end() && It->second <= PrologueLimit;
  };
  // Whether argument A is spilled as an unoptimized prologue does it.
  auto Spilled = [&](Argument &A) -> bool {
    // The register the argument arrives in: `store %A, ptr %reg`, the
    // argument possibly extended or bit-cast to the register's width.
    AllocaInst *Reg = nullptr;
    bool InFrame = false;
    SmallVector<Value *, 4> Vals{&A};
    for (unsigned K = 0; K < Vals.size() && K < 8; ++K)
      for (User *U : Vals[K]->users()) {
        if (auto *C = dyn_cast<CastInst>(U)) {
          Vals.push_back(C);
          continue;
        }
        auto *S = dyn_cast<StoreInst>(U);
        if (!S || S->getValueOperand() != Vals[K] || S->getParent() != &Entry)
          continue;
        if (auto *R = dyn_cast<AllocaInst>(S->getPointerOperand()); R && !isFrame(R))
          Reg = R;
        else if (A.getArgNo() >= 4 && isFrame(frameBase(S->getPointerOperand())))
          InFrame = true; // a stack parameter the lifter placed in the frame
      }
    if (InFrame)
      return true;
    if (!Reg)
      return false;
    // Its first read in the entry block must be stored to the frame.
    LoadInst *First = nullptr;
    for (Instruction &I : Entry) {
      if (auto *L = dyn_cast<LoadInst>(&I))
        if (L->getPointerOperand() == Reg) {
          First = L;
          break;
        }
      if (auto *S = dyn_cast<StoreInst>(&I))
        if (S->getPointerOperand() == Reg && !llvm::is_contained(Vals, S->getValueOperand()))
          return false;
    }
    if (!First)
      return false;
    // An unoptimized body never computes from the argument register again:
    // every use reloads the slot. Optimized code keeps computing from it (a
    // second load, or the lifter's cached value feeding something besides
    // the spill). A load that only merges a partial write back into the
    // register (`mov cl, [rax]`: `and old, mask | new`) is no such use.
    auto MergesOnly = [&](LoadInst *L) {
      if (!L->hasOneUse())
        return false;
      auto *And = dyn_cast<BinaryOperator>(L->user_back());
      if (!And || And->getOpcode() != Instruction::And || !isa<ConstantInt>(And->getOperand(1)) ||
          !And->hasOneUse())
        return false;
      auto *Or = dyn_cast<BinaryOperator>(And->user_back());
      if (!Or || Or->getOpcode() != Instruction::Or || !Or->hasOneUse())
        return false;
      auto *S = dyn_cast<StoreInst>(Or->user_back());
      return S && S->getPointerOperand() == Reg;
    };
    for (Instruction *I = First->getNextNode(); I; I = I->getNextNode()) {
      if (auto *L = dyn_cast<LoadInst>(I); L && L->getPointerOperand() == Reg && !MergesOnly(L))
        return false;
      if (auto *S = dyn_cast<StoreInst>(I); S && S->getPointerOperand() == Reg)
        break;
    }
    {
      SmallVector<Value *, 4> Cast{First};
      for (unsigned K = 0; K < Cast.size(); ++K)
        for (User *U : Cast[K]->users()) {
          if (isa<CastInst>(U))
            Cast.push_back(cast<Instruction>(U));
          else if (auto *S = dyn_cast<StoreInst>(U);
                   !S || S->getValueOperand() != Cast[K] ||
                   !isFrame(frameBase(S->getPointerOperand())))
            return false;
        }
    }
    SmallVector<Value *, 4> Work{First};
    bool Stored = false;
    for (unsigned Steps = 0; !Work.empty() && !Stored && Steps < 16; ++Steps) {
      Value *V = Work.pop_back_val();
      for (User *U : V->users()) {
        if (auto *S = dyn_cast<StoreInst>(U)) {
          Stored |= S->getValueOperand() == V &&
                    isFrame(frameBase(S->getPointerOperand())) && InPrologue(*S);
        } else if (auto *C = dyn_cast<CastInst>(U)) {
          Work.push_back(C);
        }
      }
    }
    return Stored;
  };
  return llvm::all_of(F.args(), Spilled);
}
// The call that pins an unoptimized slot through cleanup.
constexpr char KeepSlotName[] = "revenant.o0.keep";
// Keep an unoptimized frame slot's memory traffic through cleanup: every
// access becomes volatile (forwarding and folding leave it alone), and the
// slot's address escapes into `revenant.o0.keep` so SROA and mem2reg keep
// the alloca whole, with the metadata on its accesses. `revenant-o0-slots`
// undoes both.
void markUnoptimizedSlot(AllocaInst &Slot) {
  MDNode *Tag = MDNode::get(Slot.getContext(), {});
  SmallVector<Value *, 8> Work{&Slot};
  while (!Work.empty()) {
    Value *P = Work.pop_back_val();
    for (Use &U : P->uses()) {
      User *Usr = U.getUser();
      if (auto *G = dyn_cast<GetElementPtrInst>(Usr)) {
        Work.push_back(G);
      } else if (auto *L = dyn_cast<LoadInst>(Usr)) {
        L->setVolatile(true);
        L->setMetadata("revenant.o0", Tag);
      } else if (auto *S = dyn_cast<StoreInst>(Usr);
                 S && U.getOperandNo() == StoreInst::getPointerOperandIndex()) {
        S->setVolatile(true);
        S->setMetadata("revenant.o0", Tag);
      }
    }
  }
  Module &M = *Slot.getModule();
  FunctionCallee Keep = M.getOrInsertFunction(
      KeepSlotName, FunctionType::get(Type::getVoidTy(M.getContext()),
                                      {PointerType::getUnqual(M.getContext())}, false));
  IRBuilder<> B(Slot.getNextNode());
  B.CreateCall(Keep, {&Slot});
}
bool isRustModule(const Module &M) {
  auto *S = dyn_cast_or_null<MDString>(M.getModuleFlag("revenant.lang"));
  return S && S->getString() == "rust";
}
} // namespace
PreservedAnalyses FrameSlotsPass::run(Function &F,
                                      FunctionAnalysisManager &FAM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  const DataLayout &DL = F.getDataLayout();
  SmallVector<AllocaInst *, 4> Frames;
  for (Instruction &I : F.getEntryBlock())
    if (auto *A = dyn_cast<AllocaInst>(&I))
      Frames.push_back(A);
  bool Changed = false;
  SmallVector<AllocaInst *, 16> Scalars;
  for (AllocaInst *A : Frames)
    Changed |= splitFrame(*A, DL, /*AllowEscapes=*/true, &Scalars);
  if (!Changed)
    return PreservedAnalyses::all();
  // The scalars no escape reaches are compiler temporaries and spills, the
  // values a pre-optimization IR keeps in SSA form, as the cleanup does for
  // a closed frame. The escaped objects and the write-only arrays stay.
  llvm::erase_if(Scalars, [](AllocaInst *A) { return !isAllocaPromotable(A); });
  if (!Scalars.empty())
    PromoteMemToReg(Scalars, FAM.getResult<DominatorTreeAnalysis>(F));
  return PreservedAnalyses::none();
}
// Every use of the slot's address, through constant GEPs, is a load or a
// store to it: nothing else can read or write the slot.
static bool closedSlot(AllocaInst &Slot) {
  SmallVector<Value *, 8> Work{&Slot};
  while (!Work.empty()) {
    Value *P = Work.pop_back_val();
    for (Use &U : P->uses()) {
      User *Usr = U.getUser();
      if (isa<GetElementPtrInst>(Usr))
        Work.push_back(Usr);
      else if (isa<LoadInst>(Usr))
        continue;
      else if (auto *S = dyn_cast<StoreInst>(Usr);
               !S || U.getOperandNo() != StoreInst::getPointerOperandIndex())
        return false;
    }
  }
  return true;
}
// Erase the slot and the GEPs over it once no access remains.
static void eraseDeadAddresses(AllocaInst &Slot) {
  SmallVector<Instruction *, 8> Order;
  SmallVector<Instruction *, 8> Work{&Slot};
  while (!Work.empty()) {
    Instruction *P = Work.pop_back_val();
    Order.push_back(P);
    for (User *U : P->users())
      if (auto *G = dyn_cast<GetElementPtrInst>(U))
        Work.push_back(G);
  }
  for (Instruction *I : llvm::reverse(Order))
    if (I->use_empty())
      I->eraseFromParent();
}
namespace {
// (slot, byte offset) of an address made of constant GEPs over an alloca.
std::pair<AllocaInst *, int64_t> slotOffset(Value *P, const DataLayout &DL) {
  int64_t Off = 0;
  while (auto *G = dyn_cast<GetElementPtrInst>(P)) {
    APInt O(DL.getIndexTypeSizeInBits(G->getType()), 0);
    if (!G->accumulateConstantOffset(DL, O))
      return {nullptr, 0};
    Off += O.getSExtValue();
    P = G->getPointerOperand();
  }
  return {dyn_cast<AllocaInst>(P), Off};
}
struct SlotUses {
  SmallVector<LoadInst *, 4> Loads;
  SmallVector<StoreInst *, 4> Stores;
  // Any use that is not a direct load or store (GEP, escape).
  bool Indirect = false;
};
SlotUses usesOf(AllocaInst &A) {
  SlotUses U;
  for (Use &Us : A.uses()) {
    if (auto *L = dyn_cast<LoadInst>(Us.getUser()))
      U.Loads.push_back(L);
    else if (auto *S = dyn_cast<StoreInst>(Us.getUser());
             S && Us.getOperandNo() == StoreInst::getPointerOperandIndex())
      U.Stores.push_back(S);
    else
      U.Indirect = true;
  }
  return U;
}
// V re-read as a value of type Ty: the same bits through a no-op cast.
Value *asType(Value *V, Type *Ty, Instruction *Before, const DataLayout &DL) {
  if (V->getType() == Ty)
    return V;
  if (DL.getTypeStoreSize(V->getType()) != DL.getTypeStoreSize(Ty))
    return nullptr;
  IRBuilder<> B(Before);
  if (V->getType()->isPointerTy() && Ty->isIntegerTy())
    return B.CreatePtrToInt(V, Ty);
  if (V->getType()->isIntegerTy() && Ty->isPointerTy())
    return B.CreateIntToPtr(V, Ty);
  if (CastInst::isBitCastable(V->getType(), Ty))
    return B.CreateBitCast(V, Ty);
  return nullptr;
}
// One unoptimized slot whose direct accesses sit at distinct, non-overlapping
// offsets becomes one alloca per offset.
bool splitSlot(AllocaInst &Slot, const DataLayout &DL) {
  if (!Slot.isStaticAlloca())
    return false;
  struct Acc {
    Instruction *I;
    unsigned PtrOp;
    int64_t Off;
    uint64_t Size;
    Type *Ty;
  };
  SmallVector<Acc, 8> Accs;
  SmallVector<Value *, 8> Work{&Slot};
  while (!Work.empty()) {
    Value *P = Work.pop_back_val();
    for (Use &U : P->uses()) {
      User *Usr = U.getUser();
      if (isa<GetElementPtrInst>(Usr)) {
        Work.push_back(Usr);
        continue;
      }
      Type *Ty = nullptr;
      unsigned PtrOp = 0;
      if (auto *L = dyn_cast<LoadInst>(Usr)) {
        Ty = L->getType();
      } else if (auto *S = dyn_cast<StoreInst>(Usr);
                 S && U.getOperandNo() == StoreInst::getPointerOperandIndex()) {
        Ty = S->getValueOperand()->getType();
        PtrOp = 1;
      } else {
        return false;
      }
      auto *I = cast<Instruction>(Usr);
      auto [Base, Off] = slotOffset(I->getOperand(PtrOp), DL);
      TypeSize Size = DL.getTypeStoreSize(Ty);
      if (Base != &Slot || Off < 0 || Size.isScalable())
        return false;
      Accs.push_back({I, PtrOp, Off, Size.getFixedValue(), Ty});
    }
  }
  llvm::stable_sort(Accs, [](const Acc &A, const Acc &B) { return A.Off < B.Off; });
  struct Group {
    int64_t Begin;
    int64_t End;
    SmallVector<const Acc *, 4> Members;
  };
  SmallVector<Group, 4> Groups;
  for (const Acc &A : Accs) {
    int64_t End = A.Off + int64_t(A.Size);
    if (!Groups.empty() && A.Off < Groups.back().End)
      Groups.back().End = std::max(Groups.back().End, End);
    else
      Groups.push_back({A.Off, End, {}});
    Groups.back().Members.push_back(&A);
  }
  if (Groups.size() < 2)
    return false;
  IRBuilder<> Entry(&Slot);
  for (const Group &G : Groups) {
    Type *Ty = G.Members.front()->Ty;
    for (const Acc *A : G.Members)
      if (A->Off != G.Begin || A->Ty != Ty)
        Ty = nullptr;
    if (!Ty)
      Ty = ArrayType::get(Entry.getInt8Ty(), G.End - G.Begin);
    AllocaInst *Part = Entry.CreateAlloca(Ty, nullptr, Slot.getName());
    Part->setAlignment(Slot.getAlign());
    provenance(Part, Slot);
    for (const Acc *A : G.Members) {
      IRBuilder<> B(A->I);
      Value *P = Part;
      if (A->Off != G.Begin)
        P = B.CreateGEP(B.getInt8Ty(), Part, B.getInt64(A->Off - G.Begin));
      A->I->setOperand(A->PtrOp, P);
    }
  }
  eraseDeadAddresses(Slot);
  return true;
}
} // namespace
PreservedAnalyses UnoptimizedModulePass::run(Module &M, ModuleAnalysisManager &) {
  unsigned WithArgs = 0, Unoptimized = 0;
  for (const Function &F : M)
    if (!F.isDeclaration() && !F.arg_empty() &&
        F.getFnAttribute("revenant.user").getValueAsString() == "true") {
      ++WithArgs;
      Unoptimized += F.hasFnAttribute("revenant.o0");
    }
  if (!Unoptimized || 2 * Unoptimized < WithArgs)
    return PreservedAnalyses::all();
  // Everything but the code a library signature identified: the user code
  // the reachability walk missed was built with the same flags.
  bool Changed = false;
  for (Function &F : M)
    if (!F.isDeclaration() && !F.hasFnAttribute("revenant.lib") &&
        !F.hasFnAttribute("revenant.o0")) {
      F.addFnAttr("revenant.o0");
      Changed = true;
    }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
PreservedAnalyses UnoptimizedSlotsPass::run(Function &F,
                                            FunctionAnalysisManager &FAM) {
  if (F.isDeclaration() || !F.hasFnAttribute("revenant.o0"))
    return PreservedAnalyses::all();
  const DataLayout &DL = F.getDataLayout();
  // The frame slots' accesses are ordinary memory again. Cleanup may split
  // a slot (SROA keeps the volatile flag, not the tag), so every stack
  // access counts: the lifter never makes one volatile itself.
  for (Instruction &I : instructions(F)) {
    if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (L->isVolatile() && slotOffset(L->getPointerOperand(), DL).first)
        L->setVolatile(false);
    } else if (auto *S = dyn_cast<StoreInst>(&I)) {
      if (S->isVolatile() && slotOffset(S->getPointerOperand(), DL).first)
        S->setVolatile(false);
    }
    I.setMetadata("revenant.o0", nullptr);
  }
  // Unpin the slots. A store of an undefined register (`push` of a register
  // the function never set, which reserves stack) and a load nothing reads
  // (the matching `pop`) are not part of any variable.
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    if (auto *C = dyn_cast<CallInst>(&I)) {
      if (C->getCalledFunction() && C->getCalledFunction()->getName() == KeepSlotName)
        C->eraseFromParent();
    } else if (auto *S = dyn_cast<StoreInst>(&I)) {
      if (isa<UndefValue>(S->getValueOperand()) && slotOffset(S->getPointerOperand(), DL).first)
        S->eraseFromParent();
    } else if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (L->use_empty() && !L->isVolatile() && slotOffset(L->getPointerOperand(), DL).first)
        L->eraseFromParent();
    }
  }
  // With the reservation gone, one frame object may hold several variables
  // at distinct offsets (two parameters inside an 8-byte `push`): one alloca
  // per variable.
  {
    SmallVector<AllocaInst *, 16> Allocas;
    for (Instruction &I : F.getEntryBlock())
      if (auto *A = dyn_cast<AllocaInst>(&I))
        Allocas.push_back(A);
    for (AllocaInst *A : Allocas)
      if (!splitSlot(*A, DL))
        eraseDeadAddresses(*A); // a reservation nothing uses (frame padding)
  }
  // Outgoing stack arguments: the caller stores them right below the call;
  // the compiler's IR passes the value as an operand instead.
  SmallPtrSet<AllocaInst *, 8> ArgSlots;
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *L = dyn_cast<LoadInst>(&I);
    if (!L || !L->getMetadata("revenant.stackarg"))
      continue;
    auto [Slot, Off] = slotOffset(L->getPointerOperand(), DL);
    if (!Slot || !closedSlot(*Slot))
      continue;
    for (Instruction *P = L->getPrevNode(); P; P = P->getPrevNode()) {
      if (auto *S = dyn_cast<StoreInst>(P)) {
        auto [SSlot, SOff] = slotOffset(S->getPointerOperand(), DL);
        if (SSlot != Slot)
          continue;
        if (SOff == Off)
          if (Value *V = asType(S->getValueOperand(), L->getType(), L, DL)) {
            L->replaceAllUsesWith(V);
            L->eraseFromParent();
            ArgSlots.insert(Slot);
          }
        break;
      }
      if (isa<CallBase>(P) && !isa<DbgInfoIntrinsic>(P))
        break;
    }
  }
  // An argument slot nothing reads any more is the outgoing-argument area.
  for (AllocaInst *Slot : ArgSlots) {
    bool Read = false;
    SmallVector<Instruction *, 8> Dead;
    SmallVector<Value *, 8> Work{Slot};
    while (!Work.empty() && !Read) {
      Value *P = Work.pop_back_val();
      for (User *U : P->users()) {
        if (auto *G = dyn_cast<GetElementPtrInst>(U))
          Work.push_back(G);
        else if (auto *S = dyn_cast<StoreInst>(U); S && S->getPointerOperand() == P)
          Dead.push_back(S);
        else
          Read = true;
      }
    }
    if (Read)
      continue;
    for (Instruction *S : Dead)
      S->eraseFromParent();
    eraseDeadAddresses(*Slot);
  }
  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  SmallVector<AllocaInst *, 16> Slots;
  for (Instruction &I : F.getEntryBlock())
    if (auto *A = dyn_cast<AllocaInst>(&I))
      Slots.push_back(A);
  for (AllocaInst *A : Slots) {
    SlotUses U = usesOf(*A);
    if (U.Indirect || U.Stores.empty())
      continue;
    // Only an undefined register is ever stored: a callee-saved register
    // pushed and popped around the body.
    if (llvm::all_of(U.Stores, [](StoreInst *S) { return isa<UndefValue>(S->getValueOperand()); })) {
      for (LoadInst *L : U.Loads) {
        L->replaceAllUsesWith(UndefValue::get(L->getType()));
        L->eraseFromParent();
      }
      for (StoreInst *S : U.Stores)
        S->eraseFromParent();
      A->eraseFromParent();
      continue;
    }
    // rustc -O0 writes the values it describes in debug info to slots it
    // never reads (`<name>.dbg.spill`); those are debug info, not program
    // state, so a slot nothing reads back goes whatever it holds.
    if (U.Loads.empty() && isRustModule(*F.getParent())) {
      for (StoreInst *S : U.Stores)
        S->eraseFromParent();
      A->eraseFromParent();
      continue;
    }
    // A register-allocator spill: one store of a value that is also used
    // directly, read back only where that store dominates. A value used
    // only by the store is a spill too when it is read back once in the
    // same block (RegAllocFast keeps a temporary across a call or a
    // division that needs its register); arguments and loaded values stay,
    // those are parameter homes and copies between variables.
    if (U.Stores.size() != 1)
      continue;
    StoreInst *S = U.Stores.front();
    Value *V = S->getValueOperand();
    if (!isa<Instruction>(V) && !isa<Argument>(V))
      continue;
    if (V->hasOneUse()) {
      bool Temporary = isa<Instruction>(V) && !isa<LoadInst>(V) &&
                       U.Loads.size() == 1 &&
                       U.Loads.front()->getParent() == S->getParent();
      // The return value kept across the epilogue (the /GS check call):
      // the compiler's IR returns the loaded value itself.
      bool Returned = isa<Instruction>(V) && U.Loads.size() == 1 &&
                      U.Loads.front()->hasOneUse() &&
                      isa<ReturnInst>(U.Loads.front()->user_back());
      if (!Temporary && !Returned)
        continue;
    }
    if (!llvm::all_of(U.Loads, [&](LoadInst *L) {
          return L->getType() == V->getType() && DT.dominates(S, L);
        }))
      continue;
    for (LoadInst *L : U.Loads) {
      L->replaceAllUsesWith(V);
      L->eraseFromParent();
    }
    S->eraseFromParent();
    A->eraseFromParent();
  }
  // Cleanup's instcombine rewrote source subtractions: `x - c` became
  // `x + -c`, and `y - x / c` became `y + x / -c`. The compiler's -O0 IR
  // has the subtraction.
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *Add = dyn_cast<BinaryOperator>(&I);
    if (!Add || Add->getOpcode() != Instruction::Add)
      continue;
    auto Negated = [](Value *V) -> Constant * {
      auto *C = dyn_cast<ConstantInt>(V);
      if (!C || !C->isNegative() || C->isMinValue(/*IsSigned=*/true))
        return nullptr;
      return ConstantInt::get(C->getType(), -C->getValue());
    };
    IRBuilder<> B(Add);
    Value *Sub = nullptr;
    if (Constant *C = Negated(Add->getOperand(1))) {
      Sub = B.CreateSub(Add->getOperand(0), C, "", /*HasNUW=*/false,
                        Add->hasNoSignedWrap());
    } else {
      for (unsigned K = 0; K < 2 && !Sub; ++K) {
        auto *Div = dyn_cast<BinaryOperator>(Add->getOperand(K));
        if (!Div || Div->getOpcode() != Instruction::SDiv || !Div->hasOneUse())
          continue;
        if (Constant *C = Negated(Div->getOperand(1))) {
          Value *Pos = B.CreateSDiv(Div->getOperand(0), C, "", Div->isExact());
          Sub = B.CreateSub(Add->getOperand(1 - K), Pos);
        }
      }
    }
    if (!Sub)
      continue;
    if (auto *SI = dyn_cast<Instruction>(Sub))
      provenance(SI, *Add);
    Sub->takeName(Add);
    Add->replaceAllUsesWith(Sub);
    RecursivelyDeleteTriviallyDeadInstructions(Add);
  }
  return PreservedAnalyses::none();
}
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
  // An unoptimized function keeps its frame: every slot becomes its own
  // alloca (escaped ones too) and its accesses survive cleanup, as the
  // compiler's -O0 IR keeps one alloca per variable. The first run sees the
  // raw lift and decides; later runs find the attribute. A rustc binary is
  // decided as a whole by the driver, which sets the attribute before
  // cleanup (lib/CAPI/Unoptimized.h).
  bool Unoptimized = F.hasFnAttribute("revenant.o0");
  if (!Unoptimized && !Frames.empty() && !isRustModule(*F.getParent()) && spillsArguments(F)) {
    F.addFnAttr("revenant.o0");
    Unoptimized = true;
  }
  SmallVector<AllocaInst *, 16> Slots;
  for (AllocaInst *A : Frames)
    Changed |= Unoptimized ? splitFrame(*A, DL, /*AllowEscapes=*/true, nullptr, &Slots)
                           : splitFrame(*A, DL);
  for (AllocaInst *S : Slots)
    markUnoptimizedSlot(*S);
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
