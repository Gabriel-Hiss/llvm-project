//===- StructuredStmtBuilder.cpp - region-annotated CFG -> clang::Stmt ---===//
//
// Structured StmtBuilder (lib/Stmt/): consumes `!revenant.region`
// annotations produced by RevenantStructure on header terminators and emits
// CompoundStmt/IfStmt/WhileStmt/DoStmt/ForStmt/SwitchStmt. Blocks no
// annotation covers fall back to LabelStmt + GotoStmt, matching the
// GotoStmtBuilder reference emission in lib/Core/.
//
// Region schema (all block operands are i32 indices into the function's
// basic-block list order, same order Swe2Structure emits):
//   !revenant.region !{!"if", i32 <follow>}
//   !revenant.region !{!"while"|"dowhile"|"for", i32 <header>, i32 <latch>,
//                    i32 <exit>}
//   !revenant.region !{!"switch", i32 <follow>}
//   !revenant.region !{!"sc", i32 <partner>}  (member of an "if" region)
//
// Control-flow emission rules are a port of crates/emit-c and
// crates/emit-cpp: else-if chains, `if (!c) break;` flattening for
// else-arms that are a bare jump out, `for` loops recovered from the
// canonical `init; while(cond-refs-v) {...; v=v+k}` shape, and implicit
// `break` insertion on non-fallthrough switch cases.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/FrontendContext.h"
#include "revenant/Clang/TypeBuilder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/StmtCXX.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"

using namespace clang;
using namespace llvm;

namespace revenant::clangx {

namespace {

enum class RegionKind { If, While, DoWhile, For, Loop, Switch };

static bool isLoopKind(RegionKind K) {
  return K == RegionKind::While || K == RegionKind::DoWhile ||
         K == RegionKind::For || K == RegionKind::Loop;
}

/// Parsed `!revenant.region` annotation on a header block.
struct Region {
  RegionKind Kind = RegionKind::If;
  BasicBlock *Header = nullptr;
  BasicBlock *Follow = nullptr; // merge/exit block, outside the region
  BasicBlock *Latch = nullptr;  // loops only
  /// Members: for if/switch, blocks dominated by Header and post-dominated
  /// by Follow; for loops, dominated by Header and able to reach Latch
  /// without passing through Follow. Header itself is always a member for
  /// loops, never for if/switch.
  SmallPtrSet<BasicBlock *, 16> Members;
  /// Set when an outside edge targets a member: the region degrades to
  /// label+goto emission for its blocks.
  bool Degraded = false;
};

/// A `!"sc"` tag: member of an "if" region whose condition folds into the
/// header's compound condition (the `Partner` operand is the folded peer).
struct ScTag {
  BasicBlock *Partner = nullptr;
};

/// Sanitize a block name into a C label, mirroring structure::label_of
/// (`bb_1A2B` -> `L_bb_1A2B`).
static std::string labelName(const BasicBlock &BB) {
  StringRef N = BB.getName();
  std::string S;
  S.reserve(N.size() + 2);
  S += "L_";
  for (char C : N)
    S.push_back(isAlnum(C) || C == '_' ? C : '_');
  if (S == "L_")
    S += "anon";
  return S;
}

/// Read operand I of a region MDNode as either a string tag (Str set) or
/// an i32 index (Idx set). Returns false when out of range.
static bool regionOperand(MDNode *N, unsigned I, StringRef &Str,
                          int64_t &Idx) {
  if (I >= N->getNumOperands())
    return false;
  Metadata *M = N->getOperand(I).get();
  if (auto *S = dyn_cast<MDString>(M)) {
    Str = S->getString();
    return true;
  }
  if (auto *CM = dyn_cast<ConstantAsMetadata>(M))
    if (auto *CI = dyn_cast<ConstantInt>(CM->getValue())) {
      Idx = CI->getSExtValue();
      return true;
    }
  return false;
}

class StructuredStmtBuilder final : public StmtBuilder {
public:
  StructuredStmtBuilder(FrontendContext &Ctx, TypeBuilder &Types)
      : Ctx(Ctx), Types(Types) {}

  CompoundStmt *buildBody(Function &F, FunctionDecl *FD, ExprBuilder &Exprs,
                          ExprScope &Scope) override;

private:
  // ---- setup ---------------------------------------------------------
  void collectRegions();
  bool parseRegion(BasicBlock *BB, Region &R);
  void computeIfMembers(Region &R);
  void computeLoopMembers(Region &R);

  // ---- queries -------------------------------------------------------
  BasicBlock *blockOf(int64_t Idx) {
    return Idx >= 0 && uint64_t(Idx) < Blocks.size() ? Blocks[Idx] : nullptr;
  }
  Region *regionOf(BasicBlock *BB) {
    auto It = Regions.find(BB);
    return It == Regions.end() ? nullptr : It->second.get();
  }
  /// Smallest region strictly containing BB (BB may be a header itself).
  Region *enclosingRegion(BasicBlock *BB);
  /// Whether To is emitted immediately after From inside Rgn: either the
  /// next member in block order, or the region follow.
  bool flowsIntoNext(BasicBlock *To, BasicBlock *From, const Region *Rgn);
  Expr *caseValue(const ConstantInt *V, QualType CT);
  /// When any IR case needs the high bit, widen a signed switch
  /// condition to its unsigned type (same bit-pattern dispatch, but
  /// C++ narrowing accepts the cases). Updates CT; returns the
  /// condition to emit.
  Expr *widenSwitchCond(Expr *Cond, QualType &CT, SwitchInst *SI);
  LabelDecl *labelFor(BasicBlock *BB);
  void ensureLabel(BasicBlock *BB) { NeededLabels.insert(BB); }
  Stmt *gotoTo(BasicBlock *BB);

  // ---- emission ------------------------------------------------------
  void emitBlockStmts(BasicBlock *BB, SmallVectorImpl<Stmt *> &Out);
  void collectReturnStores();
  StoreInst *returnStoreForEdge(BasicBlock *Target, BasicBlock *Pred);
  Stmt *emitTerminator(BasicBlock *BB, const Region *Rgn);
  void emitMembers(const Region &R, const SmallPtrSetImpl<BasicBlock *> &Set,
                   SmallVectorImpl<Stmt *> &Out);
  Stmt *emitRegionHeader(Region &R, const Region *Outer);
  Stmt *emitIf(Region &R, const Region *Outer);
  Stmt *emitLoop(Region &R, const Region *Outer);
  Stmt *emitSwitch(Region &R, const Region *Outer);
  /// Try/catch from native LLVM EH pads (no region annotation needed).
  /// Null when BB does not end in an invoke into a catchswitch or
  /// landingpad/cleanuppad dispatch.
  Stmt *tryTry(BasicBlock *BB);
  VarDecl *catchVariable(MDNode *TypeMD);
  void emitCatchBlockStmts(BasicBlock *BB, SmallVectorImpl<Stmt *> &Out);
  Expr *condOf(BranchInst *BI, BasicBlock *ContinueSucc);
  Expr *inverted(Value *V);
  /// Canonical for-shape: match `init = store -> IV` in the preheader and
  /// `step = store (binop load IV, k) -> IV` in the latch; on match claim
  /// the instructions and build the init statement and step expression.
  Stmt *tryForPieces(Region &R, Expr *&Step);
  /// Does the member list end in a statement that already transfers
  /// control (return/break/continue/goto)?
  static bool endsTerminal(ArrayRef<Stmt *> SS);

  FrontendContext &Ctx;
  TypeBuilder &Types;

  Function *Fn = nullptr;
  FunctionDecl *FD = nullptr;
  ExprBuilder *Exprs = nullptr;
  ExprScope *Scope = nullptr;
  ASTContext *AST = nullptr;

