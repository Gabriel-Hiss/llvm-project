// Structural specialization recovery. No symbol or oracle data is consulted.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include <optional>
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace {

// Lifted IR carries one data object per virtual address, not per source
// object. A compiler that deduplicated identical bytes leaves several
// @g_/@str_/@a globals with the same initializer, and the same VA can be
// re-emitted under a dotted suffix. Two globals are equivalent when they
// describe the same bytes: identical !revenant.va, or same-typed internal
// constants with identical initializers.
bool globalAddressCompared(const GlobalVariable &GV) {
  SmallVector<const Value *, 8> Worklist{&GV};
  SmallPtrSet<const Value *, 16> Seen;
  while (!Worklist.empty()) {
    const Value *V = Worklist.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (const User *U : V->users()) {
      if (isa<ICmpInst>(U) || isa<StoreInst>(U) || isa<ReturnInst>(U))
        return true;
      if (const auto *CB = dyn_cast<CallBase>(U)) {
        if (CB->getCalledOperand() == V)
          return true;
        // Scalar diagnostic-call arguments do not expose the pointer value.
        continue;
      }
      if (isa<LoadInst>(U))
        continue;
      if (isa<Constant>(U) || isa<Instruction>(U)) {
        Worklist.push_back(cast<Value>(U));
        continue;
      }
      return true;
    }
  }
  return false;
}
std::optional<uint64_t> globalVA(const GlobalObject &GV) {
  MDNode *N =
      GV.getMetadata(GV.getContext().getMDKindID("revenant.va"));
  if (!N || N->getNumOperands() != 1)
    return std::nullopt;
  auto *V = mdconst::dyn_extract<ConstantInt>(N->getOperand(0));
  return V ? std::optional<uint64_t>(V->getZExtValue()) : std::nullopt;
}

bool globalEquivalent(const GlobalValue *A, const GlobalValue *B) {
  if (A == B)
    return true;
  if (const auto *FA = dyn_cast<Function>(A)) {
    // Tail-call/trap stubs: the lifter emits a no-body `sub_0x<va>`
    // declaration for the trap bytes reached after a noreturn call.
    // Same-type stubs are the same unreachable marker; their distinct
    // VAs live only in the removed !revenant.va metadata.
    const auto *FB = dyn_cast<Function>(B);
    if (!FB || !FA->isDeclaration() || !FB->isDeclaration())
      return false;
    if (FA->getFunctionType() != FB->getFunctionType())
      return false;
    return FA->getName().starts_with("sub_0x") &&
           FB->getName().starts_with("sub_0x");
  }
  const auto *VA = dyn_cast<GlobalVariable>(A);
  const auto *VB = dyn_cast<GlobalVariable>(B);
  if (!VA || !VB || VA->getValueType() != VB->getValueType())
    return false;
  std::optional<uint64_t> X = globalVA(*VA);
  std::optional<uint64_t> Y = globalVA(*VB);
  if (X && Y && *X == *Y)
    return true;
  return VA->isConstant() && VB->isConstant() && VA->hasInitializer() &&
         VB->hasInitializer() && VA->getInitializer() == VB->getInitializer() &&
         !globalAddressCompared(*VA) && !globalAddressCompared(*VB);
}


// Within an operand pair that is already constantsEquivalent: did the
// equality rely on dedup'd constant data (a GlobalVariable pair), rather
// than identical sub-trees or trap-stub decls?
bool differsOnlyByDataGlobal(const Constant *A, const Constant *B) {
  if (A == B)
    return false;
  if (const auto *GA = dyn_cast<GlobalVariable>(A))
    return isa<GlobalVariable>(B) &&
           globalEquivalent(GA, cast<GlobalVariable>(B));
  if (isa<GlobalValue>(A))
    return false; // trap stubs and other globals are not data
  const auto *EA = dyn_cast<ConstantExpr>(A);
  const auto *EB = dyn_cast<ConstantExpr>(B);
  if (!EA || !EB || EA->getNumOperands() != EB->getNumOperands())
    return false;
  for (unsigned I = 0; I < EA->getNumOperands(); ++I)
    if (differsOnlyByDataGlobal(EA->getOperand(I), EB->getOperand(I)))
      return true;
  return false;
}

// `sub_<hexva>` lifted names carry the entry address; used to prefer the
// lowest-VA equivalent as the fold survivor (GCC keeps the general body
// at its own address and places clones at fresh addresses).
std::optional<uint64_t> functionVA(const Function &F) {
  StringRef N = F.getName();
  if (!N.consume_front("sub_"))
    return std::nullopt;
  uint64_t V = 0;
  if (N.getAsInteger(16, V))
    return std::nullopt;
  return V;
}

bool sameFunctionAttributes(const Function &A, const Function &B) {
  SmallVector<Attribute, 8> AA, BA;
  for (Attribute Attr : A.getAttributes().getFnAttrs())
    if (!Attr.isStringAttribute() ||
        Attr.getKindAsString() != "revenant.va")
      AA.push_back(Attr);
  for (Attribute Attr : B.getAttributes().getFnAttrs())
    if (!Attr.isStringAttribute() ||
        Attr.getKindAsString() != "revenant.va")
      BA.push_back(Attr);
  return AA == BA;
}
} // namespace

