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
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <map>
#include <set>
#include <string>

using namespace llvm;

namespace revenant {
namespace {
// A phi web: phi destinations that feed each other through phi incoming
// edges share one local. `canon` resolves an SSA name to its web's
// canonical member (hop-bounded; a cycle means a missed coalesce, never
// a hang). Webs are keyed by name, so an unnamed phi never joins one: all
// unnamed phis share the empty name, and coalescing through it would put
// unrelated values in one local. Two webs merge only when no member of
// one is live where a member of the other is defined: `x = phi [y]`,
// `y = phi [x + y]` (a Fibonacci step) or a swap keep both values alive
// in the loop, and one local for both would lose one of them.
class PhiWebs {
public:
  void build(Function &F) {
    for (BasicBlock &BB : F) {
      for (PHINode &PN : BB.phis()) {
        if (!PN.hasName())
          continue;
        std::string Name = PN.getName().str();
        PhiDsts[Name] = &PN;
        Members[Name].push_back(&PN);
      }
    }
    for (BasicBlock &BB : F) {
      for (PHINode &PN : BB.phis()) {
        if (!PN.hasName())
          continue;
        std::string Dst = PN.getName().str();
        for (Value *V : PN.incoming_values()) {
          if (!V->hasName())
            continue;
          std::string N = V->getName().str();
          if (!PhiDsts.count(N))
            continue;
          std::string D = canon(Dst);
          std::string S = canon(N);
          if (D == S || interferes(Members[D], Members[S]))
            continue;
          Alias[S] = D;
          Members[D].append(Members[S].begin(), Members[S].end());
          Members.erase(S);
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

private:
  // The blocks at whose start phi A is live: a use of A is reachable from
  // there without passing A's own block, which redefines A. A phi use
  // reads A at the end of its incoming block. Backward from the uses,
  // computed once per phi.
  const SmallPtrSet<BasicBlock *, 16> &liveIn(PHINode *A) {
    auto [It, New] = LiveIn.try_emplace(A);
    if (!New)
      return It->second;
    SmallPtrSet<BasicBlock *, 16> &Live = It->second;
    BasicBlock *Def = A->getParent();
    SmallVector<BasicBlock *, 16> Work;
    auto Reach = [&](BasicBlock *BB) {
      if (BB != Def && Live.insert(BB).second)
        Work.push_back(BB);
    };
    for (Use &U : A->uses()) {
      auto *I = cast<Instruction>(U.getUser());
      if (auto *PN = dyn_cast<PHINode>(I))
        Reach(PN->getIncomingBlock(U));
      else
        Reach(I->getParent());
    }
    while (!Work.empty())
      for (BasicBlock *Pred : predecessors(Work.pop_back_val()))
        Reach(Pred);
    return Live;
  }
  // Two phis interfere when one is live where the other is defined (the
  // start of its block). Phis of one block are defined together.
  bool interferes(ArrayRef<PHINode *> A, ArrayRef<PHINode *> B) {
    for (PHINode *X : A)
      for (PHINode *Y : B)
        if (X->getParent() == Y->getParent() ||
            liveIn(X).count(Y->getParent()) ||
            liveIn(Y).count(X->getParent()))
          return true;
    return false;
  }

  // Small-string-keyed sets: phi names are short; std::map keeps this
  // header-free (no DenseMapInfo<StringRef> specialization needed).
  std::map<std::string, PHINode *> PhiDsts;
  std::map<std::string, std::string> Alias;
  // Web root name -> the phis in that web.
  std::map<std::string, SmallVector<PHINode *, 4>> Members;
  DenseMap<PHINode *, SmallPtrSet<BasicBlock *, 16>> LiveIn;
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

  // Phi webs always get a local, even before uses are rewritten. An
  // unnamed phi is a web of its own: the first one of each type keeps the
  // shared unnamed key, every further one takes a fresh temporary, so two
  // unnamed accumulators never write the same local.
  std::set<std::string> UnnamedPhiKeys;
  auto freshTmpKey = [&]() {
    std::string Key;
    do
      Key = "tmp" + std::to_string(TmpN++);
    while (LocalByName.count(Key) || F.getValueSymbolTable()->lookup(Key));
    return Key;
  };
  for (BasicBlock &BB : F) {
    for (PHINode &PN : BB.phis()) {
      std::string Key = keyFor(PN.getName(), PN.getType());
      if (!PN.hasName() && !UnnamedPhiKeys.insert(Key).second)
        Key = freshTmpKey();
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
      // Lifetime markers require the original alloca, not a load from
      // its materialized pointer slot.
      if (auto *CB = dyn_cast<CallBase>(U.getUser()))
        if (CB->isLifetimeStartOrEnd())
          continue;
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
  // One spill local per type, made on the first cycle that needs it.
  std::map<Type *, AllocaInst *> SpillByType;
  auto spillFor = [&](Type *Ty) {
    AllocaInst *&Spill = SpillByType[Ty];
    if (Spill)
      return Spill;
    IRBuilder<> B(&F.getEntryBlock(), F.getEntryBlock().begin());
    Spill = B.CreateAlloca(Ty, nullptr, "v_spill");
    DIType *SpillTy = nullptr;
    if (Ty->isFloatingPointTy())
      SpillTy = DIB.createBasicType("double", 64, dwarf::DW_ATE_float);
    else if (Ty->isIntegerTy(1))
      SpillTy = DIB.createBasicType("bool", 1, dwarf::DW_ATE_boolean);
    else if (Ty->isIntegerTy())
      SpillTy = DIB.createBasicType("long", Ty->getIntegerBitWidth(),
                                    dwarf::DW_ATE_signed);
    else
      SpillTy = DIB.createBasicType("ptr", 64, dwarf::DW_ATE_address);
    DILocalVariable *SpillVar =
        DIB.createAutoVariable(SP, Spill->getName(), File, 1, SpillTy);
    DIB.insertDeclare(Spill, SpillVar, DIB.createExpression(),
                      DILocation::get(Ctx, 1, 0, SP), Spill->getNextNode());
    Changed = true;
    return Spill;
  };
  for (BasicBlock &Succ : F) {
    if (Succ.phis().empty())
      continue;
    // pred -> list of (dst local, incoming value)
    std::map<BasicBlock *, SmallVector<std::pair<AllocaInst *, Value *>, 4>>
        EdgeCopies;
    for (PHINode &PN : Succ.phis()) {
      AllocaInst *AI = Materialized.lookup(&PN);
      if (!AI)
        continue;
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
      // A copy reads a web local at the copy site only when its incoming
      // value is a phi; every other incoming value was computed, or stored
      // to its own local, before the edge. A phi copied into its own web's
      // local is a no-op (the latch value is already there).
      struct EdgeCopy {
        AllocaInst *Dst;
        Value *V;
        AllocaInst *ReadSlot;
      };
      SmallVector<EdgeCopy, 4> Pending;
      for (auto &[Dst, V] : EdgeCopies[Pred]) {
        AllocaInst *ReadSlot = nullptr;
        if (auto *PN = dyn_cast<PHINode>(V)) {
          ReadSlot = Materialized.lookup(PN);
          if (!ReadSlot || ReadSlot == Dst)
            continue;
        }
        Pending.push_back({Dst, V, ReadSlot});
      }
      // Order: repeatedly emit the first copy whose destination no other
      // pending copy still reads. When every destination is still read
      // (a cycle), save one destination's current value in a spill local
      // and send its readers there; that copy is then safe.
      SmallVector<EdgeCopy, 4> Ordered;
      while (!Pending.empty()) {
        size_t Safe = Pending.size();
        for (size_t I = 0; I < Pending.size() && Safe == Pending.size(); ++I) {
          bool Read = false;
          for (size_t J = 0; J < Pending.size() && !Read; ++J)
            Read = J != I && Pending[J].ReadSlot == Pending[I].Dst;
          if (!Read)
            Safe = I;
        }
        if (Safe == Pending.size()) {
          AllocaInst *Dst = Pending[0].Dst;
          AllocaInst *Spill = spillFor(Dst->getAllocatedType());
          Ordered.push_back({Spill, nullptr, Dst});
          for (EdgeCopy &C : Pending)
            if (C.ReadSlot == Dst)
              C.ReadSlot = Spill;
          continue;
        }
        Ordered.push_back(Pending[Safe]);
        Pending.erase(Pending.begin() + Safe);
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
      for (EdgeCopy &C : Ordered) {
        Value *V = C.V;
        if (C.ReadSlot) {
          // A phi-to-phi edge (or a spill) reads the source local's
          // current value at the copy site.
          auto *PPN = dyn_cast_or_null<PHINode>(V);
          B.SetCurrentDebugLocation(PPN ? PPN->getDebugLoc()
                                        : Term->getDebugLoc());
          V = B.CreateLoad(C.ReadSlot->getAllocatedType(), C.ReadSlot,
                           "v_phiread");
        } else if (Instruction *DI = dyn_cast<Instruction>(V)) {
          // An incoming value that was itself materialized (non-phi def)
          // is read through a load so the copy sees the stored value.
          auto MIt = Materialized.find(DI);
          if (MIt != Materialized.end()) {
            B.SetCurrentDebugLocation(DI->getDebugLoc());
            V = B.CreateLoad(MIt->second->getAllocatedType(), MIt->second);
          }
        }
        B.SetCurrentDebugLocation(Term->getDebugLoc());
        StoreInst *ST = B.CreateStore(V, C.Dst);
        if (Term->hasMetadata("revenant.va")) {
          MDNode *MD = Term->getMetadata("revenant.va");
          ST->setMetadata("revenant.va", MD);
        } else {
          // Fall back to the phi's own provenance.
          for (PHINode &PN : Succ.phis()) {
            if (Materialized.lookup(&PN) == C.Dst) {
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