  SmallVector<BasicBlock *, 32> Blocks;
  DenseMap<const BasicBlock *, LabelDecl *> Labels;
  DenseSet<const IdentifierInfo *> LabelNames;
  SmallPtrSet<BasicBlock *, 8> NeededLabels;
  DenseMap<BasicBlock *, std::unique_ptr<Region>> Regions;
  DenseMap<BasicBlock *, ScTag> ScTags;
  std::unique_ptr<DominatorTree> DT;
  std::unique_ptr<PostDominatorTree> PDT;
  /// Instructions consumed by a for-init or for-step; skipped during leaf
  /// emission.
  DenseSet<Instruction *> ForClaimed;
  SmallVector<Region *, 4> ActiveLoops;
  DenseMap<StoreInst *, BasicBlock *> ReturnStoreFollows;
  /// Invoke blocks already emitted inside a try statement.
  SmallPtrSet<BasicBlock *, 8> EHClaimed;
  /// Blocks whose code already printed: nested if regions sharing one
  /// follow each claim the shared tail in their arm sets, so without
  /// this the same block (and its label) prints once per nesting
  /// level. First emission wins; later ones skip.
  DenseSet<BasicBlock *> Emitted;
};
LabelDecl *StructuredStmtBuilder::labelFor(BasicBlock *BB) {
  auto It = Labels.find(BB);
  if (It != Labels.end())
    return It->second;
  StringRef BlockName = BB->getName();
  std::string FunctionName = FD->getNameAsString();
  for (char &C : FunctionName)
    if (!isAlnum(C) && C != '_')
      C = '_';
  std::string Name =
      FunctionName + "_" +
      (BlockName.empty() ? "L_anon" + Twine(Labels.size()).str()
                         : labelName(*BB));
  IdentifierInfo *II = Ctx.ident(Name);

  if (LabelNames.contains(II)) {
    Name += "_" + Twine(Labels.size()).str();
    II = Ctx.ident(Name);
    while (LabelNames.contains(II)) {
      Name += "_";
      II = Ctx.ident(Name);
    }
  }
  LabelNames.insert(II);
  LabelDecl *LD = LabelDecl::Create(*AST, FD, SourceLocation(), II);
  Labels[BB] = LD;
  return LD;
}

Stmt *StructuredStmtBuilder::gotoTo(BasicBlock *BB) {
  ensureLabel(BB);
  return new (*AST)
      GotoStmt(labelFor(BB), SourceLocation(), SourceLocation());
}

bool StructuredStmtBuilder::parseRegion(BasicBlock *BB, Region &R) {
  MDNode *N = BB->getTerminator()->getMetadata("revenant.region");
  if (!N || N->getNumOperands() == 0)
    return false;

  SmallVector<MDNode *, 4> Descriptors;
  if (isa<MDString>(N->getOperand(0))) {
    Descriptors.push_back(N);
  } else {
    for (const MDOperand &Op : N->operands())
      if (auto *D = dyn_cast_or_null<MDNode>(Op.get()))
        Descriptors.push_back(D);
  }

  auto indexArg = [](MDNode *D, unsigned I, int64_t &Idx) {
    StringRef Tag;
    return regionOperand(D, I, Tag, Idx) && Tag.empty();
  };
  int BestRank = -1;
  Region Best;
  for (MDNode *D : Descriptors) {
    StringRef Tag;
    int64_t Unused = -1;
    if (!regionOperand(D, 0, Tag, Unused) || Tag.empty())
      continue;
    if (Tag == "sc") {
      int64_t PartnerIdx = -1;
      if (indexArg(D, 1, PartnerIdx))
        if (BasicBlock *Partner = blockOf(PartnerIdx))
          ScTags[BB] = ScTag{Partner};
      continue;
    }

    Region Candidate;
    Candidate.Header = BB;
    if (Tag == "if")
      Candidate.Kind = RegionKind::If;
    else if (Tag == "switch")
      Candidate.Kind = RegionKind::Switch;
    else if (Tag == "while")
      Candidate.Kind = RegionKind::While;
    else if (Tag == "dowhile")
      Candidate.Kind = RegionKind::DoWhile;
    else if (Tag == "for")
      Candidate.Kind = RegionKind::For;
    else if (Tag == "loop")
      Candidate.Kind = RegionKind::Loop;
    else
      continue;

    int64_t FollowIdx = -1;
    if (isLoopKind(Candidate.Kind)) {
      int64_t HeaderIdx = -1, LatchIdx = -1;
      if (!indexArg(D, 1, HeaderIdx) || blockOf(HeaderIdx) != BB ||
          !indexArg(D, 2, LatchIdx) || !indexArg(D, 3, FollowIdx))
        continue;
      Candidate.Latch = blockOf(LatchIdx);
      if (!Candidate.Latch)
        continue;
    } else if (!indexArg(D, 1, FollowIdx)) {
      continue;
    }
    if (FollowIdx < -1)
      continue;
    Candidate.Follow = FollowIdx == -1 ? nullptr : blockOf(FollowIdx);
    if (FollowIdx >= 0 && !Candidate.Follow)
      continue;

    int Rank = isLoopKind(Candidate.Kind)
                   ? 3
                   : Candidate.Kind == RegionKind::Switch ? 2 : 1;
    if (Rank > BestRank) {
      BestRank = Rank;
      Best.Kind = Candidate.Kind;
      Best.Header = Candidate.Header;
      Best.Latch = Candidate.Latch;
      Best.Follow = Candidate.Follow;
    }
  }

  if (BestRank < 0)
    return false;
  R.Kind = Best.Kind;
  R.Header = Best.Header;
  R.Latch = Best.Latch;
  R.Follow = Best.Follow;
  return true;
}

void StructuredStmtBuilder::computeIfMembers(Region &R) {
  for (BasicBlock *BB : Blocks) {
    if (BB == R.Header || BB == R.Follow ||
        !DT->dominates(R.Header, BB))
      continue;
    if (R.Follow) {
      if (!PDT->dominates(R.Follow, BB))
        continue;
      // Post-dominance is vacuous for non-terminating exits, so require a
      // path from each member to the region's follow as well.
      SmallVector<BasicBlock *, 16> Stack{BB};
      SmallPtrSet<BasicBlock *, 16> Seen;
      bool ReachesFollow = false;
      while (!Stack.empty() && !ReachesFollow) {
        BasicBlock *N = Stack.pop_back_val();
        if (N == R.Follow) {
          ReachesFollow = true;
          break;
        }
        if (!Seen.insert(N).second || !DT->dominates(R.Header, N))
          continue;
        for (BasicBlock *S : successors(N))
          Stack.push_back(S);
      }
      if (ReachesFollow)
        R.Members.insert(BB);
    }
  }

  if (R.Follow)
    return;

  // With no merge, keep the arm that continues through the function outside
  // the if. The other dominated blocks form the terminating arm(s).
  BasicBlock *Continuation = nullptr;
  for (BasicBlock *BB : llvm::reverse(Blocks)) {
    if (BB == R.Header || !DT->dominates(R.Header, BB))
      continue;
    for (BasicBlock *Succ : successors(R.Header))
      if (Succ != BB && DT->dominates(Succ, BB)) {
        Continuation = Succ;
        break;
      }
    break;
  }
  for (BasicBlock *BB : Blocks) {
    if (BB == R.Header || !DT->dominates(R.Header, BB))
      continue;
    if (Continuation &&
        (BB == Continuation || DT->dominates(Continuation, BB)))
      continue;
    R.Members.insert(BB);
  }
}

void StructuredStmtBuilder::computeLoopMembers(Region &R) {
  BasicBlock *Exit = R.Follow;
  R.Members.insert(R.Header);
  for (BasicBlock *BB : Blocks) {
    if (BB == R.Header || BB == Exit || !DT->dominates(R.Header, BB))
      continue;
    SmallVector<BasicBlock *, 16> Stack{BB};
    SmallPtrSet<BasicBlock *, 16> Seen;
    bool ReachesLatch = false;
    while (!Stack.empty() && !ReachesLatch) {
      BasicBlock *N = Stack.pop_back_val();
      if (N == Exit)
        continue;
      if (N == R.Latch) {
        ReachesLatch = true;
        break;
      }
      if (!Seen.insert(N).second)
        continue;
      for (BasicBlock *S : successors(N))
        if (S != Exit)
          Stack.push_back(S);
    }
    if (ReachesLatch)
      R.Members.insert(BB);
  }
}

void StructuredStmtBuilder::collectRegions() {
  for (BasicBlock &BB : *Fn) {
    Region R;
    if (!parseRegion(&BB, R))
      continue;
    if (isLoopKind(R.Kind))
      computeLoopMembers(R);
    else
      computeIfMembers(R);
    Regions[&BB] = std::make_unique<Region>(std::move(R));
  }
  collectReturnStores();
  // An edge entering the interior of a region from outside forces a label
  // on the target and degrades that region to goto emission. Members
  // branching inward stay structured; only foreign entries degrade.
  for (BasicBlock &BB : *Fn) {
    Instruction *T = BB.getTerminator();
    for (unsigned I = 0, E = T->getNumSuccessors(); I != E; ++I) {
      BasicBlock *S = T->getSuccessor(I);
      // Region emission order does not follow CFG reachability. Determine
      // label needs before emitting any region so later gotos can target
      // blocks whose structured bodies already printed.
      ensureLabel(S);
      Region *SR = enclosingRegion(S);
      if (SR && !SR->Members.contains(&BB) && SR->Header != &BB)
        SR->Degraded = true;
    }
  }
}
void StructuredStmtBuilder::collectReturnStores() {
  ReturnStoreFollows.clear();
  for (const auto &Entry : Regions) {
    Region &R = *Entry.second;
    if (R.Kind != RegionKind::If || !R.Follow)
      continue;
    auto *RI = dyn_cast<ReturnInst>(R.Follow->getTerminator());
    auto *ReturnLoad =
        RI ? dyn_cast_or_null<LoadInst>(RI->getReturnValue()) : nullptr;
    auto *Slot = ReturnLoad
                     ? dyn_cast<AllocaInst>(ReturnLoad->getPointerOperand())
                     : nullptr;
    if (!Slot || ReturnLoad->isVolatile() || ReturnLoad->isAtomic())
      continue;

    bool SlotEscapes = false;
    for (User *U : Slot->users()) {
      if (isa<DbgInfoIntrinsic>(U))
        continue;
      if (auto *Load = dyn_cast<LoadInst>(U)) {
        SlotEscapes |= Load->getPointerOperand() != Slot;
        continue;
      }
      if (auto *Store = dyn_cast<StoreInst>(U)) {
        SlotEscapes |= Store->getPointerOperand() != Slot;
        continue;
      }
      SlotEscapes = true;
      break;
    }
    if (SlotEscapes)
      continue;

    for (BasicBlock *BB : R.Members) {
      auto *BI = dyn_cast<BranchInst>(BB->getTerminator());
      if (!BI || !BI->isUnconditional() ||
          BI->getSuccessor(0) != R.Follow)
        continue;
      StoreInst *LastStore = nullptr;
      for (Instruction &I : llvm::reverse(*BB)) {
        if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I))
          continue;
        LastStore = dyn_cast<StoreInst>(&I);
        break;
      }
      if (!LastStore || LastStore->isVolatile() || LastStore->isAtomic() ||
          LastStore->getPointerOperand() != Slot)
        continue;
      ReturnStoreFollows[LastStore] = R.Follow;
    }
  }
}