namespace revenant {
// Constant equivalence modulo dedup'd constant data and trap-stub
// declarations; ConstantExprs compare structurally through it.
bool constantsEquivalent(const Constant *A, const Constant *B) {
  if (A == B)
    return true;
  if (!A || !B || A->getType() != B->getType())
    return false;
  if (const auto *GA = dyn_cast<GlobalValue>(A))
    return isa<GlobalValue>(B) &&
           globalEquivalent(GA, cast<GlobalValue>(B));
  const auto *EA = dyn_cast<ConstantExpr>(A);
  const auto *EB = dyn_cast<ConstantExpr>(B);
  if (!EA || !EB || EA->getOpcode() != EB->getOpcode() ||
      EA->getNumOperands() != EB->getNumOperands())
    return false;
  for (unsigned I = 0; I < EA->getNumOperands(); ++I)
    if (!constantsEquivalent(EA->getOperand(I), EB->getOperand(I)))
      return false;
  return true;
}
} // namespace revenant

namespace {

bool eligible(Function &F) {
  if (F.isDeclaration() || F.isVarArg() || F.hasPersonalityFn() ||
      F.hasDLLExportStorageClass() || F.hasAddressTaken())
    return false;
  // `.part.N` and `.cold` symbols are outlined regions of their parent
  // (ipa-split/hot-cold partitioning), not specializations; merging them
  // is uninline's job. Lifted `sub_<va>` names never match this check.
  if (F.getName().contains(".part.") || F.getName().contains(".cold"))
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
        H = hash_combine(H, V->getType());
    }
  return H;
}

// `SawDataGlobal` (optional) records that at least one operand pair was
// equal only through GlobalVariable equivalence (dedup'd constant data),
// so the bodies are not literally identical. Trap-stub-decl equivalence
// alone does not set it: identical throw/trap tails are far more common
// than clones and are not IPO evidence by themselves.
bool equalBodies(Function &A, Function &B, unsigned P, bool Dropped,
                 bool *SawDataGlobal = nullptr) {
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
        if (It != Map.end()) {
          if (It->second != W)
            return false;
        } else if (V != W) {
          // Constants differ only by pointer: equal iff they describe the
          // same dedup'd data or the same trap-stub decl.
          auto *CV = dyn_cast<Constant>(V);
          auto *CW = dyn_cast<Constant>(W);
          if (!CV || !CW || !revenant::constantsEquivalent(CV, CW))
            return false;
          if (SawDataGlobal && differsOnlyByDataGlobal(CV, CW))
            *SawDataGlobal = true;
        }
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
        // C == nullptr: the folded-out formal is provably dead in A
        // (equalBodies matched with it unbound), so poison is correct.
        Args.push_back(C ? (Value *)C
                         : (Value *)PoisonValue::get(A.getArg(I)->getType()));
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
  // worklist pointers. Normalized copies never become candidates.
  for (unsigned BI = 0; BI < Functions.size();) {
    Function &B = *Functions[BI];
    bool Folded = false;
    Function *DroppedBody = nullptr;
    if (rewritable(B))
      for (unsigned AI = 0; AI < Functions.size() && !Folded; ++AI) {
        Function &A = *Functions[AI];
        bool Dropped = A.arg_size() == B.arg_size() + 1;
        if (&A == &B || (!Dropped && A.arg_size() != B.arg_size()) ||
            !sameFunctionAttributes(A, B) ||
            A.getAttributes().getRetAttrs() != B.getAttributes().getRetAttrs())
          continue;
        if (bodyHash(A) != bodyHash(B))
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
          // A same-arity specialization must bind a live formal. A dropped
          // formal may be dead: its value is irrelevant and callers can pass
          // poison when rebuilding the general signature.
          if (!Dropped && A.getArg(P)->use_empty())
            continue;
          SmallVector<Constant *, 8> Constants;
          if (!Dropped) {
            // Same arity: only a constant B's callers unanimously pass.
            Constant *C = nullptr;
            bool Same = true;
            for (User *U : B.users()) {
              auto *V =
                  dyn_cast<Constant>(cast<CallInst>(U)->getArgOperand(P));
              if (!V || isa<UndefValue>(V) || isa<PoisonValue>(V) ||
                  (C && C != V)) {
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
        // Same-arity clone with no surviving specialization constant: the
        // bodies are equal modulo dedup'd constant data (the GCC .isra
        // shape that differs only in a pooled string). Literal-identical
        // bodies are linker-folded duplicates, not compiler clones, and
        // trap-stub-only differences merge unrelated throw/cold tails, so
        // require at least one dedup'd-data operand pair. Survivor: the
        // lowest entry VA (clone addresses are fresh), falling back to
        // module order when names lack VAs.
        if (!Folded && !Dropped) {
          std::optional<uint64_t> AV = functionVA(A);
          std::optional<uint64_t> BV = functionVA(B);
          bool Earlier = AV && BV ? *AV < *BV : AI < BI;
          bool DataGlobal = false;
          bool BodiesEqual =
              Earlier && bodyHash(A) == bodyHash(B) &&
              equalBodies(A, B, ~0u, false, &DataGlobal);
          if (BodiesEqual && DataGlobal) {
            redirect(A, B, ~0u, nullptr, false);
            Functions.erase(Functions.begin() + BI);
            Folded = Changed = true;
          }
        }
      }
    if (DroppedBody)
      DroppedBody->eraseFromParent();
    if (!Folded)
      ++BI;
  }
  return Changed;
}
} // namespace revenant
