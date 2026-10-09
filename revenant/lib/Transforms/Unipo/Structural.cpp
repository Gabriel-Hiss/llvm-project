// Structural specialization recovery. No symbol or oracle data is consulted.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace {
bool eligible(Function &F) {
  if (F.isDeclaration() || F.isVarArg() || F.hasPersonalityFn() ||
      F.hasDLLExportStorageClass() || F.hasAddressTaken())
    return false;
  for (Argument &A : F.args())
    if (F.getAttributes().getParamAttrs(A.getArgNo()).hasAttributes())
      return false;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      // These annotations describe instructions/regions that normalization
      // must not erase. Provenance is retained on the original definitions.
      if (I.hasMetadata("revenant.opaque") || I.hasMetadata("revenant.region"))
        return false;
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (!isa<CallInst>(CB) || CB->isTailCall() || CB->hasOperandBundles() ||
            !CB->getCalledFunction() || CB->getCalledFunction() == &F ||
            CB->getCalledFunction()->getName().starts_with("revenant.asm."))
          return false;
    }
  return true;
}

// Constants are deliberately abstracted only in the candidate hash. The final
// equality check below compares their exact values and referenced globals.
hash_code bodyHash(Function &F) {
  hash_code H = hash_value(F.size());
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      H = hash_combine(H, I.getOpcode(), I.getType(), I.getNumOperands());
      for (Value *V : I.operands())
        H = hash_combine(H, V->getType(), isa<Constant>(V));
    }
  return H;
}

bool equalBodies(Function &A, Function &B, unsigned P, bool Dropped) {
  if (A.size() != B.size())
    return false;
  DenseMap<const Value *, const Value *> Map;
  for (unsigned I = 0, J = 0; I < A.arg_size(); ++I) {
    if (Dropped && I == P)
      continue;
    if (J >= B.arg_size() || A.getArg(I)->getType() != B.getArg(J)->getType())
      return false;
    Map[A.getArg(I)] = B.getArg(J++);
  }
  auto BI = B.begin();
  for (BasicBlock &BB : A) {
    BasicBlock &Other = *BI++;
    if (BB.size() != Other.size())
      return false;
    Map[&BB] = &Other;
    auto II = Other.begin();
    for (Instruction &I : BB)
      Map[&I] = &*II++;
  }
  BI = B.begin();
  for (BasicBlock &BB : A) {
    auto II = BI++->begin();
    for (Instruction &I : BB) {
      Instruction &J = *II++;
      if (!I.isSameOperationAs(&J))
        return false;
      // Alias/range metadata can affect semantics, unlike names and VAs.
      SmallVector<std::pair<unsigned, MDNode *>, 4> AM, BM;
      I.getAllMetadataOtherThanDebugLoc(AM);
      J.getAllMetadataOtherThanDebugLoc(BM);
      unsigned VA = A.getContext().getMDKindID("revenant.va");
      llvm::erase_if(AM, [VA](auto &M) { return M.first == VA; });
      llvm::erase_if(BM, [VA](auto &M) { return M.first == VA; });
      if (AM != BM)
        return false;
      for (unsigned K = 0; K < I.getNumOperands(); ++K) {
        Value *V = I.getOperand(K), *W = J.getOperand(K);
        auto It = Map.find(V);
        if (It != Map.end() ? It->second != W : V != W)
          return false;
      }
    }
  }
  return true;
}

Function *normalized(Function &F, unsigned P, Constant *C) {
  ValueToValueMapTy Map;
  Function *Copy = CloneFunction(&F, Map);
  if (C)
    Copy->getArg(P)->replaceAllUsesWith(C);
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  FunctionPassManager PM;
  PM.addPass(InstCombinePass());
  PM.addPass(SimplifyCFGPass());
  PM.addPass(SCCPPass());
  PM.run(*Copy, FAM);
  return Copy;
}

bool rewritable(Function &F) {
  if (F.use_empty())
    return false;
  for (User *U : F.users()) {
    auto *CI = dyn_cast<CallInst>(U);
    if (!CI || CI->getCalledFunction() != &F || CI->isTailCall() ||
        CI->hasOperandBundles() || CI->hasMetadata("revenant.opaque"))
      return false;
    for (unsigned I = 0; I < CI->arg_size(); ++I)
      if (CI->getAttributes().getParamAttrs(I).hasAttributes())
        return false;
  }
  return true;
}

void redirect(Function &A, Function &B, unsigned P, Constant *C, bool Dropped) {
  SmallVector<CallInst *, 8> Calls;
  for (User *U : B.users())
    Calls.push_back(cast<CallInst>(U));
  for (CallInst *Old : Calls) {
    SmallVector<Value *, 8> Args;
    for (unsigned I = 0, J = 0; I < A.arg_size(); ++I) {
      if (I == P) {
        Args.push_back(C);
        if (!Dropped)
          ++J;
      } else
        Args.push_back(Old->getArgOperand(J++));
    }
    CallInst *New = CallInst::Create(A.getFunctionType(), &A, Args);
    New->insertBefore(Old->getIterator());
    New->setCallingConv(A.getCallingConv());
    New->setAttributes(Old->getAttributes());
    New->copyMetadata(*Old);
    New->setDebugLoc(Old->getDebugLoc());
    New->takeName(Old);
    Old->replaceAllUsesWith(New);
    Old->eraseFromParent();
  }
  B.eraseFromParent();
}
} // namespace