StoreInst *StructuredStmtBuilder::returnStoreForEdge(BasicBlock *Target,
                                                     BasicBlock *Pred) {
  if (Emitted.contains(Target) || pred_size(Target) != 1 ||
      *pred_begin(Target) != Pred)
    return nullptr;

  StoreInst *ReturnStore = nullptr;
  for (const auto &Entry : ReturnStoreFollows)
    if (Entry.first->getParent() == Target) {
      ReturnStore = Entry.first;
      break;
    }
  if (!ReturnStore)
    return nullptr;

  SmallPtrSet<const Value *, 16> Needed;
  SmallVector<const Value *, 16> Work{ReturnStore->getValueOperand()};
  while (!Work.empty()) {
    const Value *V = Work.pop_back_val();
    auto *I = dyn_cast<Instruction>(V);
    if (!I || I->getParent() != Target || !Needed.insert(V).second)
      continue;
    for (const Use &Operand : I->operands())
      Work.push_back(Operand.get());
  }
  for (Instruction &I : *Target) {
    if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        &I == ReturnStore)
      continue;
    if (!Needed.contains(&I) || isa<StoreInst>(I))
      return nullptr;
  }
  Emitted.insert(Target);
  return ReturnStore;
}

Region *StructuredStmtBuilder::enclosingRegion(BasicBlock *BB) {
  Region *Best = nullptr;
  for (auto &KV : Regions) {
    Region *R = KV.second.get();
    if (!R->Members.contains(BB) || R->Header == BB)
      continue;
    if (!Best || Best->Members.contains(R->Header))
      Best = R;
  }
  return Best;
}
bool StructuredStmtBuilder::flowsIntoNext(BasicBlock *To, BasicBlock *From,
                                          const Region *Rgn) {
  if (!Rgn)
    return false;
  // "Next emitted" is the next member in block order. Edges landing
  // mid-region need a real terminator; only the tail member may fall
  // through to the region follow.
  bool AfterFrom = false;
  for (BasicBlock *BB : Blocks) {
    if (BB == From) {
      AfterFrom = true;
      continue;
    }
    if (AfterFrom && Rgn->Members.contains(BB))
      return BB == To;
  }
  // From is the last member: flow continues at Follow.
  return To == Rgn->Follow;
}

/// Non-terminator statements of a block (phis, debug intrinsics and
/// instructions claimed by a for-header are skipped).
void StructuredStmtBuilder::emitBlockStmts(BasicBlock *BB,
                                           SmallVectorImpl<Stmt *> &Out) {
  for (Instruction &I : *BB) {
    if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I))
      continue;
    if (ForClaimed.contains(&I))
      continue;
    if (auto *Store = dyn_cast<StoreInst>(&I);
        Store && ReturnStoreFollows.contains(Store))
      continue;
    if (Stmt *S = Exprs->instr(I, *Scope))
      Out.push_back(S);
  }
}

/// Condition for a conditional branch where `ContinueSucc` is the
/// successor that keeps control inside the construct.
Expr *StructuredStmtBuilder::condOf(BranchInst *BI,
                                    BasicBlock *ContinueSucc) {
  if (BI->getSuccessor(0) == ContinueSucc)
    return Exprs->cond(BI->getCondition(), *Scope);
  return inverted(BI->getCondition());
}

Expr *StructuredStmtBuilder::inverted(Value *V) {
  // Predicate flip or a parenthesized `!`, as the expression builder decides.
  return Exprs->inverted(V, *Scope);
}

Expr *StructuredStmtBuilder::caseValue(const ConstantInt *V, QualType CT) {
  if (CT->isEnumeralType())
    CT = AST->getPromotedIntegerType(CT);
  if (!CT->isIntegerType())
    CT = AST->IntTy;
  // Promote small int types to int to avoid Ui8/Ui16 suffixes
  if (CT->isIntegerType() && AST->getTypeSize(CT) < 32)
    CT = AST->IntTy;
  unsigned W = (unsigned)AST->getIntWidth(CT);
  APInt Raw = V->getValue();
  if (CT->isSignedIntegerType())
    Raw = Raw.sextOrTrunc(W);
  else if (Raw.getBitWidth() != W)
    Raw = Raw.zextOrTrunc(W);
  return IntegerLiteral::Create(*AST, Raw, CT, SourceLocation());
}

Expr *StructuredStmtBuilder::widenSwitchCond(Expr *Cond, QualType &CT,
                                             SwitchInst *SI) {
  // The checker types the switch by the PRINTED condition: an
  // ImplicitCast (lvalue-to-rvalue) prints as its operand, so the
  // declared variable type governs, not the load's type. When that
  // printed type is signed and any case needs the high bit, wrap the
  // condition in an explicit unsigned cast (same bit-pattern
  // dispatch) so C++ narrowing accepts the cases.
  if (!CT->isIntegerType()) {
    unsigned Bits = SI->getCondition()->getType()->getIntegerBitWidth();
    QualType IntegerTy = AST->getIntTypeForBitwidth(Bits, false);
    if (IntegerTy.isNull())
      return Cond;
    CastKind K = Cond->getType()->isPointerType() ? CK_PointerToIntegral
                                                  : CK_IntegralCast;
    CT = IntegerTy;
    return CStyleCastExpr::Create(*AST, IntegerTy, VK_PRValue, K, Cond,
                                  nullptr, FPOptionsOverride(),
                                  AST->getTrivialTypeSourceInfo(IntegerTy),
                                  SourceLocation(), SourceLocation());
  }
  Expr *Stripped = Cond;
  while (auto *IC = dyn_cast<ImplicitCastExpr>(Stripped))
    Stripped = IC->getSubExpr();
  QualType Printed = Stripped ? Stripped->getType() : CT;
  if (!Printed->isSignedIntegerType())
    return Cond;
  for (auto &Case : SI->cases())
    if (Case.getCaseValue()->getValue().isNegative()) {
      if (Printed->isSignedIntegerType() || Printed->isUnsignedIntegerType()) {
        QualType U = AST->getCorrespondingUnsignedType(Printed);
        if (U.isNull())
          return Cond;
        TypeSourceInfo *TSI = AST->getTrivialTypeSourceInfo(U);
        CT = U;
        return CStyleCastExpr::Create(*AST, U, VK_PRValue, CK_IntegralCast,
                                      Cond, nullptr, FPOptionsOverride(), TSI,
                                      SourceLocation(), SourceLocation());
      }
    }
  return Cond;
}
Stmt *StructuredStmtBuilder::emitTerminator(BasicBlock *BB,
                                            const Region *Rgn) {
  Instruction *T = BB->getTerminator();
  if (auto *RI = dyn_cast<ReturnInst>(T))
    return ReturnStmt::Create(*AST, SourceLocation(),
                              Exprs->retVal(RI->getReturnValue(), *Scope),
                              nullptr);
  if (isa<UnreachableInst>(T))
    return nullptr;
  auto loopTransfer = [&](BasicBlock *Target) -> Stmt * {
    if (ActiveLoops.empty())
      return nullptr;
    Region *Loop = ActiveLoops.back();
    if (Target == Loop->Follow)
      return new (*AST) BreakStmt(SourceLocation());
    if (Target != Loop->Header && Target != Loop->Latch)
      return nullptr;
    for (Instruction &I : *Target)
      if (!I.isTerminator() && !isa<PHINode>(I) &&
          !isa<DbgInfoIntrinsic>(I))
        return nullptr;
    return new (*AST) ContinueStmt(SourceLocation());
  };
  auto loopTrampolineTransfer = [&](BasicBlock *Target) -> Stmt * {
    if (ActiveLoops.empty() || pred_size(Target) != 1)
      return nullptr;
    for (Instruction &I : *Target)
      if (!I.isTerminator() && !isa<PHINode>(I) &&
          !isa<DbgInfoIntrinsic>(I))
        return nullptr;
    auto *Edge = dyn_cast<BranchInst>(Target->getTerminator());
    if (!Edge || !Edge->isUnconditional())
      return nullptr;
    Stmt *Transfer = loopTransfer(Edge->getSuccessor(0));
    if (Transfer)
      Emitted.insert(Target);
    return Transfer;
  };

  if (auto *BI = dyn_cast<BranchInst>(T)) {
    if (BI->isUnconditional()) {
      BasicBlock *S = BI->getSuccessor(0);
      for (const auto &Entry : ReturnStoreFollows)
        if (Entry.first->getParent() == BB && Entry.second == S)
          return ReturnStmt::Create(
              *AST, SourceLocation(),
              Exprs->retVal(Entry.first->getValueOperand(), *Scope), nullptr);
      if (Rgn && !Rgn->Degraded) {
        // A non-latch edge back to the header is a source-level continue;
        // in a for-loop, Clang routes it through the emitted increment.
        if (isLoopKind(Rgn->Kind) && S == Rgn->Header &&
            BB != Rgn->Latch)
          return new (*AST) ContinueStmt(SourceLocation());
        // Edges reaching an outer construct's exit stay gotos (membership
        // does not prove lexical containment).
        if ((isLoopKind(Rgn->Kind) || Rgn->Kind == RegionKind::Switch) &&
            S == Rgn->Follow)
          return new (*AST) BreakStmt(SourceLocation());
        // The latch's backedge is implicit in the loop construct.
        if (isLoopKind(Rgn->Kind) && S == Rgn->Header)
          return nullptr;
        if (flowsIntoNext(S, BB, Rgn))
          return nullptr;
      }
      if (Stmt *Transfer = loopTransfer(S))
        return Transfer;
      if (Stmt *Transfer = loopTrampolineTransfer(S))
        return Transfer;
      return gotoTo(S);
    }
    // Conditional leaf: express each arm as a control statement or let it
    // flow linearly. emit-c flattens `if (c) {..} else { break; }` to
    // `if (!c) break;`; we produce the same AST shape directly.
    BasicBlock *TS = BI->getSuccessor(0), *FS = BI->getSuccessor(1);
    auto arm = [&](BasicBlock *S) -> Stmt * {
      if (TS != FS)
        if (StoreInst *Store = returnStoreForEdge(S, BB))
          return ReturnStmt::Create(
              *AST, SourceLocation(),
              Exprs->retVal(Store->getValueOperand(), *Scope), nullptr);
      if (Rgn && !Rgn->Degraded) {
        // Same rule as the unconditional edge: break only when the arm
        // exits the construct it is lexically emitted inside.
        if (isLoopKind(Rgn->Kind) && S == Rgn->Header &&
            BB != Rgn->Latch)
          return new (*AST) ContinueStmt(SourceLocation());
        if ((isLoopKind(Rgn->Kind) || Rgn->Kind == RegionKind::Switch) &&
            S == Rgn->Follow)
          return new (*AST) BreakStmt(SourceLocation());
        if (flowsIntoNext(S, BB, Rgn))
          return nullptr;
      }
      if (Stmt *Transfer = loopTransfer(S))
        return Transfer;
      if (Stmt *Transfer = loopTrampolineTransfer(S))
        return Transfer;
      return gotoTo(S);
    };
    Stmt *TA = arm(TS), *FA = arm(FS);
    if (!TA && !FA)
      return nullptr;
    if (!FA)
      return IfStmt::Create(*AST, SourceLocation(), IfStatementKind::Ordinary,
                            nullptr, nullptr,
                            Exprs->cond(BI->getCondition(), *Scope),
                            SourceLocation(), SourceLocation(), TA,
                            SourceLocation(), nullptr);
    if (!TA)
      return IfStmt::Create(*AST, SourceLocation(), IfStatementKind::Ordinary,
                            nullptr, nullptr,
                            inverted(BI->getCondition()), SourceLocation(),
                            SourceLocation(), FA, SourceLocation(), nullptr);
    return IfStmt::Create(*AST, SourceLocation(), IfStatementKind::Ordinary,
                          nullptr, nullptr,
                          Exprs->cond(BI->getCondition(), *Scope),
                          SourceLocation(), SourceLocation(), TA,
                          SourceLocation(), FA);
  }

  if (auto *SI = dyn_cast<SwitchInst>(T)) {
    // Unannotated switch: goto-style dispatch (same as GotoStmtBuilder).
    Expr *CondE = Exprs->cond(SI->getCondition(), *Scope);
    QualType CT = CondE ? CondE->getType() : AST->IntTy;
    CondE = widenSwitchCond(CondE, CT, SI);
    SwitchStmt *SW = SwitchStmt::Create(*AST, nullptr, nullptr, CondE,
                                        SourceLocation(), SourceLocation());
    SmallVector<Stmt *, 8> Cases;
    for (auto &Case : SI->cases()) {
      Expr *LH = caseValue(Case.getCaseValue(), CT);
      CaseStmt *CS = CaseStmt::Create(*AST, LH, nullptr, SourceLocation(),
                                      SourceLocation(), SourceLocation());
      CS->setSubStmt(gotoTo(Case.getCaseSuccessor()));
      Cases.push_back(CS);
    }
    if (BasicBlock *Def = SI->getDefaultDest())
      Cases.push_back(new (*AST)
                          DefaultStmt(SourceLocation(), SourceLocation(),
                                      gotoTo(Def)));
    SW->setBody(CompoundStmt::Create(*AST, Cases, FPOptionsOverride(),
                                     SourceLocation(), SourceLocation()));
    return SW;
  }

  // invoke/callbr/indirectbr and friends: emit a null statement; leaves
  // the output compilable until those terminators get real lowering.
  return new (*AST) NullStmt(SourceLocation(), false);
}

