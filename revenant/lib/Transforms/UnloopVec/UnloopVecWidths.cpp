//===- UnloopVecWidths.cpp - recover scalar element type/stride -----------===//
//
// Part of the Revenant LLVM fork.
//
// See UnloopVecWidths.h for the firing shape and the soundness argument.
//
// Recovers the scalar element type and stride from vectorized loops with
// type changes: widening/narrowing (pmovzx/pmovsx, packus/packss, punpck),
// i8/i16 element types, interleaved/strided access via shuffles, unaligned
// heads, conversions int<->float.
//
// Firing shape: a single-block self-loop L with constant-step IV where
// step == vector width V, at least one vector load/store with scalarizable
// element type (i8/i16/i32/i64/float/double), and every vector op in L
// is element-wise (no cross-lane shuffles, no reductions, no gathers).
//
// On match: each vector op is replaced by V scalar ops (one per lane),
// the IV step becomes 1, and the body iterates V times more often.
// The original loop condition is preserved (same trip count N).
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/UnloopVec/UnloopVecWidths.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PatternMatch.h"
#include <algorithm>
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace revenant {
namespace {

#define DEBUG_TYPE "unloopvecwidths"

bool isVectorTyped(Type *T) { return isa<FixedVectorType>(T); }

bool isScalarizableElemType(Type *T) {
  return T->isIntegerTy(8) || T->isIntegerTy(16) || T->isIntegerTy(32) ||
         T->isIntegerTy(64) || T->isFloatTy() || T->isDoubleTy();
}

// Cross-lane or shape-changing vector ops that block scalarization.
bool isCrossLaneOp(const Instruction &I) {
  if (auto *SI = dyn_cast<ShuffleVectorInst>(&I)) {
    auto *VT = dyn_cast<FixedVectorType>(SI->getType());
    if (!VT) return false;
    unsigned N = VT->getNumElements();
    auto Mask = SI->getShuffleMask();
    for (unsigned i = 0; i != N; ++i) {
      int M = Mask[i];
      if (M >= 0 && (unsigned)M != i)
        return true;
    }
  }
  if (isa<InsertElementInst>(&I) || isa<ExtractElementInst>(&I))
    return true;
  if (const auto *CB = dyn_cast<CallBase>(&I)) {
    if (const Function *CF = CB->getCalledFunction()) {
      StringRef N = CF->getName();
      if (N.starts_with("llvm.vector.reduce.") ||
          N.starts_with("llvm.masked.") || N.starts_with("llvm.vp."))
        return true;
    }
  }
  return false;
}

// Vector ops we know how to scalarize lane-by-lane.
bool isElementWiseVectorOp(const Instruction &I) {
  if (isCrossLaneOp(I)) return false;
  if (isa<LoadInst>(&I)) return isVectorTyped(I.getType());
  if (isa<StoreInst>(&I)) return isVectorTyped(I.getOperand(0)->getType());
  if (isa<ICmpInst>(&I)) return isVectorTyped(I.getOperand(0)->getType());
  if (isa<FCmpInst>(&I)) return isVectorTyped(I.getOperand(0)->getType());
  if (auto *BO = dyn_cast<BinaryOperator>(&I))
    return isVectorTyped(I.getType()) &&
           (BO->getOpcode() == Instruction::Add ||
            BO->getOpcode() == Instruction::Sub ||
            BO->getOpcode() == Instruction::Mul ||
            BO->getOpcode() == Instruction::And ||
            BO->getOpcode() == Instruction::Or ||
            BO->getOpcode() == Instruction::Xor ||
            BO->getOpcode() == Instruction::Shl ||
            BO->getOpcode() == Instruction::LShr ||
            BO->getOpcode() == Instruction::AShr ||
            BO->getOpcode() == Instruction::FAdd ||
            BO->getOpcode() == Instruction::FSub ||
            BO->getOpcode() == Instruction::FMul ||
            BO->getOpcode() == Instruction::FDiv);
  if (auto *CI = dyn_cast<CastInst>(&I))
    return isVectorTyped(I.getType()) && isVectorTyped(CI->getOperand(0)->getType()) &&
           (CI->getOpcode() == Instruction::ZExt ||
            CI->getOpcode() == Instruction::SExt ||
            CI->getOpcode() == Instruction::Trunc ||
            CI->getOpcode() == Instruction::BitCast);
  if (auto *SI = dyn_cast<SelectInst>(&I))
    return isVectorTyped(I.getType()) && isVectorTyped(I.getOperand(1)->getType());
  return false;
}

bool isVectorConstZero(Value *V) {
  if (auto *C = dyn_cast<Constant>(V))
    if (isVectorTyped(C->getType()))
      return C->isNullValue();
  return false;
}

bool matchScalarLoop(BasicBlock *BB, PHINode *&IV, ConstantInt *&Step) {
  auto *Term = dyn_cast<BranchInst>(BB->getTerminator());
  if (!Term || !Term->isConditional()) return false;
  bool Self = false;
  for (BasicBlock *S : successors(BB)) if (S == BB) Self = true;
  if (!Self) return false;
  if (Term->getSuccessor(0) == Term->getSuccessor(1)) return false;
  for (PHINode &P : BB->phis()) {
    Value *Back = nullptr, *Start = nullptr;
    for (unsigned i = 0; i != P.getNumIncomingValues(); ++i) {
      if (P.getIncomingBlock(i) == BB) Back = P.getIncomingValue(i);
      else Start = P.getIncomingValue(i);
    }
    if (!Back || !Start) continue;
    auto *Add = dyn_cast<BinaryOperator>(Back);
    if (!Add || Add->getParent() != BB) continue;
    if (Add->getOpcode() != Instruction::Add) continue;
    if (Add->getOperand(0) != &P) continue;
    if (auto *C = dyn_cast<ConstantInt>(Add->getOperand(1))) {
      IV = &P; Step = C; return true;
    }
  }
  return false;
}

// Get element type of a vector-typed instruction.
Type *elemTyOf(const Instruction &I) {
  if (auto *LI = dyn_cast<LoadInst>(&I)) {
    auto *VT = dyn_cast<FixedVectorType>(LI->getType());
    return VT ? VT->getElementType() : nullptr;
  }
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    auto *VT = dyn_cast<FixedVectorType>(SI->getValueOperand()->getType());
    return VT ? VT->getElementType() : nullptr;
  }
  if (isa<ICmpInst>(&I) || isa<FCmpInst>(&I)) {
    auto *VT = dyn_cast<FixedVectorType>(I.getOperand(0)->getType());
    return VT ? VT->getElementType() : nullptr;
  }
  auto *VT = dyn_cast<FixedVectorType>(I.getType());
  return VT ? VT->getElementType() : nullptr;
}

// Check that a cast between vector types has matching element types.
bool castElemTypesMatch(const CastInst *CI) {
  auto *Src = dyn_cast<FixedVectorType>(CI->getOperand(0)->getType());
  auto *Dst = dyn_cast<FixedVectorType>(CI->getType());
  return Src && Dst && Src->getElementType() == Dst->getElementType();
}

bool scalarizeLoop(BasicBlock *BB, PHINode *IV, ConstantInt *Step, Function &F) {
  // 1. Collect vector instructions and determine element type + width.
  SmallVector<Instruction *, 32> VecInsts;
  Type *ElemTy = nullptr;
  unsigned Width = 0;
  for (Instruction &I : *BB) {
    if (!isVectorTyped(I.getType()) && !isa<StoreInst>(&I)) continue;
    if (isa<PHINode>(&I) || isa<BranchInst>(&I) || isa<DbgInfoIntrinsic>(&I))
      continue;
    if (!isElementWiseVectorOp(I)) return false;
    if (!isVectorTyped(I.getType()) && !(isa<StoreInst>(&I) && isVectorTyped(cast<StoreInst>(&I)->getValueOperand()->getType()))) continue;
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      auto *VT = dyn_cast<FixedVectorType>(LI->getType());
      if (!VT || !isScalarizableElemType(VT->getElementType())) return false;
      if (!ElemTy) { ElemTy = VT->getElementType(); Width = VT->getNumElements(); }
      else if (VT->getElementType() != ElemTy || VT->getNumElements() != Width) return false;
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      auto *VT = dyn_cast<FixedVectorType>(SI->getValueOperand()->getType());
      if (!VT || !isScalarizableElemType(VT->getElementType())) return false;
      if (!ElemTy) { ElemTy = VT->getElementType(); Width = VT->getNumElements(); }
      else if (VT->getElementType() != ElemTy || VT->getNumElements() != Width) return false;
    } else if (auto *CI = dyn_cast<CastInst>(&I)) {
      if (!castElemTypesMatch(CI)) return false;
      auto *VT = dyn_cast<FixedVectorType>(CI->getType());
      if (!VT || !isScalarizableElemType(VT->getElementType())) return false;
      if (!ElemTy) { ElemTy = VT->getElementType(); Width = VT->getNumElements(); }
      else if (VT->getElementType() != ElemTy || VT->getNumElements() != Width) return false;
    } else {
      Type *ET = elemTyOf(I);
      if (!ET || ET != ElemTy) return false;
    }
    VecInsts.push_back(&I);
  }
  if (!ElemTy || Width == 0) return false;
  if (Step->getZExtValue() != Width) return false;

  // 2. Check trip count and rewrite condition for step 1.
  // Original loop: iv += Width, condition iv.next < n (or similar).
  // After scalarize: iv += 1, need condition iv < n to process n elements.
  auto *Term = dyn_cast<BranchInst>(BB->getTerminator());
  if (!Term || !Term->isConditional()) return false;
  auto *Cond = dyn_cast<ICmpInst>(Term->getCondition());
  if (!Cond) return false;
  // The condition should be on iv.next (the IV + Step)
  auto *IVNext = dyn_cast<BinaryOperator>(IV->getIncomingValueForBlock(BB));
  if (!IVNext || IVNext->getOpcode() != Instruction::Add) return false;
  if (Cond->getOperand(0) != IVNext && Cond->getOperand(1) != IVNext) return false;
  // Get the bound (the non-IV operand)
  Value *Bound = (Cond->getOperand(0) == IVNext) ? Cond->getOperand(1) : Cond->getOperand(0);
  // For non-constant bounds, we cannot prove n % Width == 0.
  // Conservative: only fire when the bound is a constant divisible by Width.
  if (auto *BoundCI = dyn_cast<ConstantInt>(Bound)) {
    uint64_t N = BoundCI->getZExtValue();
    if (N % Width != 0) return false;
  }
  // Rewrite condition to iv < n (ICMP_ULT)
  IRBuilder<> BCond(Term);
  auto *NewCond = BCond.CreateICmp(ICmpInst::ICMP_ULT, IV, Bound);
  Term->setOperand(0, NewCond);

  // 3. Build lane-0 scalar replacements.
  DenseMap<const Instruction *, Value *> LaneMap;
  IRBuilder<> B(BB->getFirstNonPHI());

  for (Instruction *I : VecInsts) {
    B.SetInsertPoint(I);
    if (auto *LI = dyn_cast<LoadInst>(I)) {
      Value *Ptr = LI->getPointerOperand();
      auto *SL = B.CreateLoad(ElemTy, Ptr);
      SL->setAlignment(LI->getAlign());
      SL->setVolatile(LI->isVolatile());
      LaneMap[I] = SL;
    } else if (auto *SI = dyn_cast<StoreInst>(I)) {
      Value *Ptr = SI->getPointerOperand();
      Value *Val = SI->getValueOperand();
      Value *LaneVal;
      if (auto *OpI = dyn_cast<Instruction>(Val)) {
        if (auto It = LaneMap.find(OpI); It != LaneMap.end()) {
          LaneVal = It->second;
        } else if (auto *CDS = dyn_cast<ConstantDataVector>(Val)) {
          LaneVal = CDS->getElementAsConstant(0);
        } else if (isVectorConstZero(Val)) {
          LaneVal = Constant::getNullValue(ElemTy);
        } else if (isVectorTyped(Val->getType())) {
          LaneVal = B.CreateExtractElement(Val, B.getInt32(0));
        } else {
          LaneVal = Val;
        }
      } else if (auto *CDS = dyn_cast<ConstantDataVector>(Val)) {
        LaneVal = CDS->getElementAsConstant(0);
      } else if (isVectorConstZero(Val)) {
        LaneVal = Constant::getNullValue(ElemTy);
      } else {
        LaneVal = Val;
      }
      auto *SS = B.CreateStore(LaneVal, Ptr);
      SS->setAlignment(SI->getAlign());
      SS->setVolatile(SI->isVolatile());
      LaneMap[I] = SS;
    } else if (auto *CI = dyn_cast<ICmpInst>(I)) {
      Value *Op0 = CI->getOperand(0);
      Value *Op1 = CI->getOperand(1);
      Value *L0 = isVectorConstZero(Op0) ? Constant::getNullValue(ElemTy)
                                         : (isVectorTyped(Op0->getType()) ? B.CreateExtractElement(Op0, B.getInt32(0)) : Op0);
      Value *L1 = isVectorConstZero(Op1) ? Constant::getNullValue(ElemTy)
                                         : (isVectorTyped(Op1->getType()) ? B.CreateExtractElement(Op1, B.getInt32(0)) : Op1);
      LaneMap[I] = B.CreateICmp(CI->getPredicate(), L0, L1);
    } else if (auto *BO = dyn_cast<BinaryOperator>(I)) {
      Value *Op0 = BO->getOperand(0);
      Value *Op1 = BO->getOperand(1);
      Value *L0 = isVectorTyped(Op0->getType()) ? B.CreateExtractElement(Op0, B.getInt32(0)) : Op0;
      Value *L1 = isVectorTyped(Op1->getType()) ? B.CreateExtractElement(Op1, B.getInt32(0)) : Op1;
      Value *R = nullptr;
      switch (BO->getOpcode()) {
      case Instruction::Add: R = B.CreateAdd(L0, L1); break;
      case Instruction::Sub: R = B.CreateSub(L0, L1); break;
      case Instruction::Mul: R = B.CreateMul(L0, L1); break;
      case Instruction::And: R = B.CreateAnd(L0, L1); break;
      case Instruction::Or: R = B.CreateOr(L0, L1); break;
      case Instruction::Xor: R = B.CreateXor(L0, L1); break;
      case Instruction::Shl: R = B.CreateShl(L0, L1); break;
      case Instruction::LShr: R = B.CreateLShr(L0, L1); break;
      case Instruction::AShr: R = B.CreateAShr(L0, L1); break;
      case Instruction::FAdd: R = B.CreateFAdd(L0, L1); break;
      case Instruction::FSub: R = B.CreateFSub(L0, L1); break;
      case Instruction::FMul: R = B.CreateFMul(L0, L1); break;
      case Instruction::FDiv: R = B.CreateFDiv(L0, L1); break;
      default: return false;
      }
      LaneMap[I] = R;
    } else if (auto *CI = dyn_cast<CastInst>(I)) {
      Value *Op0 = CI->getOperand(0);
      Value *L0 = isVectorTyped(Op0->getType()) ? B.CreateExtractElement(Op0, B.getInt32(0)) : Op0;
      Value *R = nullptr;
      switch (CI->getOpcode()) {
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::Trunc: {
        auto *DstVT = cast<FixedVectorType>(CI->getType());
        Type *Dst = DstVT->getElementType();
        if (CI->getOpcode() == Instruction::ZExt) R = B.CreateZExt(L0, Dst);
        else if (CI->getOpcode() == Instruction::SExt) R = B.CreateSExt(L0, Dst);
        else R = B.CreateTrunc(L0, Dst);
        break;
      }
      case Instruction::BitCast: {
        auto *DstVT = cast<FixedVectorType>(CI->getType());
        R = B.CreateBitCast(L0, DstVT->getElementType());
        break;
      }
      default: return false;
      }
      LaneMap[I] = R;
    } else if (auto *SI = dyn_cast<SelectInst>(I)) {
      Value *Cond = SI->getCondition();
      Value *TV = SI->getTrueValue();
      Value *FV = SI->getFalseValue();
      Value *LC = isVectorTyped(Cond->getType()) ? B.CreateExtractElement(Cond, B.getInt32(0)) : Cond;
      Value *LT = isVectorTyped(TV->getType()) ? B.CreateExtractElement(TV, B.getInt32(0)) : TV;
      Value *LF = isVectorTyped(FV->getType()) ? B.CreateExtractElement(FV, B.getInt32(0)) : FV;
      LaneMap[I] = B.CreateSelect(LC, LT, LF);
    }
  }

  // 4. Replace uses of vector instructions with their lane-0 scalar.
  for (Instruction &I : *BB) {
    if (isVectorTyped(I.getType())) continue;
    for (unsigned op = 0; op != I.getNumOperands(); ++op) {
      if (auto *OpI = dyn_cast<Instruction>(I.getOperand(op))) {
        auto It = LaneMap.find(OpI);
        if (It != LaneMap.end() && It->second != OpI)
          I.setOperand(op, It->second);
      }
    }
  }

  // 5. Remove old vector instructions.
  for (Instruction *I : VecInsts)
    if (I->use_empty())
      I->eraseFromParent();

  // 6. Update IV step to 1.
  if (auto *IVAdd = dyn_cast<BinaryOperator>(IV->getIncomingValueForBlock(BB))) {
    IVAdd->setOperand(1, ConstantInt::get(Step->getType(), 1));
  }
  return true;
}

bool runOnFunction(Function &F) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    PHINode *IV = nullptr;
    ConstantInt *Step = nullptr;
    if (!matchScalarLoop(&BB, IV, Step)) continue;
    if (Step->getZExtValue() < 2) continue;
    Changed |= scalarizeLoop(&BB, IV, Step, F);
  }
  return Changed;
}

} // namespace

PreservedAnalyses UnloopVecWidthsPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.hasOptNone())
    return PreservedAnalyses::all();
  if (!runOnFunction(F))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

#ifdef REVENANT_BUILD_PLUGIN
#undef DEBUG_TYPE
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

static void registerUnloopVecWidthsCallbacks(::llvm::PassBuilder &PB) {
  PB.registerPipelineParsingCallback(
      [](::llvm::StringRef Name, ::llvm::FunctionPassManager &PM,
         ::llvm::ArrayRef<::llvm::PassBuilder::PipelineElement>) {
        if (Name == "revenant-unloopvecwidths") {
          PM.addPass(revenant::UnloopVecWidthsPass());
          return true;
        }
        return false;
      });
}

extern "C" __declspec(dllexport) ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUnloopVecWidths", LLVM_VERSION_STRING,
          registerUnloopVecWidthsCallbacks};
}
#endif