namespace revenant {
bool foldStructuralIPO(Module &M) {
  bool Changed = false;
  SmallVector<Function *, 32> Functions;
  for (Function &F : M)
    if (eligible(F))
      Functions.push_back(&F);
  // Remove erased entries immediately, rather than retaining dangling worklist
  // pointers. Normalized copies never become candidates themselves.
  for (unsigned BI = 0; BI < Functions.size();) {
    Function &B = *Functions[BI];
    bool Folded = false;
    Function *DroppedBody = nullptr;
    if (rewritable(B))
      for (Function *AP : Functions) {
        Function &A = *AP;
        bool Dropped = A.arg_size() == B.arg_size() + 1;
        if (&A == &B || (!Dropped && A.arg_size() != B.arg_size()) ||
            A.getReturnType() != B.getReturnType() ||
            A.getCallingConv() != B.getCallingConv() ||
            A.getAttributes().getFnAttrs().removeAttribute(M.getContext(), "revenant.va") !=
                B.getAttributes().getFnAttrs().removeAttribute(M.getContext(), "revenant.va") ||
            A.getAttributes().getRetAttrs() != B.getAttributes().getRetAttrs())
          continue;
        for (unsigned P = 0; P < A.arg_size() && !Folded; ++P) {
          bool TypesMatch = true;
          for (unsigned I = 0, J = 0; I < A.arg_size(); ++I) {
            if (Dropped && I == P)
              continue;
            if (A.getArg(I)->getType() != B.getArg(J++)->getType())
              TypesMatch = false;
          }
          if (!TypesMatch)
            continue;
          SmallVector<Constant *, 8> Constants;
          if (!Dropped) {
            Constant *C = nullptr;
            bool Same = true;
            for (User *U : B.users()) {
              auto *V = dyn_cast<Constant>(cast<CallInst>(U)->getArgOperand(P));
              if (!V || isa<UndefValue>(V) || isa<PoisonValue>(V) || (C && C != V)) {
                Same = false;
                break;
              }
              C = V;
            }
            if (Same && C)
              Constants.push_back(C);
          } else {
            // Constants that vanished during folding can survive in the
            // caller's lifted register stores, rather than B's body.
            static const char *Regs[] = {"rcx", "rdx", "r8", "r9"};
            if (M.getTargetTriple().isOSWindows() && P < 4)
              for (User *U : B.users()) {
                auto *Call = cast<CallInst>(U);
                for (auto It = Call->getIterator();
                     It != Call->getParent()->begin();) {
                  Instruction &I = *--It;
                  if (isa<CallBase>(I))
                    break;
                  auto *S = dyn_cast<StoreInst>(&I);
                  if (!S) {
                    if (I.mayWriteToMemory())
                      break;
                    continue;
                  }
                  auto *Slot = dyn_cast<AllocaInst>(S->getPointerOperand());
                  if (!Slot)
                    break;
                  if (Slot->getName() != Regs[P])
                    continue;
                  auto *C = dyn_cast<ConstantInt>(S->getValueOperand());
                  if (C && C->getType() == A.getArg(P)->getType() &&
                      !llvm::is_contained(Constants, C))
                    Constants.push_back(C);
                  break;
                }
              }
            for (BasicBlock &BB : B)
              for (Instruction &I : BB)
                for (Value *V : I.operands())
                  if (auto *C = dyn_cast<Constant>(V))
                    if (C->getType() == A.getArg(P)->getType() &&
                        !isa<UndefValue>(C) && !isa<PoisonValue>(C) &&
                        !isa<GlobalValue>(C) &&
                        !llvm::is_contained(Constants, C))
                      Constants.push_back(C);
          }
          for (Constant *C : Constants) {
            if (C->getType() != A.getArg(P)->getType())
              continue;
            Function *AC = normalized(A, P, C);
            if (Dropped && !DroppedBody)
              DroppedBody = normalized(B, P, nullptr);
            Function *BC = Dropped ? DroppedBody : normalized(B, P, C);
            bool Match = bodyHash(*AC) == bodyHash(*BC) &&
                         equalBodies(*AC, *BC, P, Dropped);
            AC->eraseFromParent();
            if (!Dropped)
              BC->eraseFromParent();
            if (!Match)
              continue;
            redirect(A, B, P, C, Dropped);
            Functions.erase(Functions.begin() + BI);
            Folded = Changed = true;
            break;
          }
        }
        if (Folded)
          break;
      }
    if (DroppedBody)
      DroppedBody->eraseFromParent();
    if (!Folded)
      ++BI;
  }
  return Changed;
}
} // namespace revenant