void StructuredStmtBuilder::emitMembers(
    const Region &R, const SmallPtrSetImpl<BasicBlock *> &Set,
    SmallVectorImpl<Stmt *> &Out) {
  for (BasicBlock *BB : Blocks) {
    if (!Set.contains(BB))
      continue;
    if (BB == R.Header || BB == R.Latch)
      continue; // header/latch code is consumed by the construct
    // Each block prints once per function: nested regions sharing a
    // follow all claim the shared tail.
    if (Region *Nested = regionOf(BB); Nested && !Nested->Degraded) {
      if (!Emitted.insert(BB).second)
        continue;
      Out.push_back(emitRegionHeader(*Nested, &R));
      continue;
    }
    if (!Emitted.insert(BB).second)
      continue;
    bool Labeled = NeededLabels.contains(BB);
    size_t Start = Out.size();
    emitBlockStmts(BB, Out);
    if (Stmt *T = emitTerminator(BB, &R))
      Out.push_back(T);
    if (Labeled) {
      // Wrap this block's emitted statements in a compound so the label
      // always labels a statement.
      SmallVector<Stmt *, 8> Inner(Out.begin() + Start, Out.end());
      Out.resize(Start);
      CompoundStmt *CS =
          CompoundStmt::Create(*AST, Inner, FPOptionsOverride(),
                               SourceLocation(), SourceLocation());
      Out.push_back(new (*AST) LabelStmt(SourceLocation(), labelFor(BB), CS));
    }
  }
}


Stmt *StructuredStmtBuilder::emitIf(Region &R, const Region *Outer) {
  auto *BI = dyn_cast<BranchInst>(R.Header->getTerminator());
  if (!BI || !BI->isConditional()) {
    // Degenerate annotation: emit the block flat.
    SmallVector<Stmt *, 8> BS;
    emitBlockStmts(R.Header, BS);
    emitMembers(R, R.Members, BS);
    if (Stmt *T = emitTerminator(R.Header, Outer))
      BS.push_back(T);
    return CompoundStmt::Create(*AST, BS, FPOptionsOverride(),
                                SourceLocation(), SourceLocation());
  }
  BasicBlock *TS = BI->getSuccessor(0), *FS = BI->getSuccessor(1);
  auto armMembers = [&](BasicBlock *Arm) {
    SmallPtrSet<BasicBlock *, 8> Out;
    if (Arm == R.Follow)
      return Out;
    if (R.Members.contains(Arm))
      Out.insert(Arm);
    for (BasicBlock *BB : R.Members)
      if (DT->dominates(Arm, BB))
        Out.insert(BB);
    return Out;
  };
  SmallPtrSet<BasicBlock *, 8> TArm = armMembers(TS);
  SmallPtrSet<BasicBlock *, 8> FArm = armMembers(FS);
  // Tail-duplicated joins: a block reachable from both arms (a shared
  // successor the pass duplicated per predecessor) would emit once per
  // arm and redefine its label. Keep it in the arm that dominates it
  // and drop it from the other; control reaches it through the kept
  // copy's fallthrough.
  for (BasicBlock *BB : Blocks) {
    if (!TArm.contains(BB) || !FArm.contains(BB))
      continue;
    bool TDominates = DT->dominates(TS, BB);
    bool FDominates = DT->dominates(FS, BB);
    if (TDominates && !FDominates)
      FArm.erase(BB);
    else if (FDominates && !TDominates)
      TArm.erase(BB);
    else
      FArm.erase(BB);
  }

  // Folded compound conditions: an "sc" member whose successors split the
  // same way as the header folds into the condition. `A && B` when the
  // member's true successor is the partner/true arm and false is the
  // region false arm; `A || B` mirrored.
  Expr *C = Exprs->cond(BI->getCondition(), *Scope);
  for (auto &KV : ScTags) {
    BasicBlock *SC = KV.first;
    BasicBlock *Partner = KV.second.Partner;
    if (!Partner || !R.Members.contains(SC))
      continue;
    auto *SBI = dyn_cast<BranchInst>(SC->getTerminator());
    if (!SBI || !SBI->isConditional())
      continue;
    BasicBlock *S0 = SBI->getSuccessor(0), *S1 = SBI->getSuccessor(1);
    Expr *SCnd = Exprs->cond(SBI->getCondition(), *Scope);
    if (S0 == TS && S1 == FS)
      continue; // identical split: nothing to fold
    if (S1 == FS && (TArm.contains(S0) || S0 == TS))
      C = clang::BinaryOperator::Create(*AST, C, SCnd, BO_LAnd, AST->BoolTy,
                                 VK_PRValue, OK_Ordinary, SourceLocation(),
                                 FPOptionsOverride());
    else if (S0 == TS && (FArm.contains(S1) || S1 == FS))
      C = clang::BinaryOperator::Create(*AST, C, SCnd, BO_LOr, AST->BoolTy,
                                 VK_PRValue, OK_Ordinary, SourceLocation(),
                                 FPOptionsOverride());
  }

  auto emitArm = [&](BasicBlock *Arm,
                     const SmallPtrSetImpl<BasicBlock *> &Mem) -> Stmt * {
    if (!ActiveLoops.empty() && Arm != R.Follow && pred_size(Arm) == 1) {
      bool Empty = true;
      for (Instruction &I : *Arm)
        if (!I.isTerminator() && !isa<PHINode>(I) &&
            !isa<DbgInfoIntrinsic>(I)) {
          Empty = false;
          break;
        }
      auto *Edge = dyn_cast<BranchInst>(Arm->getTerminator());
      Region *Loop = ActiveLoops.back();
      if (Empty && Edge && Edge->isUnconditional() &&
          Edge->getSuccessor(0) == Loop->Follow) {
        Emitted.insert(Arm);
        return new (*AST) BreakStmt(SourceLocation());
      }
    }
    if (Arm == R.Follow || Mem.empty())
      return nullptr;
    SmallVector<Stmt *, 8> SS;
    emitMembers(R, Mem, SS);
    if (SS.size() == 1)
      return SS.front();
    return CompoundStmt::Create(*AST, SS, FPOptionsOverride(),
                                SourceLocation(), SourceLocation());
  };

  Stmt *Then = emitArm(TS, TArm);
  Stmt *Else = emitArm(FS, FArm);

  // `else` arm is a bare break/continue/return-shaped single jump: emit-c
  // flattens to `if (!c) <jump>;` and un-nests the then-arm. In AST terms
  // the parent region emits `IfStmt(!c, jump)` followed by the then-arm's
  // statements. We express that here by returning a CompoundStmt when the
  // else is a lone jump and the then side is a multi-statement block —
  // but only the flattening that changes AST shape matters: swap the
  // condition so `if (!c) elseJump` and the then-arm runs linearly. The
  // caller (emitMembers) sees regions as one statement, so we return a
  // compound with [if(!c) jump, then-stmts...] to match emit-c's text.
  bool ElseSingleJump =
      Else && (isa<BreakStmt>(Else) || isa<ContinueStmt>(Else) ||
               isa<ReturnStmt>(Else) || isa<GotoStmt>(Else));
  if (ElseSingleJump && Then && isa<CompoundStmt>(Then)) {
    SmallVector<Stmt *, 8> Flat;
    Flat.push_back(IfStmt::Create(*AST, SourceLocation(),
                                  IfStatementKind::Ordinary, nullptr,
                                  nullptr, inverted(BI->getCondition()),
                                  SourceLocation(), SourceLocation(), Else,
                                  SourceLocation(), nullptr));
    for (Stmt *S : cast<CompoundStmt>(Then)->body())
      Flat.push_back(S);
    return CompoundStmt::Create(*AST, Flat, FPOptionsOverride(),
                                SourceLocation(), SourceLocation());
  }

  if (!Then && Else) {
    // Guard-only arm on the false edge: `if (!c) { else }`.
    return IfStmt::Create(*AST, SourceLocation(), IfStatementKind::Ordinary,
                          nullptr, nullptr, inverted(BI->getCondition()),
                          SourceLocation(), SourceLocation(), Else,
                          SourceLocation(), nullptr);
  }
  if (!Then)
    Then = new (*AST) NullStmt(SourceLocation());
  return IfStmt::Create(*AST, SourceLocation(), IfStatementKind::Ordinary,
                        nullptr, nullptr, C, SourceLocation(),
                        SourceLocation(), Then, SourceLocation(), Else);
}

