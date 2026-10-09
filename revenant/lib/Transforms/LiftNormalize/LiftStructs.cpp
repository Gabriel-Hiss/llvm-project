//===- LiftStructs.cpp - shared pointer field layouts --------------------===//
#include "revenant/Transforms/LiftNormalize/LiftNormalize.h"
#include "revenant/IR/Contract.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include <map>
using namespace llvm;

namespace revenant {
namespace {
class Bases {
  DenseMap<Value *, Value *> Parent;
public:
  Value *find(Value *V) {
    auto I = Parent.find(V);
    if (I == Parent.end()) {
      Parent[V] = V;
      return V;
    }
    if (I->second == V)
      return V;
    Value *R = find(I->second);
    Parent[V] = R;
    return R;
  }
  void unite(Value *A, Value *B) {
    A = find(A);
    B = find(B);
    if (A != B)
      Parent[B] = A;
  }
};
struct Field {
  Type *Ty = nullptr;
  bool Conflict = false;
  SmallVector<Instruction *, 8> Accesses;
};
Value *base(Value *P, int64_t &Offset, const DataLayout &DL) {
  return GetPointerBaseWithConstantOffset(P, Offset, DL, true);
}
} // namespace
PreservedAnalyses LiftStructsPass::run(Module &M, ModuleAnalysisManager &) {
  const DataLayout &DL = M.getDataLayout();
  Bases Shared;
  for (Function &F : M)
    for (Instruction &I : instructions(F))
      if (auto *C = dyn_cast<CallBase>(&I)) {
        Function *Target = C->getCalledFunction();
        if (!Target || Target->isDeclaration())
          continue;
        for (Argument &A : Target->args())
          if (A.getType()->isPointerTy()) {
            int64_t Offset = 0;
            Value *Actual = base(C->getArgOperand(A.getArgNo()), Offset, DL);
            // A field pointer passed to another function starts a new object.
            // Merging it with its parent's layout would lose that offset.
            if (!Offset)
              Shared.unite(Actual, &A);
          }
        if (C->getType()->isPointerTy())
          for (BasicBlock &BB : *Target)
            if (auto *R = dyn_cast<ReturnInst>(BB.getTerminator()))
              if (R->getReturnValue()) {
                int64_t Offset = 0;
                Value *Returned = base(R->getReturnValue(), Offset, DL);
                if (!Offset)
                  Shared.unite(Returned, C);
              }
      }
  DenseMap<Value *, std::map<int64_t, Field>> Layouts;
  for (Function &F : M)
    for (Instruction &I : instructions(F)) {
      Value *P = nullptr;
      Type *Ty = nullptr;
      if (auto *L = dyn_cast<LoadInst>(&I)) {
        P = L->getPointerOperand();
        Ty = L->getType();
      } else if (auto *S = dyn_cast<StoreInst>(&I)) {
        P = S->getPointerOperand();
        Ty = S->getValueOperand()->getType();
      }
      if (!P || !Ty->isSized() || Ty->isVectorTy() || Ty->isAggregateType())
        continue;
      if (auto *GEP = dyn_cast<GetElementPtrInst>(P))
        if (GEP->getMetadata("revenant.pointee"))
          continue;
      int64_t Offset = 0;
      Value *Root = base(P, Offset, DL);
      if (Offset < 0 || Offset > 65536 || isa<GlobalValue>(Root))
        continue;
      if (auto *A = dyn_cast<AllocaInst>(Root))
        if (!isa<ArrayType>(A->getAllocatedType()))
          continue;
      Field &Cell = Layouts[Shared.find(Root)][Offset];
      if (Cell.Ty && Cell.Ty != Ty)
        Cell.Conflict = true;
      else
        Cell.Ty = Ty;
      Cell.Accesses.push_back(&I);
    }
  DICompileUnit *Existing = nullptr;
  if (auto *CUs = M.getNamedMetadata("llvm.dbg.cu"))
    if (CUs->getNumOperands())
      Existing = dyn_cast<DICompileUnit>(CUs->getOperand(0));
  DIBuilder DIB(M, true, Existing);
  bool Changed = false;
  unsigned Number = 0;
  for (auto &KV : Layouts) {
    auto &Fields = KV.second;
    if (Fields.size() < 2)
      continue;
    bool Valid = true;
    uint64_t End = 0;
    for (auto &F : Fields) {
      TypeSize Size = DL.getTypeAllocSize(F.second.Ty);
      if (F.second.Conflict || Size.isScalable() || uint64_t(F.first) < End) {
        Valid = false;
        break;
      }
      End = F.first + Size.getFixedValue();
    }
    if (!Valid)
      continue;
    SmallVector<Type *, 16> Elements;
    std::map<int64_t, unsigned> Indices;
    uint64_t At = 0;
    for (auto &F : Fields) {
      if (uint64_t(F.first) > At)
        Elements.push_back(ArrayType::get(Type::getInt8Ty(M.getContext()), F.first - At));
      Indices[F.first] = Elements.size();
      Elements.push_back(F.second.Ty);
      At = F.first + DL.getTypeAllocSize(F.second.Ty).getFixedValue();
    }
    std::string Name = "rv_layout_" + std::to_string(Number++);
    StructType *ST = StructType::create(M.getContext(), Elements, Name, true);
    auto &CU = ir::getOrCreateCompileUnit(M, DIB, "revenant.ll");
    SmallVector<Metadata *, 16> Members;
    for (auto &F : Fields) {
      Type *Ty = F.second.Ty;
      uint64_t Bits = DL.getTypeAllocSize(Ty).getFixedValue() * 8;
      DIType *DT = nullptr;
      if (Ty->isPointerTy())
        DT = ir::createPointerType(DIB, ir::createBasicType(DIB, "uint8_t", 8,
                                                         dwarf::DW_ATE_unsigned));
      else
        DT = ir::createBasicType(DIB, Ty->isFloatingPointTy() ? "double" :
                                "uint" + std::to_string(Bits) + "_t", Bits,
                                Ty->isFloatingPointTy() ? dwarf::DW_ATE_float :
                                                         dwarf::DW_ATE_unsigned);
      Members.push_back(DIB.createMemberType(&CU, "field_" + std::to_string(F.first),
          CU.getFile(), 1, Bits, 8, F.first * 8, DINode::FlagZero, DT));
    }
    DIType *Composite = ir::createStructType(DIB, &CU, Name, End * 8, Members);
    for (auto &F : Fields)
      for (Instruction *I : F.second.Accesses) {
        Value *Old = isa<LoadInst>(I) ? cast<LoadInst>(I)->getPointerOperand() :
                                       cast<StoreInst>(I)->getPointerOperand();
        int64_t Offset = 0;
        Value *Root = base(Old, Offset, DL);
        IRBuilder<> B(I);
        Value *P = B.CreateGEP(ST, Root, {B.getInt32(0), B.getInt32(Indices[Offset])}, "field");
        if (auto *G = dyn_cast<Instruction>(P)) {
          if (MDNode *VA = I->getMetadata("revenant.va"))
            G->setMetadata("revenant.va", VA);
          G->setMetadata("revenant.pointee", MDNode::get(M.getContext(), Composite));
          G->setDebugLoc(I->getDebugLoc());
        }
        if (auto *L = dyn_cast<LoadInst>(I))
          L->setOperand(0, P);
        else
          cast<StoreInst>(I)->setOperand(1, P);
      }
    Changed = true;
  }

  // Struct metadata can add a subprogram to a lifted function. LLVM then
  // requires every call in that function to carry a debug location.
  for (Function &F : M)
    if (DISubprogram *SP = F.getSubprogram()) {
      DILocation *Loc = DILocation::get(M.getContext(), 1, 1, SP);
      for (Instruction &I : instructions(F))
        if (isa<CallBase>(I) && !I.getDebugLoc()) {
          I.setDebugLoc(Loc);
          Changed = true;
        }
    }
  if (NamedMDNode *CUs = M.getNamedMetadata("llvm.dbg.cu"))
    if (CUs->getNumOperands())
      DIB.finalize();
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // namespace revenant
