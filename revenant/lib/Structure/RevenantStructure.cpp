//===- RevenantStructure.cpp - goto-free structuring annotations ----------===//
//
// Port of crates/structure's DREAM-style region walk to LLVM IR, operating
// on DominatorTree / PostDominatorTree / LoopInfo. It never changes the
// CFG: it classifies regions and records them as `!revenant.region`
// metadata on header terminators, plus stable `bb_*` block names.
//
// Wire format (see the IR contract):
//   if:      !{!"if", i32 <follow>}
//   loop:    !{!"while"|"dowhile"|"for"|"loop", i32 <hdr>, i32 <latch>,
//            i32 <exit>}          (-1 = no exit)
//   switch:  !{!"switch", i32 <follow>}
//   folded short-circuit cond block: !{!"sc", i32 <partner-head>}
//   irreducible edge:                !{!"goto", i32 <target>}
//
// A single annotation is `!revenant.region !{!"if", i32 3}`; a terminator
// carrying several gets `!revenant.region !{!{..}, !{..}}`.
//
//===----------------------------------------------------------------------===//

#include "revenant/Structure/RevenantStructure.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;

namespace {

// Depth bound for the region walk and short-circuit recursion. Mirrors the
// reference crate (2048 nested regions / 2000 fold frames).
constexpr unsigned MaxRegionDepth = 2048;
// Ceiling on region-walk steps per function; past it the CFG is in
// exponential tail duplication and ends in the flat fallback anyway.
constexpr unsigned MaxRegionVisits = 250000;

enum class ArmKind { Normal, Break, Continue };

/// LLVM-side counterpart of the Rust `Ctx`: the analysis results plus the
/// per-function walk state.
class Structurer {
public:
  Structurer(Function &F, DominatorTree &DT, PostDominatorTree &PDT,
             LoopInfo &LI)
      : F(F), DT(DT), PDT(PDT), LI(LI) {}

  revenant::StructureResult run() {
    indexBlocks();
    computeIpdom();
    nameBlocks();
    bool Structured = region(&F.getEntryBlock(), /*Stop=*/nullptr,
                             /*JumpFrom=*/nullptr);
    if (!Structured || RecursionFailure) {
      // Depth, visit, or active-region cycle: use the flat goto fallback.
      clearAnnotations();
      Result.FlatFallback = true;
    }
    return Result;
  }

private:
  Function &F;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  LoopInfo &LI;
  revenant::StructureResult Result;

  /// Block -> index in the function's basic block list order. This is the
  /// index space the `!revenant.region` operands refer to.
  DenseMap<const BasicBlock *, unsigned> IndexOf;
  /// Immediate post-dominator per block (nullptr when none).
  DenseMap<const BasicBlock *, const BasicBlock *> Ipdom;
  /// Natural loop bodies by header (LoopInfo gives reducible loops only;
  /// that is exactly the reference's back-edge definition).
  DenseMap<const BasicBlock *, Loop *> LoopByHeader;
  /// Folded short-circuit condition blocks: never annotated as `if`.
  DenseSet<const BasicBlock *> FoldedSC;
  /// (ret-only block, folded-into predecessor) pairs, mirroring the
  /// reference's `consumed` set so fully-consumed ret blocks aren't
  /// revisited.
  DenseSet<std::pair<const BasicBlock *, const BasicBlock *>> Consumed;
  /// Active recursive region starts, used to break irreducible cycles that
  /// can re-enter the same region through nested branch walks.
  DenseSet<const BasicBlock *> ActiveRegions;
  bool RecursionFailure = false;
  unsigned Depth = 0;
  unsigned Visits = 0;

  // ---------- setup ----------

  void indexBlocks() {
    unsigned I = 0;
    for (BasicBlock &BB : F) {
      IndexOf[&BB] = I++;
      if (Loop *L = LI.getLoopFor(&BB))
        if (L->getHeader() == &BB)
          LoopByHeader.try_emplace(&BB, L);
    }
  }

  void computeIpdom() {
    for (BasicBlock &BB : F) {
      if (DomTreeNode *N = PDT.getNode(&BB))
        if (DomTreeNode *ID = N->getIDom())
          if (ID->getBlock())
            Ipdom[&BB] = ID->getBlock();
    }
  }