/// Match `init; while(cond refs v) {...; v=v+k}` and claim the pieces.
/// Mirrors structure::refine_for: init is the preheader's tail store into
/// the IV slot, step is the latch's tail `store (add|sub (load IV), k)`
/// into the same slot, and the header icmp must reference a load of IV.
Stmt *StructuredStmtBuilder::tryForPieces(Region &R, Expr *&Step) {
  Step = nullptr;
  BasicBlock *Pre = nullptr;
  for (BasicBlock *P : predecessors(R.Header)) {
    if (R.Members.contains(P))
      continue;
    auto *BI = dyn_cast<BranchInst>(P->getTerminator());
    if (!BI || !BI->isUnconditional() || BI->getSuccessor(0) != R.Header)
      continue;
    if (Pre)
      return nullptr; // ambiguous preheader
    Pre = P;
  }
  if (!Pre || !R.Latch)
    return nullptr;

  auto lastStore = [](BasicBlock *BB) -> StoreInst * {
    for (Instruction &I : llvm::reverse(*BB)) {
      if (I.isTerminator())
        continue;
      if (isa<DbgInfoIntrinsic>(I))
        continue;
      return dyn_cast<StoreInst>(&I);
    }
    return nullptr;
  };
  StoreInst *StepStore = lastStore(R.Latch);
  StoreInst *InitStore = lastStore(Pre);
  auto *HBI = dyn_cast<BranchInst>(R.Header->getTerminator());
  ICmpInst *Cmp =
      (HBI && HBI->isConditional()) ? dyn_cast<ICmpInst>(HBI->getCondition())
                                    : nullptr;
  if (!StepStore || !InitStore || !Cmp)
    return nullptr;
  auto *Upd = dyn_cast<llvm::BinaryOperator>(StepStore->getValueOperand());
  if (!Upd || (Upd->getOpcode() != Instruction::Add &&
               Upd->getOpcode() != Instruction::Sub))
    return nullptr;
  Value *IV = StepStore->getPointerOperand();
  if (InitStore->getPointerOperand() != IV)
    return nullptr;
  auto *StepLd = dyn_cast<LoadInst>(Upd->getOperand(0));
  if (!StepLd || StepLd->getPointerOperand() != IV)
    return nullptr;
  bool RefsIV = false;
  for (Value *Op : Cmp->operands())
    if (auto *L = dyn_cast<LoadInst>(Op))
      RefsIV |= L->getPointerOperand() == IV;
  if (!RefsIV)
    return nullptr;

  ForClaimed.insert(InitStore);
  ForClaimed.insert(StepStore);
  Expr *IVe = Exprs->value(IV, *Scope);
  Expr *InitE = Exprs->value(InitStore->getValueOperand(), *Scope);
  QualType T = IVe->getType();
  Stmt *Init = clang::BinaryOperator::Create(*AST, IVe, InitE, BO_Assign, T,
                                      VK_PRValue, OK_Ordinary,
                                      SourceLocation(), FPOptionsOverride());
  Expr *NewV = Exprs->value(StepStore->getValueOperand(), *Scope);
  Step = clang::BinaryOperator::Create(*AST, Exprs->value(IV, *Scope), NewV,
                                BO_Assign, T, VK_PRValue, OK_Ordinary,
                                SourceLocation(), FPOptionsOverride());
  return Init;
}

bool StructuredStmtBuilder::endsTerminal(ArrayRef<Stmt *> SS) {
  if (SS.empty())
    return false;
  Stmt *Last = SS.back();
  return isa<ReturnStmt, BreakStmt, ContinueStmt, GotoStmt>(Last);
}

