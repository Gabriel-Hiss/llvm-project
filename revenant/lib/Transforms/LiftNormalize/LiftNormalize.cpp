//===- LiftNormalize.cpp - machine state to compiler IR ------------------===//
#include "revenant/Transforms/LiftNormalize/LiftNormalize.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
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
  if (auto *P = dyn_cast<PtrToIntInst>(V))
    return P->getPointerOperand()->getType() == PT ? P->getPointerOperand()
                                                 : nullptr;
  if (auto *Phi = dyn_cast<PHINode>(V)) {
    auto It = PointerPhis.find(Phi);
    if (It != PointerPhis.end())
      return It->second;
    for (unsigned J = 0; J < Phi->getNumIncomingValues(); ++J)
      if (isa<InvokeInst>(Phi->getIncomingBlock(J)->getTerminator()) ||
          Phi->getIncomingBlock(J)->isEHPad())
        return nullptr;
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
        Ptr = Edge.CreateIntToPtr(Incoming, PT);
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
  for (Instruction &I : make_early_inc_range(instructions(F)))
    if (auto *G = dyn_cast<GetElementPtrInst>(&I))
      Changed |= typeGEP(*G, DL);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // namespace revenant
