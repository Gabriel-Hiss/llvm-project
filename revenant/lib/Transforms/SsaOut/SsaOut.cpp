//===- SsaOut.cpp - SSA-out with phi-web coalescing ---------------------===//
//
// Part of the Revenant LLVM fork.
//
// Port of the SSA-out half of `crates/structure` (phi coalescing,
// expression propagation, local materialization from `block_stmts`) as an
// LLVM function pass. See SsaOut.h for the phase order and guarantees.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/SsaOut/SsaOut.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <map>
#include <string>

using namespace llvm;

namespace revenant {
namespace {
// A phi web: phi destinations that feed each other through phi incoming
// edges share one local. `canon` resolves an SSA name to its web's
// canonical member (hop-bounded; a cycle means a missed coalesce, never
// a hang).
class PhiWebs {
public:
  void build(Function &F) {
    for (BasicBlock &BB : F) {
      for (PHINode &PN : BB.phis()) {
        PhiDsts[PN.getName().str()] = 1;
      }
    }
    for (BasicBlock &BB : F) {
      for (PHINode &PN : BB.phis()) {
        std::string Dst = PN.getName().str();
        for (Value *V : PN.incoming_values()) {
          if (!V->hasName())
            continue;
          std::string N = V->getName().str();
          if (!PhiDsts.count(N))
            continue;
          std::string D = canon(Dst);
          std::string S = canon(N);
          if (D != S)
            Alias[S] = D;
        }
      }
    }
  }

  std::string canon(StringRef N) const { return canon(N.str()); }
  std::string canon(const std::string &N) const {
    std::string C = N;
    for (unsigned I = 0; I < 64; ++I) {
      auto It = Alias.find(C);
      if (It == Alias.end())
        break;
      C = It->second;
    }
    return C;
  }

  bool isPhiDst(StringRef N) const {
    return PhiDsts.count(N.str()) != 0;
  }

private:
  // Small-string-keyed sets: phi names are short; std::map keeps this
  // header-free (no DenseMapInfo<StringRef> specialization needed).
  std::map<std::string, char> PhiDsts;
  std::map<std::string, std::string> Alias;
};

// Use counts over the function (phi incoming edges count as uses, as do
// terminator operands: a load feeding `ret`/`br`/`switch` must still
// materialize).
DenseMap<Value *, unsigned> collectUses(Function &F) {
  DenseMap<Value *, unsigned> Uses;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      for (Value *Op : I.operands()) {
        if (isa<Instruction>(Op) || isa<Argument>(Op))
          Uses[Op]++;
      }
    }
  }
  return Uses;
}

// Pure and cheap to leave for the frontend to inline: binop, cmp, cast,
// gep, select, extractvalue, and single-use calls. Loads, atomics,
// allocas and multi-use calls always materialize. Mirrors
// `inlineable_by_kind` in `crates/structure`.
bool inlineableByKind(Instruction *I, unsigned Uses) {
  if (isa<LoadInst>(I) || isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I) ||
      isa<AllocaInst>(I) || isa<StoreInst>(I) || isa<PHINode>(I))
    return false;
  if (isa<CallInst>(I) || isa<InvokeInst>(I))
    return Uses <= 1;
  if (isa<BinaryOperator>(I) || isa<CmpInst>(I) || isa<CastInst>(I) ||
      isa<GetElementPtrInst>(I) || isa<SelectInst>(I) ||
      isa<ExtractValueInst>(I))
    return Uses <= 1 || true; // cheap nodes propagate at every use
  return Uses <= 1;
}

// Copy provenance (`!revenant.va`) and debug location onto a new
// instruction. Opaque markers (`!revenant.opaque`) live on the original
// call and are never moved; new stores/loads need no marker.
void copyProvenance(const Instruction *Src, Instruction *Dst) {
  // Copy only revenant.va and dbg; nonnull/noundef on the source describe
  // the original pointer/value and are invalid on a fresh alloca or store.
  if (MDNode *VA = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", VA);
  Dst->setDebugLoc(Src->getDebugLoc());
}

std::string sanitize(StringRef N) {
  std::string S;
  for (char C : N) {
    if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
        (C >= '0' && C <= '9') || C == '_' || C == '.')
      S += C;
    else
      S += '_';
  }
  if (S.empty())
    S = "tmp";
  return S;
}