Stmt *StructuredStmtBuilder::emitLoop(Region &R, const Region *Outer) {
  ActiveLoops.push_back(&R);
  Expr *Cond = nullptr;
  Expr *Step = nullptr;
  Stmt *Init = nullptr;
  SmallVector<Stmt *, 8> Body;
  SmallVector<Stmt *, 8> Tail; // latch statements for do-while

  if (R.Kind == RegionKind::For)
    Init = tryForPieces(R, Step);

  if (R.Kind == RegionKind::DoWhile) {
    // Body first, test last: condition comes from the latch's conditional
    // branch (continue edge -> header). For do-while, the latch branches
    // back to header when the condition is TRUE (continue), so the emitted
    // do-while condition is the *negation* of the latch's branch condition
    // when the true arm goes to the header.
    auto *BI = dyn_cast<BranchInst>(R.Latch->getTerminator());
    if (BI && BI->isConditional()) {
      // If the true arm goes to header, the condition is negated
      BasicBlock *TrueSucc = BI->getSuccessor(0);
      BasicBlock *FalseSucc = BI->getSuccessor(1);
      if (TrueSucc == R.Header) {
        // True arm continues -> negate
        Expr *RawCond = condOf(BI, R.Header);
        Cond = clang::UnaryOperator::Create(*AST, RawCond, clang::UO_LNot,
                                     RawCond->getType(), VK_PRValue,
                                     OK_Ordinary, SourceLocation(),
                                     false, FPOptionsOverride());
      } else {
        // False arm continues -> use as-is
        Cond = condOf(BI, R.Header);
      }
    } else {
      Cond = IntegerLiteral::Create(*AST, APInt(32, 1), AST->IntTy,
                                    SourceLocation());
    }
    // Header and latch leaf code belong to the body; latch code sits at
    // the end (this includes the increment).
    emitBlockStmts(R.Header, Body);
    for (BasicBlock *BB : Blocks) {
      if (!R.Members.contains(BB) || BB == R.Header || BB == R.Latch)
        continue;
      if (Region *Nested = regionOf(BB); Nested && !Nested->Degraded) {
        if (!Emitted.insert(BB).second)
          continue;
        Body.push_back(emitRegionHeader(*Nested, &R));
        continue;
      }
      if (!Emitted.insert(BB).second)
        continue;
      if (NeededLabels.contains(BB))
        Body.push_back(new (*AST)
                           LabelStmt(SourceLocation(), labelFor(BB),
                                     new (*AST) NullStmt(SourceLocation())));
      emitBlockStmts(BB, Body);
      if (Stmt *T = emitTerminator(BB, &R))
        Body.push_back(T);
    }
    // The latch may already have printed through an overlapping region's
    // arm; skip it then, otherwise its label is defined twice.
    if (Emitted.insert(R.Latch).second) {
      if (NeededLabels.contains(R.Latch))
        Body.push_back(new (*AST)
                           LabelStmt(SourceLocation(), labelFor(R.Latch),
                                     new (*AST) NullStmt(SourceLocation())));
      emitBlockStmts(R.Latch, Body);
    }
    Stmt *B = CompoundStmt::Create(*AST, Body, FPOptionsOverride(),
                                   SourceLocation(), SourceLocation());
    Stmt *Loop = new (*AST) DoStmt(B, Cond, SourceLocation(), SourceLocation(),
                                   SourceLocation());
    ActiveLoops.pop_back();
    return Loop;
  }

  // Generic loops run until an explicit break; other loop kinds test at the header.
  auto *BI = dyn_cast<BranchInst>(R.Header->getTerminator());
  if (R.Kind == RegionKind::Loop) {
    Cond = IntegerLiteral::Create(*AST, APInt(32, 1), AST->IntTy,
                                  SourceLocation());
    SmallPtrSet<Instruction *, 4> FeedsCond;
    if (BI && BI->isConditional()) {
      if (auto *CmpI = dyn_cast<Instruction>(BI->getCondition()))
        FeedsCond.insert(CmpI);
      if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition()))
        for (Value *Op : Cmp->operands())
          if (auto *I = dyn_cast<Instruction>(Op))
            FeedsCond.insert(I);
    }
    for (Instruction &I : *R.Header) {
      if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
          FeedsCond.contains(&I))
        continue;
      if (Stmt *S = Exprs->instr(I, *Scope))
        Body.push_back(S);
    }
    if (Stmt *T = emitTerminator(R.Header, &R))
      Body.push_back(T);
  } else if (BI && BI->isConditional()) {
    // Continuing edge is the successor that is not the exit.
    BasicBlock *Cont = BI->getSuccessor(0) == R.Follow
                           ? BI->getSuccessor(1)
                           : BI->getSuccessor(0);
    Cond = condOf(BI, Cont);
    // Header instructions the condition reads (the icmp itself and the
    // loads it compares) fold into the cond expression through
    // ExprBuilder; emitting them again at body top would run them after
    // the test. Everything else in the header is per-iteration work and
    // goes at the top of the body.
    SmallPtrSet<Instruction *, 4> FeedsCond;
    if (auto *CmpI = dyn_cast<Instruction>(BI->getCondition()))
      FeedsCond.insert(CmpI);
    if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition()))
      for (Value *Op : Cmp->operands())
        if (auto *I = dyn_cast<Instruction>(Op))
          FeedsCond.insert(I);
    for (Instruction &I : *R.Header) {
      if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I))
        continue;
      if (FeedsCond.contains(&I))
        continue;
      if (Stmt *S = Exprs->instr(I, *Scope))
        Body.push_back(S);
    }
  } else {
    Cond = R.Kind == RegionKind::For
               ? nullptr
               : IntegerLiteral::Create(*AST, APInt(32, 1), AST->IntTy,
                                        SourceLocation());
  }

  for (BasicBlock *BB : Blocks) {
    if (!R.Members.contains(BB) || BB == R.Header)
      continue;
    // The latch's branch back to the header is implicit in the loop
    // construct. Its leaf instructions remain in the body. If an external
    // edge targets the latch, collectRegions requests a label here.
    if (BB == R.Latch) {
      if (!Emitted.insert(BB).second)
        continue;
      if (NeededLabels.contains(BB))
        Body.push_back(new (*AST)
                           LabelStmt(SourceLocation(), labelFor(BB),
                                     new (*AST) NullStmt(SourceLocation())));
      emitBlockStmts(BB, Body);
      continue;
    }
    if (Region *Nested = regionOf(BB); Nested && !Nested->Degraded) {
      if (!Emitted.insert(BB).second)
        continue;
      Body.push_back(emitRegionHeader(*Nested, &R));
      continue;
    }
    if (!Emitted.insert(BB).second)
      continue;
    if (NeededLabels.contains(BB))
      Body.push_back(new (*AST)
                         LabelStmt(SourceLocation(), labelFor(BB),
                                   new (*AST) NullStmt(SourceLocation())));
    // ForClaimed already filters the instructions a for-init/step
    // consumed, so the latch can go through the normal leaf path.
    emitBlockStmts(BB, Body);
    if (Stmt *T = emitTerminator(BB, &R))
      Body.push_back(T);
  }
  Stmt *B = CompoundStmt::Create(*AST, Body, FPOptionsOverride(),
                                 SourceLocation(), SourceLocation());
  Stmt *Loop = nullptr;
  if (R.Kind == RegionKind::For)
    Loop = new (*AST)
        ForStmt(*AST, Init, Cond, nullptr, Step, B, SourceLocation(),
                SourceLocation(), SourceLocation());
  else
    Loop = WhileStmt::Create(*AST, nullptr, Cond, B, SourceLocation(),
                             SourceLocation(), SourceLocation());
  ActiveLoops.pop_back();
  return Loop;
}

Stmt *StructuredStmtBuilder::emitSwitch(Region &R, const Region *Outer) {
  auto *SI = dyn_cast<SwitchInst>(R.Header->getTerminator());
  if (!SI) {
    SmallVector<Stmt *, 8> BS;
    emitBlockStmts(R.Header, BS);
    emitMembers(R, R.Members, BS);
    return CompoundStmt::Create(*AST, BS, FPOptionsOverride(),
                                SourceLocation(), SourceLocation());
  }
  Expr *Cond = Exprs->cond(SI->getCondition(), *Scope);
  QualType CT = Cond ? Cond->getType() : AST->IntTy;
  Cond = widenSwitchCond(Cond, CT, SI);
  SwitchStmt *SW = SwitchStmt::Create(*AST, nullptr, nullptr, Cond,
                                      SourceLocation(), SourceLocation());

  // Group case values by destination, preserving switch order; shared
  // destinations print as stacked labels over one body.
  SmallVector<BasicBlock *, 8> DestOrder;
  DenseMap<BasicBlock *, SmallVector<ConstantInt *, 4>> DestCases;
  SmallPtrSet<BasicBlock *, 8> DestSet;
  for (auto &Case : SI->cases()) {
    BasicBlock *D = Case.getCaseSuccessor();
    if (!DestCases.count(D)) {
      DestCases[D];
      DestOrder.push_back(D);
    }
      DestCases[D].push_back(Case.getCaseValue());
    DestSet.insert(D);
  }
  BasicBlock *Def = SI->getDefaultDest();
  if (Def)
    DestSet.insert(Def);

  SmallVector<Stmt *, 8> Body;
  auto emitCase = [&](BasicBlock *D) -> Stmt * {
    // The case body is D plus region members dominated by D that are not
    // dominated by another case destination or the follow.
    SmallVector<Stmt *, 8> SS;
    if (D != R.Follow && R.Members.contains(D)) {
      SmallPtrSet<BasicBlock *, 8> Mem;
      Mem.insert(D);
      for (BasicBlock *BB : R.Members)
        if (BB != D && DT->dominates(D, BB)) {
          bool Stolen = false;
          for (BasicBlock *O : DestSet)
            if (O != D && O != R.Follow && DT->dominates(O, BB))
              Stolen = true;
          if (!Stolen)
            Mem.insert(BB);
        }
      emitMembers(R, Mem, SS);
    }
    // Implicit break when the body does not already transfer control and
    // does not end in a goto into another case destination
    // (emit-c: `!fall && !terminal -> break`).
    bool EndsInCaseGoto = false;
    if (!SS.empty())
      if (auto *G = dyn_cast<GotoStmt>(SS.back())) {
        for (auto &KV : Labels)
          if (KV.second == G->getLabel() && DestSet.contains(KV.first))
            EndsInCaseGoto = true;
      }
    if (!endsTerminal(SS) && !EndsInCaseGoto)
      SS.push_back(new (*AST) BreakStmt(SourceLocation()));
    if (SS.size() == 1)
      return SS.front();
    if (SS.empty())
      return new (*AST) NullStmt(SourceLocation());
    return CompoundStmt::Create(*AST, SS, FPOptionsOverride(),
                                SourceLocation(), SourceLocation());
  };

  for (BasicBlock *D : DestOrder) {
    Stmt *CaseBody = emitCase(D);
    bool SharesDefault = Def == D;
    Stmt *First = nullptr;
    SwitchCase *Prev = nullptr;
    auto setSubStmt = [&](SwitchCase *Label, Stmt *Sub) {
      if (auto *Case = dyn_cast<CaseStmt>(Label))
        Case->setSubStmt(Sub);
      else
        cast<DefaultStmt>(Label)->setSubStmt(Sub);
    };
    auto appendLabel = [&](SwitchCase *Label) {
      if (Prev)
        setSubStmt(Prev, Label);
      else
        First = Label;
      Prev = Label;
    };
    if (SharesDefault)
      appendLabel(new (*AST)
                      DefaultStmt(SourceLocation(), SourceLocation(), nullptr));
    for (ConstantInt *V : DestCases[D]) {
      Expr *LH = caseValue(V, CT);
      appendLabel(CaseStmt::Create(*AST, LH, nullptr, SourceLocation(),
                                   SourceLocation(), SourceLocation()));
    }
    setSubStmt(Prev, CaseBody);
    Body.push_back(First);
  }
  if (Def && !DestCases.count(Def))
    Body.push_back(new (*AST)
                       DefaultStmt(SourceLocation(), SourceLocation(),
                                   emitCase(Def)));
  SW->setBody(CompoundStmt::Create(*AST, Body, FPOptionsOverride(),
                                   SourceLocation(), SourceLocation()));
  return SW;
}

