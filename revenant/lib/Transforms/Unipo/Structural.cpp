// Structural clone folding requires equal recovered signatures.
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
#include <cstdint>
#include <optional>

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

bool differsOnlyByTrapStub(const Constant *A, const Constant *B) {
  if (A == B)
    return false;
  const auto *FA = dyn_cast<Function>(A);
  const auto *FB = dyn_cast<Function>(B);
  if (FA || FB)
    return FA && FB && globalEquivalent(FA, FB) &&
           FA->getName().starts_with("sub_0x") &&
           FB->getName().starts_with("sub_0x");
  const auto *EA = dyn_cast<ConstantExpr>(A);
  const auto *EB = dyn_cast<ConstantExpr>(B);
  if (!EA || !EB || EA->getNumOperands() != EB->getNumOperands())
    return false;
  for (unsigned I = 0; I < EA->getNumOperands(); ++I)
    if (differsOnlyByTrapStub(EA->getOperand(I), EB->getOperand(I)))
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
bool equalBodies(Function &A, Function &B, bool *SawDataGlobal = nullptr,
                 bool *SawTrapStub = nullptr) {
  if (A.size() != B.size() || A.arg_size() != B.arg_size())
    return false;
  DenseMap<const Value *, const Value *> Map;
  for (unsigned I = 0; I < A.arg_size(); ++I) {
    if (A.getArg(I)->getType() != B.getArg(I)->getType())
      return false;
    Map[A.getArg(I)] = B.getArg(I);
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
          auto *CV = dyn_cast<Constant>(V);
          auto *CW = dyn_cast<Constant>(W);
          if (!CV || !CW || !revenant::constantsEquivalent(CV, CW))
            return false;
          if (SawDataGlobal && differsOnlyByDataGlobal(CV, CW))
            *SawDataGlobal = true;
          if (SawTrapStub && differsOnlyByTrapStub(CV, CW))
            *SawTrapStub = true;
        }
      }
    }
  }
  return true;
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

void redirect(Function &A, Function &B) {
  SmallVector<CallInst *, 8> Calls;
  for (User *U : B.users())
    Calls.push_back(cast<CallInst>(U));
  for (CallInst *Old : Calls) {
    SmallVector<Value *, 8> Args;
    for (Use &Arg : Old->args())
      Args.push_back(Arg.get());
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
  // This matcher only folds same-signature bodies. A body that matches after
  // substituting a callsite constant is not evidence that two entry points
  // came from one source function. In stripped lifts that heuristic merged
  // unrelated functions and changed their recovered signatures.
  for (unsigned BI = 0; BI < Functions.size();) {
    Function &B = *Functions[BI];
    bool Folded = false;
    if (rewritable(B))
      for (unsigned AI = 0; AI < Functions.size() && !Folded; ++AI) {
        Function &A = *Functions[AI];
        if (&A == &B || A.arg_size() != B.arg_size() ||
            !sameFunctionAttributes(A, B) ||
            A.getAttributes().getRetAttrs() != B.getAttributes().getRetAttrs())
          continue;
        if (bodyHash(A) != bodyHash(B))
          continue;
        std::optional<uint64_t> AV = functionVA(A);
        std::optional<uint64_t> BV = functionVA(B);
        bool Earlier = AV && BV ? *AV < *BV : AI < BI;
        bool DataGlobal = false;
        bool TrapStub = false;
        bool BodiesEqual =
            Earlier && equalBodies(A, B, &DataGlobal, &TrapStub);
        if (BodiesEqual && DataGlobal && TrapStub) {
          redirect(A, B);
          Functions.erase(Functions.begin() + BI);
          Folded = Changed = true;
        }
      }
    if (!Folded)
      ++BI;
  }
  return Changed;
}
} // namespace revenant
