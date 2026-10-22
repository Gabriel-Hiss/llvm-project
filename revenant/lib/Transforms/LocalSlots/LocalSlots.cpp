//===- LocalSlots.cpp - restore pre-optimization local storage -----------===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/LocalSlots/LocalSlots.h"
#include "revenant/Transforms/SsaOut/PhiCopies.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <string>

using namespace llvm;

namespace revenant {
namespace {

Instruction *firstNonAlloca(BasicBlock &Entry) {
  auto It = Entry.begin();
  while (It != Entry.end() && isa<AllocaInst>(*It))
    ++It;
  return It == Entry.end() ? Entry.getTerminator() : &*It;
}

// Provenance only: the source's other metadata (`!nonnull`, `!range`,
// `!noundef` on a load) describes that instruction's value and is invalid
// or wrong on a new store or load.
void copyProvenance(const Instruction &From, Instruction &To) {
  if (MDNode *VA = From.getMetadata("revenant.va"))
    To.setMetadata("revenant.va", VA);
  To.setDebugLoc(From.getDebugLoc());
}

/// A load replacing a PHI operand must live on the incoming edge, not in the
/// PHI's block (a PHI cannot reference a value defined in its own block).
Instruction *useInsertionPoint(Use &U, Instruction &User) {
  if (auto *PN = dyn_cast<PHINode>(&User)) {
    unsigned Incoming = PN->getIncomingValueNumForOperand(U.getOperandNo());
    return PN->getIncomingBlock(Incoming)->getTerminator();
  }
  return &User;
}

struct SplitEdgeMap {
  BasicBlock *Pred;
  BasicBlock *Succ;
  BasicBlock *Edge;
};
struct CopyPlan {
  PHINode *Phi;
  BasicBlock *Pred;
  BasicBlock *Succ;
  Value *Incoming;
  AllocaInst *Slot;
};

std::string localName(const PHINode &PN) {
  if (PN.hasName())
    return ("v_" + PN.getName()).str();
  return "v_phi";
}

} // namespace

PreservedAnalyses LocalSlotsPass::run(Function &F,
                                      FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.empty())
    return PreservedAnalyses::all();

  BasicBlock &Entry = F.getEntryBlock();
  LLVMContext &Ctx = F.getContext();
  bool Changed = false;

  SmallVector<AllocaInst *, 8> ParamSlots;
  Instruction *Before = Entry.empty() ? nullptr : &Entry.front();
  for (Argument &Arg : F.args()) {
    std::string Name =
        Arg.hasName() ? Arg.getName().str() : "a" + std::to_string(Arg.getArgNo());
    IRBuilder<> B(Before ? Before : Entry.getTerminator());
    ParamSlots.push_back(
        B.CreateAlloca(Arg.getType(), nullptr, Name + ".addr"));
    Changed = true;
  }
  if (!ParamSlots.empty()) {
    IRBuilder<> B(firstNonAlloca(Entry));
    for (unsigned I = 0; I != F.arg_size(); ++I)
      B.CreateStore(F.getArg(I), ParamSlots[I]);
  }

  SmallVector<PHINode *, 32> Phis;
  DenseMap<PHINode *, unsigned> PhiIndex;
  for (BasicBlock &BB : F)
    for (PHINode &PN : BB.phis()) {
      PhiIndex[&PN] = Phis.size();
      Phis.push_back(&PN);
    }
  DenseMap<PHINode *, char> ReturnVars;
  for (BasicBlock &BB : F)
    if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
      if (auto *PN = dyn_cast_or_null<PHINode>(RI->getReturnValue()))
        ReturnVars[PN] = 1;
  SmallVector<unsigned, 32> Parent;
  Parent.reserve(Phis.size());
  for (unsigned I = 0; I != Phis.size(); ++I)
    Parent.push_back(I);
  auto Root = [&](unsigned I) {
    while (Parent[I] != I) {
      Parent[I] = Parent[Parent[I]];
      I = Parent[I];
    }
    return I;
  };
  // A phi whose only use is another phi shares that phi's local, unless
  // the two hold live values at once (`x = phi [y]` next to `y = phi
  // [i + 1]` in one loop header: x lags y by an iteration).
  PhiLiveness Liveness;
  SmallVector<SmallVector<PHINode *, 2>, 32> Members;
  for (PHINode *PN : Phis)
    Members.push_back({PN});
  for (unsigned I = 0; I != Phis.size(); ++I) {
    PHINode *PN = Phis[I];
    for (Value *V : PN->incoming_values()) {
      auto *Other = dyn_cast<PHINode>(V);
      if (!Other)
        continue;
      auto It = PhiIndex.find(Other);
      if (It == PhiIndex.end() || Other == PN ||
          Other->getType() != PN->getType() || !Other->hasOneUse())
        continue;
      bool OnlyUseIsThisPhi = false;
      for (Use &U : Other->uses())
        OnlyUseIsThisPhi |= U.getUser() == PN;
      if (!OnlyUseIsThisPhi)
        continue;
      unsigned A = Root(I), B = Root(It->second);
      if (A == B || Liveness.interferes(Members[A], Members[B]))
        continue;
      Parent[B] = A;
      Members[A].append(Members[B].begin(), Members[B].end());
      Members[B].clear();
    }
  }