// DILocation stores its line in 32 bits; use the low 32 bits of the
// instruction VA as a stable location, or zero when provenance is absent.
unsigned debugLineFromVA(const Instruction &I) {
  MDNode *VA = I.getMetadata("revenant.va");
  if (!VA || VA->getNumOperands() == 0)
    return 0;
  auto *CAM = dyn_cast<ConstantAsMetadata>(VA->getOperand(0));
  auto *CI = CAM ? dyn_cast<ConstantInt>(CAM->getValue()) : nullptr;
  return CI ? static_cast<unsigned>(CI->getZExtValue()) : 0;
}

bool attachMissingCallLocations(Function &F, DISubprogram &SP) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!isa<CallBase>(I) || I.getDebugLoc())
        continue;
      I.setDebugLoc(DILocation::get(F.getContext(), debugLineFromVA(I), 0,
                                    &SP));
      Changed = true;
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses SsaOutPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration() || F.empty())
    return PreservedAnalyses::all();

  PhiWebs Webs;
  Webs.build(F);
  DenseMap<Value *, unsigned> Uses = collectUses(F);
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  // Reuse the module's compile unit when one exists; otherwise create
  // the shared one (producer "revenant", optimized) FIRST: DIBuilder
  // refuses to make types without one. Language follows `revenant.lang`.
  DICompileUnit *CU = nullptr;
  DIFile *File = nullptr;
  DISubprogram *SP = F.getSubprogram();
  bool Changed = false;
  if (NamedMDNode *CUs = M->getNamedMetadata("llvm.dbg.cu")) {
    if (CUs->getNumOperands() > 0)
      CU = dyn_cast<DICompileUnit>(CUs->getOperand(0));
  }
  // Scope the builder to the existing CU so created subprograms link
  // back to it; a fresh builder with no CU leaves definitions unit-less
  // and the verifier rejects the second function in the module.
  DIBuilder DIB(*M, /*AllowUnresolved=*/false, CU);
  if (CU) {
    File = CU->getFile();
    if (!File)
      File = DIB.createFile("revenant.c", "revenant");
  } else {
    File = DIB.createFile("revenant.c", "revenant");
    unsigned Lang = dwarf::DW_LANG_C;
    if (Metadata *LangFlag = M->getModuleFlag("revenant.lang")) {
      if (MDString *S = dyn_cast<MDString>(LangFlag)) {
        if (S->getString() == "cpp")
          Lang = dwarf::DW_LANG_C_plus_plus;
        else if (S->getString() == "rust")
          Lang = dwarf::DW_LANG_Rust;
      }
    }
    CU = DIB.createCompileUnit(Lang, File, "revenant",
                               /*isOptimized=*/true, "", /*RV=*/0);
    // Debug Info Version flag, required whenever DI is present.
    if (!M->getModuleFlag("Debug Info Version"))
      M->addModuleFlag(Module::Warning, "Debug Info Version", 3);
  }

  // Function DISubprogram when missing (frontends read names/types from
  // DI). Signature mirrors the IR signature with pointer-sized unknowns.
  if (!SP) {
    SmallVector<Metadata *, 8> SubTys;
    DIType *Int64Ty = DIB.createBasicType("long", 64, dwarf::DW_ATE_signed);
    if (F.getReturnType()->isVoidTy())
      SubTys.push_back(nullptr);
    else
      SubTys.push_back(Int64Ty);
    for (unsigned I = 0, E = F.arg_size(); I != E; ++I)
      SubTys.push_back(Int64Ty);
    DISubroutineType *SubTy =
        DIB.createSubroutineType(DIB.getOrCreateTypeArray(SubTys));
    SP = DIB.createFunction(CU, F.getName(), F.getName(), File, 1, SubTy, 1,
                            DINode::FlagZero,
                            DISubprogram::SPFlagDefinition |
                                DISubprogram::SPFlagOptimized);
    F.setSubprogram(SP);
    Changed = true;
  }
  // SSA-out can introduce the first !dbg scope, notably when replaying the
  // pipeline with revenant-types disabled. Give every call a location now;
  // otherwise LLVM rejects inlinable calls in this debug-info function.
  Changed |= attachMissingCallLocations(F, *SP);

  // One local per phi web + per materialized def. Keyed by canonical
  // name so web members share the slot.
  DenseMap<Value *, AllocaInst *> Materialized;
  std::map<std::string, AllocaInst *> LocalByName;
  DenseMap<AllocaInst *, DILocalVariable *> LocalVar;
  unsigned TmpN = 0;

  auto getOrCreateLocal = [&](std::string Key, Type *Ty,
                              const Instruction *Provenance) -> AllocaInst * {
    auto It = LocalByName.find(Key);
    if (It != LocalByName.end())
      return It->second;
    IRBuilder<> B(&F.getEntryBlock(), F.getEntryBlock().begin());
    B.SetCurrentDebugLocation(DebugLoc());
    AllocaInst *AI = B.CreateAlloca(Ty, nullptr, "v_" + sanitize(Key));
    if (Provenance)
      copyProvenance(Provenance, AI);
    // Named local with a DI type: i64 for ints/pointers, double for
    // floating point. Frontends read the name and type from DI.
    DIType *DT = nullptr;
    if (Ty->isFloatingPointTy())
      DT = DIB.createBasicType("double", 64, dwarf::DW_ATE_float);
    else if (Ty->isIntegerTy(1))
      DT = DIB.createBasicType("bool", 1, dwarf::DW_ATE_boolean);
    else if (Ty->isIntegerTy())
      DT = DIB.createBasicType("long", Ty->getIntegerBitWidth(),
                               dwarf::DW_ATE_signed);
    else
      DT = DIB.createBasicType("ptr", 64, dwarf::DW_ATE_address);
    DILocalVariable *Var =
        DIB.createAutoVariable(SP, AI->getName(), File, 1, DT);
    DIB.insertDeclare(AI, Var, DIB.createExpression(),
                      DILocation::get(Ctx, 1, 0, SP), AI->getNextNode());
    LocalByName[Key] = AI;
    LocalVar[AI] = Var;
    Changed = true;
    return AI;
  };
  // Web key: the canonical name, qualified by type when the same web name
  // already holds a slot of another type (`rax.0` i64 next to `rax.1` i32
  // must not share one alloca).
  auto keyFor = [&](StringRef Name, Type *Ty) -> std::string {
    std::string Base = Webs.canon(Name);
    auto It = LocalByName.find(Base);
    if (It == LocalByName.end() || It->second->getAllocatedType() == Ty)
      return Base;
    std::string TS;
    raw_string_ostream(TS) << *Ty;
    return Base + "." + TS;
  };
  // Demote materialized defs: every non-inlineable instruction with
  // uses gets a local; its def becomes `store`, each use a `load`.
  // Phis always demote (their web's local).
  SmallVector<Instruction *, 32> DefOrder;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (!I.getType()->isVoidTy() && !I.getType()->isTokenTy() &&
          !I.getType()->isLabelTy() && !I.getType()->isMetadataTy() &&
          !isa<PHINode>(&I) && !I.use_empty())
        DefOrder.push_back(&I);

  for (Instruction *I : DefOrder) {
    unsigned U = Uses.lookup(I);
    if (inlineableByKind(I, U))
      continue; // stays inline for the frontend
    std::string Key;
    if (I->hasName())
      Key = keyFor(I->getName(), I->getType());
    else
      Key = "tmp" + std::to_string(TmpN++);
    AllocaInst *AI = getOrCreateLocal(Key, I->getType(), I);
    Materialized[I] = AI;
  }

  // Phi webs always get a local, even before uses are rewritten.
  for (BasicBlock &BB : F) {
    for (PHINode &PN : BB.phis()) {
      std::string Key = keyFor(PN.getName(), PN.getType());
      AllocaInst *AI = getOrCreateLocal(Key, PN.getType(), &PN);
      Materialized[&PN] = AI;
    }
  }
  // Emit the store for each materialized non-phi def right after it,
  // then rewrite its non-phi uses to loads. One reverse pass: a use
  // whose def is also materialized gets its load before the def's own
  // store is visited, so chained defs resolve correctly.
  for (auto RI = DefOrder.rbegin(), RE = DefOrder.rend(); RI != RE; ++RI) {
    Instruction *I = *RI;
    auto It = Materialized.find(I);
    if (It == Materialized.end())
      continue;
    AllocaInst *AI = It->second;
    // Rewrite non-phi uses first (phi operands resolve through edge
    // copies below, not here).
    SmallVector<Instruction *, 8> LoadUsers;
    for (Use &U : I->uses()) {
      if (isa<PHINode>(U.getUser()))
        continue;
      LoadUsers.push_back(cast<Instruction>(U.getUser()));
    }
    for (Instruction *User : LoadUsers) {
      IRBuilder<> B(User);
      B.SetCurrentDebugLocation(User->getDebugLoc());
      LoadInst *LD = B.CreateLoad(AI->getAllocatedType(), AI);
      copyProvenance(User, LD);
      for (Use &U : User->operands()) {
        if (U.get() == I)
          U.set(LD);
      }
    }
    Instruction *InsertBefore = I->getNextNode();
    if (auto *II = dyn_cast<InvokeInst>(I)) {
      BasicBlock *NormalDest = II->getNormalDest();
      if (pred_size(NormalDest) > 1) {
        if (BasicBlock *Edge =
                SplitCriticalEdge(II->getParent(), NormalDest)) {
          // `!revenant.region` operands use block-list indices.
          Edge->moveAfter(&F.back());
          NormalDest = Edge;
          Changed = true;
        }
      }
      InsertBefore = NormalDest->getFirstNonPHI();
    }
    IRBuilder<> B(InsertBefore);
    B.SetCurrentDebugLocation(I->getDebugLoc());
    StoreInst *ST = B.CreateStore(I, AI);
    copyProvenance(I, ST);
  }
  // Reads of a phi web become loads of the web's local, placed before
  // the reader. Runs before copy insertion so latch reads see the slot;
  // the edge copies keep the slot current on every incoming edge.
  for (BasicBlock &BB : F) {
    // Collect first: rewriting mutates the use lists.
    SmallVector<Instruction *, 8> Readers;
    for (Instruction &I : BB) {
      if (isa<PHINode>(&I))
        continue;
      bool ReadsPhi = false;
      for (Value *Op : I.operands()) {
        if (isa<PHINode>(Op)) {
          ReadsPhi = true;
          break;
        }
      }
      if (ReadsPhi)
        Readers.push_back(&I);
    }
    for (Instruction *User : Readers) {
      IRBuilder<> B(User);
      B.SetCurrentDebugLocation(User->getDebugLoc());
      // One load per distinct phi operand.
      DenseMap<PHINode *, LoadInst *> Loads;
      for (Use &U : User->operands()) {
        PHINode *PN = dyn_cast<PHINode>(U.get());
        if (!PN)
          continue;
        auto MIt = Materialized.find(PN);
        if (MIt == Materialized.end())
          continue;
        LoadInst *LD = Loads.lookup(PN);
        if (!LD) {
          LD = B.CreateLoad(MIt->second->getAllocatedType(), MIt->second);
          copyProvenance(User, LD);
          Loads[PN] = LD;
        }
        U.set(LD);
      }
    }
  }
  // 2. Split critical edges into phi blocks so each copy runs only on
  // its own edge. Region annotations ride along to the new terminator.
  for (BasicBlock &BB : F) {
    SmallVector<BasicBlock *, 4> Succs(succ_begin(&BB), succ_end(&BB));
    Instruction *PredTerm = BB.getTerminator();
    if (Succs.size() < 2 || BB.isEHPad() ||
        isa<CatchSwitchInst>(PredTerm) || isa<CatchReturnInst>(PredTerm) ||
        isa<CleanupReturnInst>(PredTerm))
      continue;
    for (BasicBlock *Succ : Succs) {
      if (Succ->phis().empty() || Succ->isEHPad())
        continue;
      if (BasicBlock *EdgeBB = SplitCriticalEdge(&BB, Succ)) {
        // `!revenant.region` operands are block-list indices; inserting
        // the split block mid-list would shift every later index and
        // mis-decode annotations written before this pass. Keep the new
        // block at the end so existing indices stay valid.
        EdgeBB->moveAfter(&F.back());
        Changed = true;
      }
    }
  }

  // 3. Phi copy insertion: for each edge pred -> succ, `store incoming,
  // web-local` at the end of pred. Copies on one edge are parallel, so
  // order them: a copy is safe once no other pending copy reads its
  // destination; cycles spill through a temporary.
  // Batch per successor block: gather (pred, incoming) pairs per phi.
  for (BasicBlock &Succ : F) {
    if (Succ.phis().empty())
      continue;
    // pred -> list of (dst local, incoming value)
    std::map<BasicBlock *, SmallVector<std::pair<AllocaInst *, Value *>, 4>>
        EdgeCopies;
    for (PHINode &PN : Succ.phis()) {
      std::string Key = keyFor(PN.getName(), PN.getType());
      auto LIt = LocalByName.find(Key);
      if (LIt == LocalByName.end())
        continue;
      AllocaInst *AI = LIt->second;
      for (unsigned I = 0, E = PN.getNumIncomingValues(); I != E; ++I) {
        BasicBlock *Pred = PN.getIncomingBlock(I);
        Value *V = PN.getIncomingValue(I);
        // Resolve through materialized locals: a load at the copy site.
        // (Loads are inserted when the edge is processed below.)
        EdgeCopies[Pred].push_back({AI, V});
      }
    }
    // The copies must be emitted in a fixed order: a map keyed by block
    // pointer iterates in address order, which differs per process and
    // would give the emitted loads (and the names they get) a different
    // order in every run.
    SmallVector<BasicBlock *, 8> EdgeOrder;
    for (auto &EC : EdgeCopies)
      EdgeOrder.push_back(EC.first);
    {
      DenseMap<const BasicBlock *, unsigned> BlockPos;
      unsigned Pos = 0;
      for (BasicBlock &BB : F)
        BlockPos[&BB] = Pos++;
      llvm::sort(EdgeOrder, [&](BasicBlock *A, BasicBlock *B) {
        return BlockPos.lookup(A) < BlockPos.lookup(B);
      });
    }
    for (BasicBlock *Pred : EdgeOrder) {
      auto &Copies = EdgeCopies[Pred];
      // Order: repeatedly emit a copy whose destination no other pending
      // copy reads; on a cycle, spill one destination through a temp.
      SmallVector<std::pair<AllocaInst *, Value *>, 4> Pending = Copies;
      SmallVector<std::pair<AllocaInst *, Value *>, 4> Ordered;
      AllocaInst *Spill = nullptr;
      Value *SpilledVal = nullptr;
      while (!Pending.empty()) {
        bool Progress = false;
        for (size_t I = 0; I < Pending.size(); ++I) {
          AllocaInst *Dst = Pending[I].first;
          bool Read = false;
          for (size_t J = 0; J < Pending.size(); ++J) {
            if (I == J)
              continue;
            if (Pending[J].second == Dst) {
              Read = true;
              break;
            }
          }
          if (!Read) {
            Ordered.push_back(Pending[I]);
            Pending.erase(Pending.begin() + I);
            Progress = true;
            break;
          }
        }
        if (!Progress) {
          // Cycle: spill Pending[0]'s destination first.
          AllocaInst *Dst = Pending[0].first;
          if (!Spill) {
            IRBuilder<> B(&F.getEntryBlock(), F.getEntryBlock().begin());
            Spill = B.CreateAlloca(Dst->getAllocatedType(), nullptr,
                                   "v_spill");
            DIType *SpillTy = nullptr;
            if (Dst->getAllocatedType()->isFloatingPointTy())
              SpillTy = DIB.createBasicType("double", 64, dwarf::DW_ATE_float);
            else if (Dst->getAllocatedType()->isIntegerTy(1))
              SpillTy = DIB.createBasicType("bool", 1, dwarf::DW_ATE_boolean);
            else if (Dst->getAllocatedType()->isIntegerTy())
              SpillTy = DIB.createBasicType("long",
                                           Dst->getAllocatedType()->getIntegerBitWidth(),
                                           dwarf::DW_ATE_signed);
            else
              SpillTy = DIB.createBasicType("ptr", 64, dwarf::DW_ATE_address);
            DILocalVariable *SpillVar =
                DIB.createAutoVariable(SP, Spill->getName(), File, 1, SpillTy);
            DIB.insertDeclare(Spill, SpillVar, DIB.createExpression(),
                              DILocation::get(Ctx, 1, 0, SP), Spill->getNextNode());
          }
          SpilledVal = Dst;
          Ordered.push_back({Spill, Pending[0].second});
          Pending.erase(Pending.begin());
          // Retarget remaining reads of Dst to the spill slot.
          for (auto &P : Pending) {
            if (P.second == Dst)
              P.second = Spill;
          }
          (void)SpilledVal;
        }
      }
      Instruction *Term = Pred->getTerminator();
      // A catchswitch must be the first non-PHI instruction in its block.
      // Its handler choice also makes a pre-switch copy incorrect for all
      // but one handler. The handler pad has a single catchswitch predecessor,
      // so materialize its phi copies immediately after the pad instead.
      Instruction *InsertBefore = Term;
      if (Succ.isEHPad() && isa<CatchSwitchInst>(Term)) {
        Instruction *Pad = Succ.getFirstNonPHI();
        if (!Pad)
          continue;
        InsertBefore = Pad->getNextNode();
        if (!InsertBefore)
          continue;
      }
      IRBuilder<> B(InsertBefore);
      for (auto &C : Ordered) {
        Value *V = C.second;
        // An incoming value that was itself materialized (non-phi def)
        // is read through a load so the copy sees the stored value.
        if (Instruction *DI = dyn_cast<Instruction>(V)) {
          auto MIt = Materialized.find(DI);
          if (MIt != Materialized.end() && !isa<PHINode>(DI)) {
            B.SetCurrentDebugLocation(DI->getDebugLoc());
            V = B.CreateLoad(MIt->second->getAllocatedType(), MIt->second);
          }
        }
        // Web members resolve to the shared local's current value: a
        // phi-to-phi edge reads the source web's slot.
        if (Instruction *PI = dyn_cast<Instruction>(V)) {
          if (PHINode *PPN = dyn_cast<PHINode>(PI)) {
            std::string SKey = keyFor(PPN->getName(), PPN->getType());
            auto SLIt = LocalByName.find(SKey);
            if (SLIt != LocalByName.end() && SLIt->second != C.first) {
              B.SetCurrentDebugLocation(PPN->getDebugLoc());
              V = B.CreateLoad(SLIt->second->getAllocatedType(),
                               SLIt->second, "v_phiread");
            } else {
              // Self-edge (latch): value already in the slot; skip.
              continue;
            }
          }
        }
        B.SetCurrentDebugLocation(Term->getDebugLoc());
        StoreInst *ST = B.CreateStore(V, C.first);
        if (Term->hasMetadata("revenant.va")) {
          MDNode *MD = Term->getMetadata("revenant.va");
          ST->setMetadata("revenant.va", MD);
        } else {
          // Fall back to the phi's own provenance.
          for (PHINode &PN : Succ.phis()) {
            std::string K = keyFor(PN.getName(), PN.getType());
            auto LIt = LocalByName.find(K);
            if (LIt != LocalByName.end() && LIt->second == C.first) {
              copyProvenance(&PN, ST);
              break;
            }
          }
        }
      }
    }
  }

  // 4. Drop the phi nodes themselves: every use now reads the local.
  // Phi uses inside other phis were resolved by the edge copies; erase.
  SmallVector<PHINode *, 16> DeadPhis;
  for (BasicBlock &BB : F)
    for (PHINode &PN : BB.phis())
      DeadPhis.push_back(&PN);
  for (PHINode *PN : DeadPhis) {
    PN->replaceAllUsesWith(UndefValue::get(PN->getType()));
    PN->eraseFromParent();
    Changed = true;
  }

  // Entry-block PHI-less path: webs fed only by the entry predecessor
  // need their initial value stored on entry. The edge copies covered
  // every predecessor, including entry fallthrough, so nothing extra.
  DIB.finalize();
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-ssaout` name in Pipeline/Register.cpp, so the entry point is
// compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
#ifdef REVENANT_BUILD_PLUGIN
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantSsaOut", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-ssaout") {
                    PM.addPass(revenant::SsaOutPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
