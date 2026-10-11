//===- UncxxSupport.cpp - flavor, emission, purity helpers --*- C++ -*-===//
//
// Part of the Revenant LLVM fork. Implements the UncxxUtil.h helpers
// that need a Module or scan instructions (flavor inference, helper
// declaration/emission, callee classification, purity checks).
//
//===----------------------------------------------------------------------===//

#include "UncxxUtil.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"

#include <cstdlib>

using namespace llvm;

namespace revenant {
namespace uncxx {

// The flag is owned by this lib (parse-once, never required): plugin
// path reads the environment instead, because stock opt parses its own
// options before the plugin loads and plugin cl::opts never reach it.
static cl::opt<std::string> UncxxStl(
    "revenant-uncxx-stl",
    cl::desc("STL flavor for revenant-uncxx: auto, msvc, gnu, libcxx"),
    cl::init("auto"));

static CxxFlavor parseFlavor(StringRef S) {
  if (S == "msvc")
    return CxxFlavor::Msvc;
  if (S == "gnu" || S == "libstdc++" || S == "gcc")
    return CxxFlavor::Gnu;
  if (S == "libcxx" || S == "libc++")
    return CxxFlavor::Libcxx;
  return CxxFlavor::Auto;
}

const char *flavorName(CxxFlavor Fl) {
  switch (Fl) {
  case CxxFlavor::Msvc:
    return "msvc";
  case CxxFlavor::Gnu:
    return "gnu";
  case CxxFlavor::Libcxx:
    return "libcxx";
  case CxxFlavor::Auto:
    return "auto";
  }
  return "auto";
}

CxxFlavor flavorOf(const Module &M) {
  // Explicit flag wins.
  std::string Flag = UncxxStl;
  if (const char *Env = std::getenv("REVENANT_UNCXX_STL"))
    if (!UncxxStl.getNumOccurrences())
      Flag = Env;
  CxxFlavor Explicit = parseFlavor(Flag);
  if (Explicit != CxxFlavor::Auto)
    return Explicit;
  // Infer from module symbols.
  bool HasMsvc = false, HasItanium = false, HasLibcxx = false;
  for (const Function &F : M) {
    StringRef N = F.getName();
    if (N.contains("std::__1") || N.contains("_LIBCPP_"))
      HasLibcxx = true;
    else if (N.starts_with("?") && N.contains("@std@@"))
      HasMsvc = true;
    else if (N.starts_with("_Z") || N.contains("_ZSt") ||
             N.contains("@GLIBCXX"))
      HasItanium = true;
    if (N.contains("__CxxFrameHandler3") || N.contains("??_7"))
      HasMsvc = true;
  }
  for (const GlobalVariable &G : M.globals()) {
    StringRef N = G.getName();
    if (N.contains("std::__1") || N.contains("_LIBCPP_"))
      HasLibcxx = true;
    else if (N.starts_with("?") && N.contains("@std@@"))
      HasMsvc = true;
  }
  if (HasLibcxx)
    return CxxFlavor::Libcxx;
  if (HasMsvc && !HasItanium)
    return CxxFlavor::Msvc;
  if (HasItanium && !HasMsvc)
    return CxxFlavor::Gnu;
  if (HasMsvc)
    return CxxFlavor::Msvc; // mixed MSVC/Itanium: MSVC owns the STL
  return CxxFlavor::Auto;
}

Function *cxxHelper(Module &M, StringRef Name, Type *RetTy,
                    ArrayRef<Type *> ArgTys) {
  LLVMContext &C = M.getContext();
  FunctionType *FT = FunctionType::get(RetTy, ArgTys, false);
  std::string Full = ("revenant.cxx." + Name).str();
  if (Function *F = M.getFunction(Full)) {
    if (F->getFunctionType() == FT)
      return F;
    // Signature clash (e.g. element-type-specific index helpers):
    // keep both under suffixed names.
    for (unsigned i = 2;; ++i) {
      std::string Alt = Full + "." + std::to_string(i);
      if (Function *G = M.getFunction(Alt)) {
        if (G->getFunctionType() == FT)
          return G;
        continue;
      }
      Function *G =
          Function::Create(FT, GlobalValue::ExternalLinkage, Alt, M);
      return G;
    }
  }
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, Full, M);
  return F;
}

void copyProv(llvm::Instruction &Dst, const llvm::Instruction &Src) {
  if (llvm::MDNode *VA = Src.getMetadata("revenant.va"))
    Dst.setMetadata("revenant.va", VA);
  else {
    llvm::LLVMContext &C = Dst.getContext();
    llvm::Metadata *Ops[] = {llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 0))};
    Dst.setMetadata("revenant.va", llvm::MDNode::get(C, Ops));
  }
  Dst.setDebugLoc(Src.getDebugLoc());
}

CallInst *emitCxxCall(Module &M, StringRef Name, Type *RetTy,
                      ArrayRef<Value *> Args, Instruction *Before,
                      const Instruction *Prov) {
  SmallVector<Type *, 4> AT;
  for (Value *A : Args)
    AT.push_back(A->getType());
  Function *H = cxxHelper(M, Name, RetTy, AT);
  CallInst *C = CallInst::Create(H->getFunctionType(), H, Args, "", Before);
  C->setCallingConv(CallingConv::C);
  if (Prov)
    copyProv(*C, *Prov);
  return C;
}