  DenseMap<unsigned, AllocaInst *> Slots;
  for (unsigned I = 0; I != Phis.size(); ++I) {
    unsigned R = Root(I);
    if (Slots.count(R))
      continue;
    PHINode *PN = Phis[I];
    IRBuilder<> B(firstNonAlloca(Entry));
    std::string Name =
        ReturnVars.count(PN) ? "retval" : localName(*PN);
    AllocaInst *Slot = B.CreateAlloca(PN->getType(), nullptr, Name);
    Slots[R] = Slot;
    Changed = true;
  }

  if (!Slots.empty()) {
    Module *M = F.getParent();
    DICompileUnit *CU = nullptr;
    if (NamedMDNode *CUs = M->getNamedMetadata("llvm.dbg.cu"))
      if (CUs->getNumOperands())
        CU = dyn_cast<DICompileUnit>(CUs->getOperand(0));
    DIBuilder DIB(*M, /*AllowUnresolved=*/false, CU);
    DISubprogram *SP = F.getSubprogram();
    DIFile *File = SP ? SP->getFile() : nullptr;
    for (auto &KV : Slots) {
      AllocaInst *Slot = KV.second;
      DIType *Ty = nullptr;
      Type *AllocTy = Slot->getAllocatedType();
      if (AllocTy->isIntegerTy(1))
        Ty = DIB.createBasicType("bool", 1, dwarf::DW_ATE_boolean);
      else if (AllocTy->isIntegerTy())
        Ty = DIB.createBasicType("long", AllocTy->getIntegerBitWidth(),
                                 dwarf::DW_ATE_signed);
      else if (AllocTy->isFloatingPointTy())
        Ty = DIB.createBasicType("double", 64, dwarf::DW_ATE_float);
      else
        Ty = DIB.createBasicType("ptr", 64, dwarf::DW_ATE_address);
      if (SP && File) {
        DILocalVariable *Var = DIB.createAutoVariable(
            SP, Slot->getName(), File, 1, Ty);
        DIB.insertDeclare(Slot, Var, DIB.createExpression(),
                          DILocation::get(Ctx, 1, 0, SP),
                          Slot->getNextNode());
      }
    }
    DIB.finalize();
  }

  // The local of web R still holds a value some path reads at the start
  // of BB: a member is live there, or a member is defined there (its
  // copy can be skipped as a same-local copy that relies on the value).
  auto SlotLiveAt = [&](unsigned R, BasicBlock *BB) {
    for (PHINode *M : Members[R])
      if (M->getParent() == BB || Liveness.liveIn(M).count(BB))
        return true;
    return false;
  };
  // An incoming value defined in its predecessor is stored right after
  // its definition, as clang -O0 stores an assignment, when nothing reads
  // the local between that point and the edge and no other successor of
  // the predecessor still needs the old value.
  auto EarlyStoreSafe = [&](unsigned R, Instruction *Def, BasicBlock *Succ) {
    BasicBlock *Pred = Def->getParent();
    for (PHINode *M : Members[R])
      for (Use &U : M->uses()) {
        auto *UserI = cast<Instruction>(U.getUser());
        if (auto *UserPhi = dyn_cast<PHINode>(UserI)) {
          if (UserPhi->getIncomingBlock(U) == Pred)
            return false;
        } else if (UserI->getParent() == Pred && Def->comesBefore(UserI)) {
          return false;
        }
      }
    for (BasicBlock *Other : successors(Pred))
      if (Other != Succ && SlotLiveAt(R, Other))
        return false;
    return true;
  };