Stmt *StructuredStmtBuilder::emitRegionHeader(Region &R,
                                              const Region *Outer) {
  Emitted.insert(R.Header);
  Stmt *S = nullptr;
  switch (R.Kind) {
  case RegionKind::If:
    S = emitIf(R, Outer);
    break;
  case RegionKind::While:
  case RegionKind::DoWhile:
  case RegionKind::For:
  case RegionKind::Loop:
    S = emitLoop(R, Outer);
    break;
  case RegionKind::Switch:
    S = emitSwitch(R, Outer);
    break;
  }
  if (NeededLabels.contains(R.Header))
    return new (*AST) LabelStmt(SourceLocation(), labelFor(R.Header), S);
  return S;
}

/// Catch/cleanup handler block statements: pad instructions are structural
/// (no C spelling), catchret/cleanupret/resume terminators are control
/// flow consumed by the try construct. Everything else emits normally.
void StructuredStmtBuilder::emitCatchBlockStmts(
    BasicBlock *BB, SmallVectorImpl<Stmt *> &Out) {
  for (Instruction &I : *BB) {
    if (I.isTerminator() || isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I))
      continue;
    if (isa<CatchPadInst>(I) || isa<CleanupPadInst>(I) ||
        isa<LandingPadInst>(I))
      continue;
    if (auto *CI = dyn_cast<CallInst>(&I)) {
      if (Function *Callee = CI->getCalledFunction()) {
        StringRef N = Callee->getName();
        if (N == "__cxa_begin_catch" || N == "__cxa_end_catch" ||
            N == "llvm.eh.exceptioncode" || N == "llvm.eh.typeid.for")
          continue;
      }
    }
    if (Stmt *S = Exprs->instr(I, *Scope))
      Out.push_back(S);
  }
}
VarDecl *StructuredStmtBuilder::catchVariable(MDNode *TypeMD) {
  auto *TypeName = TypeMD && TypeMD->getNumOperands() == 1
                       ? dyn_cast<MDString>(TypeMD->getOperand(0))
                       : nullptr;
  if (!TypeName || !FD)
    return nullptr;
  StringRef Decorated = TypeName->getString();
  SmallVector<StringRef, 4> Parts;
  if (Decorated.starts_with("St")) {
    StringRef Encoded = Decorated.drop_front(2);
    size_t Digits = 0;
    while (Digits < Encoded.size() && isDigit(Encoded[Digits]))
      ++Digits;
    unsigned Length = 0;
    if (!Digits || Encoded.take_front(Digits).getAsInteger(10, Length) ||
        Length == 0 || Length > Encoded.size() - Digits)
      return nullptr;
    Parts.push_back(Encoded.drop_front(Digits).take_front(Length));
    Parts.push_back("std");
  } else {
    if (!Decorated.starts_with(".?AV") && !Decorated.starts_with(".?AU"))
      return nullptr;
    StringRef Body = Decorated.drop_front(4);
    if (!Body.ends_with("@@"))
      return nullptr;
    Body = Body.drop_back(2);
    Body.split(Parts, '@');
  }
  if (Parts.size() < 2 || Parts.front().empty())
    return nullptr;

  DeclContext *DC = AST->getTranslationUnitDecl();
  NestedNameSpecifier Qualifier = NestedNameSpecifier::getGlobal();
  for (auto It = Parts.rbegin(); It != Parts.rend() - 1; ++It) {
    IdentifierInfo *II = Ctx.ident(*It);
    NamespaceDecl *NS = nullptr;
    for (NamedDecl *D : DC->lookup(II))
      if ((NS = dyn_cast<NamespaceDecl>(D)))
        break;
    if (!NS) {
      NS = NamespaceDecl::Create(*AST, DC, false, SourceLocation(),
                                 SourceLocation(), II, nullptr, false);
      DC->addDecl(NS);
    }
    Qualifier = NestedNameSpecifier(*AST, NS, Qualifier);
    DC = NS;
  }

  IdentifierInfo *TypeId = Ctx.ident(Parts.front());
  CXXRecordDecl *Record = nullptr;
  for (NamedDecl *D : DC->lookup(TypeId))
    if ((Record = dyn_cast<CXXRecordDecl>(D)))
      break;
  if (!Record) {
    Record = CXXRecordDecl::Create(*AST, TagTypeKind::Class, DC, SourceLocation(),
                                   SourceLocation(), TypeId, nullptr);
    DC->addDecl(Record);
    Record->startDefinition();
    Record->completeDefinition();
  }

  QualType RecordType = AST->getTypeDeclType(
      ElaboratedTypeKeyword::None,
      Qualifier, Record);
  QualType CatchType =
      AST->getLValueReferenceType(AST->getConstType(RecordType));
  return VarDecl::Create(*AST, FD, SourceLocation(), SourceLocation(),
                         Ctx.ident("e"), CatchType,
                         AST->getTrivialTypeSourceInfo(CatchType), SC_None);
}