  /// Parse `i64 u0x<hex>` or `i64 <dec>` from an instruction's
  /// `!revenant.va` metadata.
  static std::optional<uint64_t> vaOf(const Instruction &I) {
    MDNode *N = I.getMetadata("revenant.va");
    if (!N || N->getNumOperands() == 0)
      return std::nullopt;
    auto *CAM = dyn_cast<ConstantAsMetadata>(N->getOperand(0));
    if (!CAM)
      return std::nullopt;
    auto *CI = dyn_cast<ConstantInt>(CAM->getValue());
    if (!CI)
      return std::nullopt;
    return CI->getZExtValue();
  }

  /// Stable `bb_<VA hex>` / `bb_x<index>` names per the IR contract.
  void nameBlocks() {
    DenseMap<StringRef, unsigned> XCount;
    unsigned X = 0;
    for (BasicBlock &BB : F) {
      std::optional<uint64_t> VA;
      for (Instruction &I : BB) {
        if ((VA = vaOf(I)))
          break;
      }
      SmallString<32> Name;
      if (VA)
        (Twine("bb_") + Twine::utohexstr(*VA)).toVector(Name);
      else {
        (Twine("bb_x") + Twine(X++)).toVector(Name);
        (void)XCount;
      }
      BB.setName(Name);
    }
  }

  unsigned idx(const BasicBlock *B) const {
    auto It = IndexOf.find(B);
    return It == IndexOf.end() ? ~0u : It->second;
  }
  int sidx(const BasicBlock *B) const {
    unsigned I = idx(B);
    return I == ~0u ? -1 : (int)I;
  }

  // ---------- metadata ----------

  static StringRef kindName(int K) {
    static const char *Names[] = {"if",      "while",  "dowhile",
                                  "for",     "switch", "loop",
                                  "sc",      "goto"};
    return Names[K];
  }
  enum { KIf = 0, KWhile, KDoWhile, KFor, KSwitch, KLoop, KSC, KGoto };

  /// Append a `!revenant.region` descriptor to T's annotation.
  void annotate(const Instruction &TC, int Kind,
                std::initializer_list<int> Ops) {
    Instruction &T = const_cast<Instruction &>(TC);
    SmallVector<Metadata *, 8> D;
    D.push_back(MDString::get(F.getContext(), kindName(Kind)));
    for (int O : Ops)
      D.push_back(ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(F.getContext()), O, true)));
    MDNode *NewDesc = MDNode::get(F.getContext(), D);