  SmallVector<CopyPlan, 64> EdgeCopies;
  for (unsigned I = 0; I != Phis.size(); ++I) {
    PHINode *PN = Phis[I];
    AllocaInst *Slot = Slots.lookup(Root(I));
    for (unsigned J = 0; J != PN->getNumIncomingValues(); ++J) {
      Value *V = PN->getIncomingValue(J);
      BasicBlock *Pred = PN->getIncomingBlock(J);
      if (auto *Other = dyn_cast<PHINode>(V))
        if (PhiIndex.count(Other) && Root(PhiIndex.lookup(Other)) == Root(I))
          continue;
      auto *Def = dyn_cast<Instruction>(V);
      if (Def && Def->getParent() == Pred && !Def->isTerminator() &&
          !isa<PHINode>(Def) && EarlyStoreSafe(Root(I), Def, PN->getParent())) {
        // An alloca incoming sits among the entry allocas, before the
        // slots: its store goes after them.
        IRBuilder<> B(isa<AllocaInst>(Def) && Def->getParent() == &Entry
                          ? firstNonAlloca(Entry)
                          : Def->getNextNode());
        StoreInst *Store = B.CreateStore(V, Slot);
        copyProvenance(*Def, *Store);
        continue;
      }
      EdgeCopies.push_back({PN, Pred, PN->getParent(), V, Slot});
    }
  }

  // A copy goes on its own edge block when the predecessor branches
  // elsewhere too and the successor has other predecessors, or when
  // another successor still reads the local the copy writes.
  SmallVector<SplitEdgeMap, 16> SplitEdges;
  for (const CopyPlan &P : EdgeCopies) {
    if (succ_size(P.Pred) < 2)
      continue;
    if (pred_size(P.Succ) < 2) {
      unsigned R = Root(PhiIndex.lookup(P.Phi));
      bool OtherReads = false;
      for (BasicBlock *Other : successors(P.Pred))
        OtherReads |= Other != P.Succ && SlotLiveAt(R, Other);
      if (!OtherReads)
        continue;
    }
    bool Seen = false;
    for (const SplitEdgeMap &E : SplitEdges)
      Seen |= E.Pred == P.Pred && E.Succ == P.Succ;
    if (Seen)
      continue;
    BasicBlock *Edge = SplitCriticalEdge(P.Pred, P.Succ);
    if (!Edge)
      Edge = SplitEdge(P.Pred, P.Succ);
    if (!Edge)
      continue;
    Edge->moveAfter(&F.back());
    SplitEdges.push_back({P.Pred, P.Succ, Edge});
  }
  // The copies that land in one block are one parallel set: each reads the
  // old value of the local it reads, so they are sequenced on those reads.
  MapVector<BasicBlock *, SmallVector<EdgeCopy, 4>> CopiesByBlock;
  for (const CopyPlan &P : EdgeCopies) {
    BasicBlock *StoreBlock = P.Pred;
    for (const SplitEdgeMap &E : SplitEdges)
      if (E.Pred == P.Pred && E.Succ == P.Succ) {
        StoreBlock = E.Edge;
        break;
      }
    AllocaInst *ReadSlot = nullptr;
    if (auto *IncomingPhi = dyn_cast<PHINode>(P.Incoming))
      if (auto It = PhiIndex.find(IncomingPhi); It != PhiIndex.end())
        ReadSlot = Slots.lookup(Root(It->second));
    CopiesByBlock[StoreBlock].push_back({P.Slot, P.Incoming, ReadSlot, P.Phi});
  }
  DenseMap<Type *, AllocaInst *> SpillByType;
  auto SpillFor = [&](Type *Ty) {
    AllocaInst *&Spill = SpillByType[Ty];
    if (!Spill) {
      IRBuilder<> B(firstNonAlloca(Entry));
      Spill = B.CreateAlloca(Ty, nullptr, "v_spill");
    }
    return Spill;
  };
  for (auto &[StoreBlock, Copies] : CopiesByBlock) {
    IRBuilder<> B(StoreBlock->getTerminator());
    for (const EdgeCopy &C : sequenceEdgeCopies(std::move(Copies), SpillFor)) {
      Value *Stored = C.V;
      if (C.ReadSlot) {
        LoadInst *Load =
            B.CreateLoad(C.ReadSlot->getAllocatedType(), C.ReadSlot);
        copyProvenance(*C.Origin, *Load);
        Stored = Load;
      } else if (auto *Arg = dyn_cast<Argument>(Stored)) {
        LoadInst *Load =
            B.CreateLoad(Arg->getType(), ParamSlots[Arg->getArgNo()]);
        copyProvenance(*C.Origin, *Load);
        Stored = Load;
      }
      StoreInst *Store = B.CreateStore(Stored, C.Dst);
      copyProvenance(*C.Origin, *Store);
    }
  }