/// Try statement from an invoke whose unwind edge reaches EH pads.
/// Shapes (mirroring clang -cc1 lowering, x86_64-pc-windows-msvc):
/// - MSVC/Itanium catch: dispatch holds catchswitch (or landingpad with
///   catch clauses); each handler block holds a catchpad. Emits
///   `try { invoke-call } catch (...) { handler }`.
/// - SEH: dispatch holds catchswitch, handler catchpad has a single null
///   selector. Personality __C_specific_handler selects __try/__except;
///   the filter expression stays opaque (constant 1).
/// - Cleanup-only (cleanuppad/cleanupret, landingpad cleanup): one
///   catch-all handler spelling the unwind-only semantics.
Stmt *StructuredStmtBuilder::tryTry(BasicBlock *BB) {
  auto *II = dyn_cast<InvokeInst>(BB->getTerminator());
  if (!II)
    return nullptr;
  BasicBlock *Dispatch = II->getUnwindDest();
  if (!Dispatch || EHClaimed.contains(BB))
    return nullptr;
  // Dispatch must hold exactly one EH pad instruction.
  CatchSwitchInst *SW = nullptr;
  LandingPadInst *LP = nullptr;
  CleanupPadInst *CP = nullptr;
  for (Instruction &I : *Dispatch) {
    if (isa<CatchSwitchInst>(I))
      SW = cast<CatchSwitchInst>(&I);
    else if (isa<LandingPadInst>(I))
      LP = cast<LandingPadInst>(&I);
    else if (isa<CleanupPadInst>(I))
      CP = cast<CleanupPadInst>(&I);
  }
  unsigned Pads = (SW ? 1 : 0) + (LP ? 1 : 0) + (CP ? 1 : 0);
  if (Pads != 1)
    return nullptr;

  // Collect handler blocks: catchswitch handlers, or the single pad
  // block for landingpad/cleanuppad dispatches.
  SmallVector<BasicBlock *, 2> Handlers;
  if (SW) {
    for (BasicBlock *H : SW->handlers())
      Handlers.push_back(H);
  } else {
    Handlers.push_back(Dispatch);
  }
  if (Handlers.empty())
    return nullptr;

  // Personality decides the spelling: __C_specific_handler -> SEH.
  // For non-MSVC targets (gcc/MinGW), never emit SEH.
  bool IsSEH = false;
  if (Fn && Fn->hasPersonalityFn()) {
    llvm::StringRef Triple = Fn->getParent()->getTargetTriple().str();
    if (Triple.contains("msvc")) {
      if (const auto *GV = dyn_cast<GlobalValue>(
              Fn->getPersonalityFn()->stripPointerCasts()))
        IsSEH = GV->getName() == "__C_specific_handler";
    }
  }

  // A decoded try-table entry can protect several invoke sites. Gather the
  // calls carrying its region ID and emit one AST try around them, using the
  // first site's shared handler set.
  SmallVector<InvokeInst *, 8> RegionInvokes{II};
  SmallVector<BasicBlock *, 8> RegionBlocks{BB};
  if (MDNode *Region = II->getMetadata("revenant.eh.region")) {
    for (BasicBlock *Candidate : Blocks) {
      if (Candidate == BB || EHClaimed.contains(Candidate))
        continue;
      auto *Other = dyn_cast<InvokeInst>(Candidate->getTerminator());
      if (!Other || Other->getMetadata("revenant.eh.region") != Region)
        continue;
      RegionInvokes.push_back(Other);
      RegionBlocks.push_back(Candidate);
    }
  }

  // Try body: spell each invoke's call through a scratch CallInst so
  // ExprBuilder (which only takes CallInst) prints the real calls.
  SmallVector<Stmt *, 8> TryBody;
  for (InvokeInst *Protected : RegionInvokes) {
    SmallVector<Value *, 8> Args(Protected->args());
    CallInst *ScratchCall = CallInst::Create(
        Protected->getFunctionType(), Protected->getCalledOperand(), Args);
    ScratchCall->setCallingConv(Protected->getCallingConv());
    ScratchCall->setAttributes(Protected->getAttributes());
    if (Stmt *S = Exprs->instr(*ScratchCall, *Scope))
      TryBody.push_back(S);
    delete ScratchCall;
  }

  for (BasicBlock *Protected : RegionBlocks) {
    EHClaimed.insert(Protected);
    BasicBlock *ProtectedDispatch =
        cast<InvokeInst>(Protected->getTerminator())->getUnwindDest();
    EHClaimed.insert(ProtectedDispatch);
    for (Instruction &I : *ProtectedDispatch)
      if (auto *ProtectedSwitch = dyn_cast<CatchSwitchInst>(&I))
        for (BasicBlock *H : ProtectedSwitch->handlers())
          EHClaimed.insert(H);
  }

  if (IsSEH && CP && CP->getMetadata("revenant.finally")) {
    SmallVector<Stmt *, 4> FinallyBody;
    emitCatchBlockStmts(Dispatch, FinallyBody);
    if (FinallyBody.empty())
      FinallyBody.push_back(new (*AST) NullStmt(SourceLocation(), false));
    CompoundStmt *TryC = CompoundStmt::Create(
        *AST, TryBody, FPOptionsOverride(), SourceLocation(), SourceLocation());
    CompoundStmt *HC = CompoundStmt::Create(
        *AST, FinallyBody, FPOptionsOverride(), SourceLocation(),
        SourceLocation());
    Stmt *Handler = SEHFinallyStmt::Create(*AST, SourceLocation(), HC);
    return SEHTryStmt::Create(*AST, false, SourceLocation(), TryC, Handler);
  }
  if (IsSEH) {
    SmallVector<Stmt *, 4> HandlerBody;
    for (BasicBlock *H : Handlers)
      emitCatchBlockStmts(H, HandlerBody);
    if (HandlerBody.empty())
      HandlerBody.push_back(new (*AST) NullStmt(SourceLocation(), false));
    CompoundStmt *TryC = CompoundStmt::Create(
        *AST, TryBody, FPOptionsOverride(), SourceLocation(), SourceLocation());
    CompoundStmt *HC = CompoundStmt::Create(
        *AST, HandlerBody, FPOptionsOverride(), SourceLocation(),
        SourceLocation());
    // Filter expression is opaque (real filters stay asm calls); emit 1.
    Expr *Filter = IntegerLiteral::Create(*AST, APInt(32, 1), AST->IntTy,
                                          SourceLocation());
    Stmt *Handler = SEHExceptStmt::Create(*AST, SourceLocation(), Filter, HC);
    return SEHTryStmt::Create(*AST, false, SourceLocation(), TryC, Handler);
  }

  // C++ try/catch (MSVC catchpad, MinGW landingpad, cleanuppad): one handler
  // Preserve catch types recovered from MSVC RTTI metadata.
  SmallVector<Stmt *, 2> CatchStmts;
  for (BasicBlock *H : Handlers) {
    VarDecl *ExceptionDecl = nullptr;
    for (Instruction &I : *H) {
      if (auto *Pad = dyn_cast<CatchPadInst>(&I))
        ExceptionDecl = catchVariable(Pad->getMetadata("revenant.catchtype"));
      else if (auto *LP = dyn_cast<LandingPadInst>(&I))
        ExceptionDecl = catchVariable(LP->getMetadata("revenant.catchtype"));
      if (ExceptionDecl)
        break;
    }
    SmallVector<Stmt *, 4> HandlerBody;
    emitCatchBlockStmts(H, HandlerBody);
    if (auto *CR = dyn_cast<CatchReturnInst>(H->getTerminator())) {
      BasicBlock *Continuation = CR->getSuccessor();
      for (Instruction &I : *Continuation) {
        auto *ReturnValue = dyn_cast<PHINode>(&I);
        if (!ReturnValue)
          continue;
        bool Returned = false;
        for (User *U : ReturnValue->users())
          if (auto *RI = dyn_cast<ReturnInst>(U))
            Returned |= RI->getReturnValue() == ReturnValue;
        int Index = ReturnValue->getBasicBlockIndex(H);
        if (!Returned || Index < 0)
          continue;
        Expr *Value = Exprs->retVal(ReturnValue->getIncomingValue(Index),
                                    *Scope);
        HandlerBody.push_back(
            ReturnStmt::Create(*AST, SourceLocation(), Value, nullptr));
        break;
      }
    }
    if (auto *CR = dyn_cast<CatchReturnInst>(H->getTerminator())) {
      BasicBlock *Continuation = CR->getSuccessor();
      SmallPtrSet<BasicBlock *, 8> Seen;
      for (unsigned Depth = 0; Continuation && Depth != 8 &&
                               Seen.insert(Continuation).second;
           ++Depth) {
        auto *RI = dyn_cast<ReturnInst>(Continuation->getTerminator());
        if (RI) {
          Value *ReturnValue = RI->getReturnValue();
          if (ReturnValue)
            if (auto *Load = dyn_cast<LoadInst>(ReturnValue))
              for (Instruction &I : *H)
                if (auto *Store = dyn_cast<StoreInst>(&I);
                    Store &&
                    Store->getPointerOperand() == Load->getPointerOperand())
                  ReturnValue = Store->getValueOperand();
          if (ReturnValue)
            HandlerBody.push_back(ReturnStmt::Create(
                *AST, SourceLocation(), Exprs->retVal(ReturnValue, *Scope),
                nullptr));
          break;
        }
        auto *BI = dyn_cast<BranchInst>(Continuation->getTerminator());
        if (!BI || BI->isConditional())
          break;
        Continuation = BI->getSuccessor(0);
      }
    }
    if (HandlerBody.empty())
      HandlerBody.push_back(new (*AST) NullStmt(SourceLocation(), false));
    CompoundStmt *HC = CompoundStmt::Create(
        *AST, HandlerBody, FPOptionsOverride(), SourceLocation(),
        SourceLocation());
    CatchStmts.push_back(
        new (*AST) CXXCatchStmt(SourceLocation(), ExceptionDecl, HC));
  }
  CompoundStmt *TryC = CompoundStmt::Create(
      *AST, TryBody, FPOptionsOverride(), SourceLocation(), SourceLocation());
  return CXXTryStmt::Create(*AST, SourceLocation(), TryC, CatchStmts);
}

CompoundStmt *StructuredStmtBuilder::buildBody(Function &F, FunctionDecl *FD,
                                             ExprBuilder &Exprs,
                                             ExprScope &Scope) {
  Fn = &F;
  this->FD = FD;
  this->Exprs = &Exprs;
  this->Scope = &Scope;
  AST = &Ctx.ast();
  Blocks.clear();
  Labels.clear();
  LabelNames.clear();
  NeededLabels.clear();
  Regions.clear();
  ScTags.clear();
  ForClaimed.clear();
  EHClaimed.clear();
  Emitted.clear();
  ActiveLoops.clear();
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);

  DT = std::make_unique<DominatorTree>(F);
  PDT = std::make_unique<PostDominatorTree>(F);
  Exprs.beginFunction(F, FD, Scope);
  collectRegions();

  SmallVector<Stmt *, 32> Stmts;
  for (BasicBlock *BB : Blocks) {
    // Native LLVM EH pads print as try/catch before region logic runs:
    // they carry no region annotation and must not degrade to labels.
    if (!EHClaimed.contains(BB)) {
      if (Stmt *Try = tryTry(BB)) {
        Stmts.push_back(Try);
        continue;
      }
    } else {
      continue; // dispatch/handler block already inside a try statement
    }
    Region *Parent = enclosingRegion(BB);
    if (Region *R = regionOf(BB);
        R && !R->Degraded && (!Parent || !Parent->Degraded)) {
      // Region headers only emit at the top level when they are not
      // nested inside another region's member set.
      if (!Parent)
        Stmts.push_back(emitRegionHeader(*R, nullptr));
      continue;
    }
    if (Parent && !Parent->Degraded)
      continue; // emitted inside a structured region
    // Blocks claimed by a region's arm sets already printed through
    // the region; never print them flat as well.
    if (!Emitted.insert(BB).second)
      continue;
    // A region follow is emitted linearly after the structured construct;
    // it needs a label only when a real goto targets it.
    bool IsRegionFollow = false;
    for (const auto &KV : Regions)
      if (KV.second->Follow == BB && !KV.second->Degraded) {
        IsRegionFollow = true;
        break;
      }
    SmallVector<Stmt *, 8> BS;
    emitBlockStmts(BB, BS);
    if (Stmt *T = emitTerminator(BB, Parent && Parent->Degraded ? Parent
                                                                : nullptr))
      BS.push_back(T);
    CompoundStmt *Inner = CompoundStmt::Create(*AST, BS, FPOptionsOverride(),
                                               SourceLocation(),
                                               SourceLocation());
    if (IsRegionFollow && !NeededLabels.contains(BB))
      Stmts.push_back(Inner);
    else
      Stmts.push_back(new (*AST)
                          LabelStmt(SourceLocation(), labelFor(BB), Inner));
  }
  for (BasicBlock *BB : Blocks) {
    if (!Emitted.insert(BB).second)
      continue;
    SmallVector<Stmt *, 8> Body;
    emitBlockStmts(BB, Body);
    if (Stmt *T = emitTerminator(BB, nullptr))
      Body.push_back(T);
    CompoundStmt *Block = CompoundStmt::Create(*AST, Body,
                                                FPOptionsOverride(),
                                                SourceLocation(),
                                                SourceLocation());
    Stmts.push_back(
        new (*AST) LabelStmt(SourceLocation(), labelFor(BB), Block));
  }
  return CompoundStmt::Create(*AST, Stmts, FPOptionsOverride(),
                              SourceLocation(), SourceLocation());
}

} // namespace

std::unique_ptr<StmtBuilder> createStmtBuilder(FrontendContext &Ctx,
                                             TypeBuilder &Types) {
  return std::make_unique<StructuredStmtBuilder>(Ctx, Types);
}

} // namespace revenant::clangx