void eraseIfDead(Instruction *I) {
  if (!I || !I->use_empty())
    return;
  // Only erase side-effect-free instructions: callers guarantee the
  // matched region is pure, but double-check atomics/volatile here.
  if (I->mayHaveSideEffects() || I->mayReadOrWriteMemory()) {
    if (!isa<LoadInst>(I) && !isa<GetElementPtrInst>(I) &&
        !isa<CastInst>(I) && !isa<BinaryOperator>(I) &&
        !isa<CmpInst>(I) && !isa<SelectInst>(I) && !isa<PHINode>(I))
      return;
    if (auto *L = dyn_cast<LoadInst>(I))
      if (!L->isUnordered())
        return;
  }
  if (I->isTerminator())
    return;
  SmallVector<Value *, 4> Ops(I->op_begin(), I->op_end());
  I->eraseFromParent();
  for (Value *O : Ops)
    if (auto *IO = dyn_cast<Instruction>(O))
      eraseIfDead(IO);
}

bool isCxxHelperCall(const CallBase &CB) {
  const Function *F = CB.getCalledFunction();
  return F && F->getName().consume_front("revenant.cxx.");
}

bool isDeleteCall(const CallBase &CB) {
  const Function *F = CB.getCalledFunction();
  if (!F)
    return false;
  StringRef N = F->getName();
  // MSVC `??3@YAXPEAX...` / `??3@YAXPEAX_K@Z`, Itanium `_ZdlPv[_...]`,
  // C `free`, Windows `RtlFreeHeap`.
  if (N.contains("??3@"))
    return true;
  if (N == "_ZdlPv" || N.starts_with("_ZdlPv") || N == "_ZdaPv" ||
      N.starts_with("_ZdaPv"))
    return true;
  if (N == "free" || N == "_free_base" || N == "RtlFreeHeap")
    return true;
  return false;
}

bool isDeallocCall(const CallBase &CB) {
  if (isDeleteCall(CB))
    return true;
  const Function *F = CB.getCalledFunction();
  if (!F)
    return false;
  StringRef N = F->getName();
  // Container deallocators route through operator delete eventually;
  // match the direct spellings so tidy diamonds fire without knowing
  // the container type.
  if (N.contains("_Deallocate") || N.contains("deallocate@") ||
      N.contains("Deallocate@") || N.contains("operator delete"))
    return true;
  return false;
}

bool isTrapBlock(const BasicBlock &BB) {
  for (const Instruction &I : BB) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      const Function *F = CB->getCalledFunction();
      if (F && (F->getIntrinsicID() == llvm::Intrinsic::trap ||
                F->getName().contains("trap") ||
                F->getName().contains("Xfail") ||
                F->getName().contains("Throw") ||
                F->getName().contains("_Xlen") ||
                F->getName().contains("_Xran")))
        return true;
      if (CB->hasFnAttr(Attribute::NoReturn))
        return true;
    }
    if (isa<UnreachableInst>(&I))
      return true;
  }
  return false;
}

bool dominates(const Instruction &Def, const Instruction &Use) {
  const BasicBlock *DB = Def.getParent(), *UB = Use.getParent();
  if (DB == UB) {
    for (auto It = DB->begin();; ++It) {
      if (&*It == &Def)
        return true;
      if (&*It == &Use)
        return false;
    }
  }
  // Every path entry->UB must cross DB: backward search from UB for a
  // Def-free path to the entry. Bounded (functions here are small).
  SmallVector<const BasicBlock *, 16> Work({UB});
  SmallPtrSet<const BasicBlock *, 16> Seen({UB});
  const Function *F = UB->getParent();
  const BasicBlock &Entry = F->getEntryBlock();
  for (unsigned Step = 0; Step < 256 && !Work.empty(); ++Step) {
    const BasicBlock *B = Work.pop_back_val();
    if (B == DB)
      continue; // paths through DB are covered
    if (B == &Entry)
      return false; // Def-free path to entry exists
    for (const BasicBlock *P : predecessors(B)) {
      if (P == DB)
        continue;
      if (Seen.insert(P).second)
        Work.push_back(P);
    }
  }
  return Work.empty(); // exhausted without finding a Def-free path
}

bool rangeIsPure(BasicBlock::iterator First, BasicBlock::iterator Last) {
  for (auto It = First;; ++It) {
    const Instruction &I = *It;
    if (hasOpaque(I))
      return false;
    if (I.mayHaveSideEffects() || I.mayReadOrWriteMemory()) {
      // Unordered loads and llvm markers are fine.
      if (auto *L = dyn_cast<LoadInst>(&I)) {
        if (!L->isUnordered())
          return false;
      } else if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Function *F = CB->getCalledFunction();
        if (!F || !F->isIntrinsic())
          return false;
      } else {
        return false;
      }
    }
    if (It == Last)
      return true;
  }
}

bool allUsersInPureRange(Value *V, BasicBlock *BB,
                         BasicBlock::iterator From) {
  // Collect the users first: walking `V->users()` in list order makes the
  // result depend on use-list order, which is not visible in the IR text
  // and differs between an in-memory and a re-parsed module. Take the
  // last user in program order instead, which is what the range needs.
  SmallPtrSet<const Instruction *, 8> Users;
  for (User *U : V->users()) {
    auto *UI = dyn_cast<Instruction>(U);
    if (!UI || UI->getParent() != BB)
      return false;
    Users.insert(UI);
  }
  if (Users.empty())
    return true;
  BasicBlock::iterator LastUse = BB->end();
  for (Instruction &I : *BB)
    if (Users.contains(&I))
      LastUse = I.getIterator();
  if (LastUse == BB->end())
    return true;
  return rangeIsPure(From, LastUse);
}

} // namespace uncxx
} // namespace revenant