  for (PHINode *PN : Phis) {
    AllocaInst *Slot = Slots.lookup(Root(PhiIndex.lookup(PN)));
    // Collect first: replacing a use while walking the use list would let the
    // freshly inserted load's operands perturb the iteration.
    SmallVector<Use *, 16> Uses;
    for (Use &U : PN->uses())
      if (!isa<PHINode>(U.getUser()))
        Uses.push_back(&U);
    for (Use *UP : Uses) {
      Use &U = *UP;
      auto *UserI = dyn_cast<Instruction>(U.getUser());
      if (!UserI)
        continue;
      IRBuilder<> B(UserI);
      LoadInst *Load = B.CreateLoad(PN->getType(), Slot);
      copyProvenance(*UserI, *Load);
      U.set(Load);
    }
  }
  for (PHINode *PN : Phis) {
    PN->replaceAllUsesWith(UndefValue::get(PN->getType()));
    PN->eraseFromParent();
  }
  unsigned ArgIndex = 0;
  for (Argument &Arg : F.args()) {
    AllocaInst *Slot = ParamSlots[ArgIndex++];
    SmallVector<Use *, 16> Uses;
    for (Use &U : Arg.uses()) {
      if (isa<DbgInfoIntrinsic>(U.getUser()))
        continue;
      auto *Store = dyn_cast<StoreInst>(U.getUser());
      if (Store && Store->getPointerOperand() == Slot &&
          Store->getValueOperand() == &Arg)
        continue;
      Uses.push_back(&U);
    }
    for (Use *UP : Uses) {
      Use &U = *UP;
      auto *UserI = dyn_cast<Instruction>(U.getUser());
      if (!UserI)
        continue;
      IRBuilder<> B(useInsertionPoint(U, *UserI));
      LoadInst *Load = B.CreateLoad(Arg.getType(), Slot);
      copyProvenance(*UserI, *Load);
      U.set(Load);
    }
  }

  SmallVector<ReturnInst *, 4> Returns;
  bool HasMustTailReturn = false;
  for (BasicBlock &BB : F)
    if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
      Returns.push_back(RI);
      if (auto *CI = dyn_cast_or_null<CallInst>(RI->getPrevNode()))
        HasMustTailReturn |= CI->isMustTailCall();
    }
  if (!F.getReturnType()->isVoidTy() && Returns.size() > 1 &&
      ReturnVars.empty() && !HasMustTailReturn) {
    IRBuilder<> B(firstNonAlloca(Entry));
    AllocaInst *RetSlot = B.CreateAlloca(F.getReturnType(), nullptr, "retval");
    for (ReturnInst *RI : Returns) {
      Value *Returned = RI->getReturnValue();
      IRBuilder<> RB(RI);
      StoreInst *Store = RB.CreateStore(Returned, RetSlot);
      copyProvenance(*RI, *Store);
      LoadInst *Load = RB.CreateLoad(F.getReturnType(), RetSlot);
      copyProvenance(*RI, *Load);
      RI->setOperand(0, Load);
    }
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace revenant

#ifdef REVENANT_BUILD_PLUGIN
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantTransformLocalSlots",
          LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-locals-to-slots") {
                    PM.addPass(revenant::LocalSlotsPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