    MDNode *Old = T.getMetadata("revenant.region");
    SmallVector<Metadata *, 4> Descs;
    if (Old) {
      // Existing annotation: single descriptor form has an MDString first
      // operand; multi form has only MDNodes.
      bool Single = Old->getNumOperands() > 0 &&
                    isa<MDString>(Old->getOperand(0));
      if (Single)
        Descs.push_back(Old);
      else
        for (const MDOperand &Op : Old->operands())
          Descs.push_back(Op.get());
      // Idempotent: skip if an identical descriptor is already attached.
      auto Same = [&](Metadata *M) {
        auto *N = dyn_cast_or_null<MDNode>(M);
        if (!N || N->getNumOperands() != NewDesc->getNumOperands())
          return false;
        for (unsigned I = 0; I < N->getNumOperands(); ++I)
          if (N->getOperand(I).get() != NewDesc->getOperand(I).get()) {
            // MDString/ConstantAsMetadata are uniqued, so pointer compare
            // is exact.
            return false;
          }
        return true;
      };
      for (Metadata *M : Descs)
        if (Same(M))
          return;
    }
    Descs.push_back(NewDesc);
    if (Descs.size() == 1) {
      T.setMetadata("revenant.region", cast<MDNode>(Descs[0]));
    } else {
      T.setMetadata("revenant.region", MDNode::get(F.getContext(), Descs));
    }
  }

  void clearAnnotations() {
    for (BasicBlock &BB : F)
      if (Instruction *T = BB.getTerminator())
        T->setMetadata("revenant.region", nullptr);
    Result.Gotos = 0;
    Result.Irreducible++;
  }

  // ---------- queries mirroring the reference ----------

  /// The innermost enclosing loop of B among LoopsIn (checked last-to-
  /// first), mirroring `innermost_loop`.
  Loop *innermostLoop(const BasicBlock *B,
                      ArrayRef<const BasicBlock *> LoopsIn) const {
    for (const BasicBlock *H : llvm::reverse(LoopsIn)) {
      auto It = LoopByHeader.find(H);
      if (It != LoopByHeader.end() && It->second->contains(B))
        return It->second;
    }
    return nullptr;
  }

  /// A natural loop headed by B that is not already being structured.
  Loop *loopAt(const BasicBlock *B,
               ArrayRef<const BasicBlock *> LoopsIn) const {
    auto It = LoopByHeader.find(B);
    if (It == LoopByHeader.end())
      return nullptr;
    if (llvm::is_contained(LoopsIn, B))
      return nullptr;
    return It->second;
  }

  /// Whether T edges into a loop header whose body does not contain B.
  bool entersLoop(const BasicBlock *B, const BasicBlock *T) const {
    auto It = LoopByHeader.find(T);
    return It != LoopByHeader.end() && !It->second->contains(B);
  }

  /// An edge B -> T inside loop L is `continue` when T is one of the
  /// enclosing loop headers (reference checks `loops_in` only).
  bool isLoopHeader(const BasicBlock *T,
                    ArrayRef<const BasicBlock *> LoopsIn) const {
    return llvm::is_contained(LoopsIn, T);
  }

  /// T exits loop L when it is neither the header nor in the body.
  static bool leavesLoop(const Loop *L, const BasicBlock *T) {
    return L->getHeader() != T && !L->contains(T);
  }

  /// Block with (debug-only +) phi instructions and a `ret` terminator.
  /// Mirrors `ret_only`: the fold happens once per (block, pred) edge.
  bool retOnly(const BasicBlock *B, const BasicBlock *Pred) {
    if (!B)
      return false;
    if (!isa<ReturnInst>(B->getTerminator()))
      return false;
    for (const Instruction &I : *B) {
      if (isa<PHINode>(I) || I.isDebugOrPseudoInst())
        continue;
      if (!I.isTerminator())
        return false;
    }
    Consumed.insert({B, Pred});
    return true;
  }

  /// Block is a pure short-circuit arm: only pure condition instructions
  /// (cmp/binop/cast/select) before a conditional branch. Mirrors the
  /// reference's foldable-arm predicate.
  static bool pureCondBlock(const BasicBlock &B) {
    auto *BI = dyn_cast<BranchInst>(B.getTerminator());
    if (!BI || !BI->isConditional())
      return false;
    for (const Instruction &I : B) {
      if (I.isDebugOrPseudoInst())
        continue;
      if (isa<ICmpInst>(I) || isa<BinaryOperator>(I) || isa<CastInst>(I) ||
          isa<SelectInst>(I) || I.isTerminator())
        continue;
      return false;
    }
    return true;
  }

  /// The reference's `try_short_circuit`, minus expression building:
  /// `cbr c1, A, B` folds when the chosen arm is a pure-cond block whose
  /// `cbr` shares a successor with ours (`A` shares F -> `&&`-like; on the
  /// F arm `B` sharing T -> `||`-like). Returns the fold's (newT, newF).
  std::optional<std::pair<const BasicBlock *, const BasicBlock *>>
  tryShortCircuit(const BasicBlock *Head, const BasicBlock *T,
                  const BasicBlock *F) {
    for (auto [Arm, Other] : {std::pair{T, F}, std::pair{F, T}}) {
      if (!Arm || Arm == Head || !pureCondBlock(*Arm))
        continue;
      auto *BI = cast<BranchInst>(Arm->getTerminator());
      const BasicBlock *T2 = BI->getSuccessor(0);
      const BasicBlock *F2 = BI->getSuccessor(1);
      // Reference folds when arm's false target coincides with `other`
      // (LAnd) or when arm==F and its true target coincides (LOr); the
      // leftover case (arm==T, t2==other) falls into LAnd as well.
      if (F2 == Other || T2 == Other) {
        FoldedSC.insert(Arm);
        annotate(*Arm->getTerminator(), KSC, {sidx(Head)});
        if (Arm == F && T2 == Other)
          return std::pair{T2, F2}; // `c1 || c2` -> (T2, F2)
        if (Arm == T && F2 == Other)
          return std::pair{T2, F2}; // `c1 && c2` -> (T2, F2)
        if (Arm == T && T2 == Other)
          return std::pair{F2, T2}; // `c1 && !c2` -> (F2, T2)
        // arm == F && F2 == Other: reference's `if f2 == other` arm — the
        // combined predicate targets (t2, f2) as `&&` on the F chain.
        return std::pair{T2, F2};
      }
    }
    return std::nullopt;
  }

  // ---------- loop classification ----------

  /// First lexicographic body block that has an edge back to the header.
  /// Mirrors BTreeSet iteration over the reference's `latches` list.
  const BasicBlock *firstLatch(Loop *L) {
    const BasicBlock *H = L->getHeader();
    SmallVector<BasicBlock *, 8> Latches;
    L->getLoopLatches(Latches);
    const BasicBlock *Best = nullptr;
    for (BasicBlock *La : Latches)
      if (!Best || La->getName() < Best->getName())
        Best = La;
    return Best;
  }

  /// Exit for a loop descriptor, mirroring the reference's `exit`: the
  /// header's ipdom when it's outside the body, else the first
  /// (name-ordered) body block's target that leaves the body.
  const BasicBlock *loopExit(Loop *L) {
    const BasicBlock *H = L->getHeader();
    auto It = Ipdom.find(H);
    if (It != Ipdom.end() && It->second && !L->contains(It->second))
      return It->second;
    // First name-ordered body block with a successor outside.
    SmallVector<BasicBlock *, 32> Body(L->block_begin(), L->block_end());
    llvm::sort(Body, [](BasicBlock *A, BasicBlock *B) {
      return A->getName() < B->getName();
    });
    for (BasicBlock *B : Body) {
      for (BasicBlock *S : successors(B))
        if (!L->contains(S))
          return S;
    }
    return nullptr;
  }

  /// Is the while-header condition based on a canonical induction phi
  /// (init outside, +-step in the latch)? Mirrors `refine_for`'s pattern.
  bool isForLoop(Loop *L, const BasicBlock *Latch) {
    const BasicBlock *H = L->getHeader();
    auto *BI = dyn_cast<BranchInst>(H->getTerminator());
    if (!BI || !BI->isConditional() || !Latch)
      return false;
    auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
    if (!Cmp)
      return false;
    for (const PHINode &Phi : H->phis()) {
      // init from a non-body predecessor, step `phi +/- k` in the latch.
      const Value *Step = Phi.getIncomingValueForBlock(Latch);
      const BasicBlock *OutPred = nullptr;
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
        if (!L->contains(Phi.getIncomingBlock(I)))
          OutPred = Phi.getIncomingBlock(I);
      if (!OutPred || !Step)
        continue;
      auto *BO = dyn_cast<BinaryOperator>(Step);
      if (!BO || (BO->getOpcode() != Instruction::Add &&
                  BO->getOpcode() != Instruction::Sub))
        continue;
      if (BO->getOperand(0) != &Phi)
        continue;
      // cond references the phi (directly or through one pure use).
      bool Refs = false;
      for (Value *Op : Cmp->operands()) {
        if (Op == &Phi) {
          Refs = true;
          break;
        }
        if (auto *I = dyn_cast<Instruction>(Op))
          if (llvm::is_contained(I->operands(), &Phi))
            Refs = true;
      }
      if (Refs)
        return true;
    }
    return false;
  }

  /// Annotate the header and walk the body of natural loop L; returns the
  /// block the outer region continues at (the loop exit), or nullptr.
  const BasicBlock *structureLoop(Loop *L,
                                  ArrayRef<const BasicBlock *> LoopsIn,
                                  SmallPtrSetImpl<const BasicBlock *> &Seen) {
    const BasicBlock *H = L->getHeader();
    SmallVector<const BasicBlock *, 8> NewCtx(LoopsIn.begin(), LoopsIn.end());
    NewCtx.push_back(H);

    const BasicBlock *Latch = firstLatch(L);
    const BasicBlock *Exit = loopExit(L);
    auto *HT = H->getTerminator();

    // while(cond): header `br` with exactly one successor out of the body.
    if (auto *BI = dyn_cast<BranchInst>(HT); BI && BI->isConditional()) {
      const BasicBlock *T = BI->getSuccessor(0);
      const BasicBlock *Fw = BI->getSuccessor(1);
      bool TOut = !L->contains(T), FOut = !L->contains(Fw);
      if (TOut ^ FOut) {
        const BasicBlock *BodyTgt = TOut ? Fw : T;
        int Kind = isForLoop(L, Latch) ? KFor : KWhile;
        annotate(*HT, Kind, {sidx(H), sidx(Latch), sidx(Exit)});
        if (BodyTgt != H)
          region(BodyTgt, H, nullptr, NewCtx);
        return Exit && Exit != H ? Exit : nullptr;
      }
    }
    // do..while: header is a `br` into the body; the latch's `br` loops
    // back to the header on one arm.
    if (auto *BI = dyn_cast<BranchInst>(HT); BI && BI->isUnconditional()) {
      if (Latch) {
        auto *LB = dyn_cast<BranchInst>(Latch->getTerminator());
        if (LB && LB->isConditional() &&
            (LB->getSuccessor(0) == H || LB->getSuccessor(1) == H)) {
          const BasicBlock *DwExit =
              LB->getSuccessor(0) == H ? LB->getSuccessor(1)
                                     : LB->getSuccessor(0);
          annotate(*HT, KDoWhile, {sidx(H), sidx(Latch), sidx(DwExit)});
          region(BI->getSuccessor(0), Latch, nullptr, NewCtx);
          return Exit && Exit != H ? Exit : nullptr;
        }
      }
    }
    // Self-looping header with both arms in the body: do-while whose test
    // is the header itself.
    if (auto *BI = dyn_cast<BranchInst>(HT);
        BI && BI->isConditional() &&
        (BI->getSuccessor(0) == H || BI->getSuccessor(1) == H)) {
      const BasicBlock *DwExit =
          BI->getSuccessor(0) == H ? BI->getSuccessor(1)
                                 : BI->getSuccessor(0);
      annotate(*HT, KDoWhile, {sidx(H), sidx(Latch), sidx(DwExit)});
      return Exit && Exit != H ? Exit : nullptr;
    }
    // Fallback: while (1) { ... } with explicit breaks.
    annotate(*HT, KLoop, {sidx(H), sidx(Latch), sidx(Exit)});
    region(H, H, nullptr, NewCtx, /*ForceFirst=*/true);
    return Exit && Exit != H ? Exit : nullptr;
  }

  // ---------- region walk ----------

  /// The reference's `region_v_f`: walk the acyclic region starting at Cur
  /// until Stop (post-dominator merge / loop boundary), recursing into
  /// arms, loops and switch cases. JumpFrom is the block whose terminator
  /// edge produced Cur, for `goto` annotation on irreducible revisits.
  bool region(const BasicBlock *Start, const BasicBlock *Stop,
              const BasicBlock *JumpFrom,
              ArrayRef<const BasicBlock *> LoopsIn = {},
              bool ForceFirst = false) {
    if (Depth >= MaxRegionDepth || !ActiveRegions.insert(Start).second) {
      RecursionFailure = true;
      return false;
    }
    Depth++;
    bool Ok = regionInner(Start, Stop, JumpFrom, LoopsIn, ForceFirst);
    Depth--;
    ActiveRegions.erase(Start);
    return Ok;
  }

  bool regionInner(const BasicBlock *Start, const BasicBlock *Stop,
                   const BasicBlock *JumpFrom,
                   ArrayRef<const BasicBlock *> LoopsIn, bool ForceFirst) {
    const BasicBlock *Cur = Start;
    const BasicBlock *From = JumpFrom;
    bool First = true;
    SmallPtrSet<const BasicBlock *, 32> Seen;

    while (true) {
      if (++Visits > MaxRegionVisits)
        return false;
      if (Cur == Stop && !(First && ForceFirst))
        break;
      if (!Seen.insert(Cur).second) {
        // Revisit: a cycle LoopInfo didn't capture (irreducible or a
        // multi-entry region). Mark the edge and stop this walk.
        Result.Gotos++;
        Result.Irreducible++;
        if (From) {
          annotate(*const_cast<BasicBlock *>(From)->getTerminator(), KGoto,
                   {sidx(Cur)});
        } else {
          for (const BasicBlock *P : predecessors(Cur))
            if (Seen.contains(P))
              annotate(*P->getTerminator(), KGoto, {sidx(Cur)});
        }
        break;
      }
      First = false;
      const BasicBlock *BB = Cur;
      From = nullptr;
      if (!BB)
        break;
      const Instruction *T = BB->getTerminator();
      if (!T)
        break;

      // Ret-only block whose every incoming edge was folded into a
      // `return`: dead, skip.
      if (isa<ReturnInst>(T) && allPhiOrDbg(*BB)) {
        SmallVector<const BasicBlock *, 8> Preds(predecessors(BB));
        if (!Preds.empty() && !Consumed.empty() &&
            llvm::all_of(Preds, [&](const BasicBlock *P) {
              return Consumed.contains({BB, P});
            }))
          break;
      }

      // Loop header?
      if (Loop *L = loopAt(BB, LoopsIn)) {
        const BasicBlock *Next = structureLoop(L, LoopsIn, Seen);
        if (!Next)
          break;
        Cur = Next;
        From = BB;
        continue;
      }

      if (auto *BI = dyn_cast<BranchInst>(T)) {
        if (BI->isUnconditional()) {
          const BasicBlock *S = BI->getSuccessor(0);
          if (!entersLoop(BB, S)) {
            if (retOnly(S, BB))
              break;
          }
          if (Loop *L = innermostLoop(BB, LoopsIn)) {
            if (isLoopHeader(S, LoopsIn))
              break; // continue;
            if (leavesLoop(L, S))
              break; // break;
          }
          if (S == Stop)
            break;
          Cur = S;
          From = BB;
          continue;
        }
        // Conditional branch.
        const BasicBlock *Next =
            structureBranch(BB, BI->getSuccessor(0), BI->getSuccessor(1),
                            Stop, LoopsIn);
        Cur = continueAt(Next, BB, LoopsIn);
        if (!Cur)
          break;
        continue;
      }
      if (auto *SI = dyn_cast<SwitchInst>(T)) {
        const BasicBlock *Next = structureSwitch(BB, SI, LoopsIn);
        Cur = continueAt(Next, BB, LoopsIn);
        if (!Cur)
          break;
        continue;
      }
      // ret / unreachable / anything else: region ends here.
      break;
    }
    return true;
  }

  static bool allPhiOrDbg(const BasicBlock &B) {
    for (const Instruction &I : B)
      if (!isa<PHINode>(I) && !I.isDebugOrPseudoInst() && !I.isTerminator())
        return false;
    return true;
  }

  /// The region may continue at Next unless that merge sits outside the
  /// innermost enclosing loop body.
  const BasicBlock *continueAt(const BasicBlock *Next, const BasicBlock *Cur,
                               ArrayRef<const BasicBlock *> LoopsIn) const {
    if (!Next)
      return nullptr;
    if (Loop *L = innermostLoop(Cur, LoopsIn))
      if (!L->contains(Next))
        return nullptr;
    return Next;
  }

  ArmKind armKind(const BasicBlock *Tgt, const Loop *LCtx) const {
    if (LCtx) {
      if (Tgt == LCtx->getHeader())
        return ArmKind::Continue;
      if (!LCtx->contains(Tgt))
        return ArmKind::Break;
    }
    return ArmKind::Normal;
  }

  /// Structure `br cond, T, F` at Head; returns the merge (next block to
  /// process) or nullptr.
  const BasicBlock *structureBranch(
      const BasicBlock *Head, const BasicBlock *T, const BasicBlock *F,
      const BasicBlock *Stop, ArrayRef<const BasicBlock *> LoopsIn) {
    const BasicBlock *Merge = nullptr;
    auto It = Ipdom.find(Head);
    if (It != Ipdom.end())
      Merge = It->second;
    else
      Merge = Stop; // reference falls back to `stop` (often none)

    Loop *LCtx = innermostLoop(Head, LoopsIn);
    ArmKind TK = armKind(T, LCtx);
    ArmKind FK = armKind(F, LCtx);
    bool TMerge = T == Merge, FMerge = F == Merge;
    bool MergeOutOfBody =
        LCtx && Merge && !LCtx->contains(Merge);

    // Short-circuit fold: consume pure-cond arm blocks and retry with the
    // folded targets until nothing folds.
    if (Depth < MaxRegionDepth - 48) {
      while (auto Fold = tryShortCircuit(Head, T, F)) {
        T = Fold->first;
        F = Fold->second;
        TK = armKind(T, LCtx);
        FK = armKind(F, LCtx);
        TMerge = T == Merge;
        FMerge = F == Merge;
      }
    }

    bool TRet = retOnly(T, Head), FRet = retOnly(F, Head);
    const BasicBlock *Next =
        (!Merge || MergeOutOfBody) ? nullptr : Merge;

    // Both arms return: `if (c) return; else return;` — annotate as an
    // `if` whose follow is whatever the merge resolved to.
    if (TRet && FRet) {
      annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
      return Next;
    }

    // Loop-edge arm shapes (break/continue combos and one-sided ones).
    if ((TK == ArmKind::Continue && FK == ArmKind::Break) ||
        (TK == ArmKind::Break && FK == ArmKind::Continue))
      return nullptr;
    if (TK == ArmKind::Break && !FMerge) {
      annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
      region(F, Merge, nullptr, LoopsIn);
      return Merge;
    }
    if (FK == ArmKind::Break && !TMerge) {
      annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
      region(T, Merge, nullptr, LoopsIn);
      return Merge;
    }
    if (TK == ArmKind::Continue && !FMerge) {
      annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
      region(F, Merge, nullptr, LoopsIn);
      return Merge;
    }
    if (FK == ArmKind::Continue && !TMerge) {
      annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
      region(T, Merge, nullptr, LoopsIn);
      return Merge;
    }

    if (TRet || FRet) {
      annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
      const BasicBlock *Arm = TRet ? F : T;
      if (!(TRet ? FMerge : TMerge))
        region(Arm, Merge, nullptr, LoopsIn);
      return TRet ? Next : Merge;
    }

    annotate(*Head->getTerminator(), KIf, {sidx(Merge)});
    if (TMerge && FMerge)
      return Next;
    if (FMerge) {
      region(T, Merge, nullptr, LoopsIn);
      return Next;
    }
    if (TMerge) {
      region(F, Merge, nullptr, LoopsIn);
      return Merge;
    }
    region(T, Merge, nullptr, LoopsIn);
    region(F, Merge, nullptr, LoopsIn);
    return Merge;
  }

  /// Structure a `switch` at Head: annotate it and walk each distinct case
  /// target plus the default, up to the merge.
  const BasicBlock *structureSwitch(const BasicBlock *Head,
                                    const SwitchInst *SI,
                                    ArrayRef<const BasicBlock *> LoopsIn) {
    const BasicBlock *Merge = nullptr;
    auto It = Ipdom.find(Head);
    if (It != Ipdom.end())
      Merge = It->second;
    annotate(*Head->getTerminator(), KSwitch, {sidx(Merge)});

    SmallPtrSet<const BasicBlock *, 8> Done;
    for (const auto &Case : SI->cases()) {
      const BasicBlock *S = Case.getCaseSuccessor();
      if (Done.insert(S).second && S != Merge)
        region(S, Merge, nullptr, LoopsIn);
    }
    const BasicBlock *D = SI->getDefaultDest();
    if (D != Merge && Done.insert(D).second)
      region(D, Merge, nullptr, LoopsIn);
    return Merge;
  }
};

} // namespace

revenant::StructureResult revenant::annotateFunction(Function &F,
                                                   DominatorTree &DT,
                                                   PostDominatorTree &PDT,
                                                   LoopInfo &LI) {
  Structurer S(F, DT, PDT, LI);
  return S.run();
}

PreservedAnalyses
revenant::RevenantStructurePass::run(Function &F,
                                     FunctionAnalysisManager &AM) {
  annotateFunction(F, AM.getResult<DominatorTreeAnalysis>(F),
                   AM.getResult<PostDominatorTreeAnalysis>(F),
                   AM.getResult<LoopAnalysis>(F));
  return PreservedAnalyses::all();
}
