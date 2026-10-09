// Exact CFG matching deliberately does not undo jump threading or folding.
// Entry may have a caller prefix; the unique return block may have a caller
// suffix. All other blocks, edges, PHIs and operations must match exactly.
struct CFGBody {
  Function *Fn;
  const ReturnInst *Ret = nullptr;
  bool Valid = false;
};

CFGBody analyzeCFG(Function &F) {
  CFGBody B{&F};
  if (F.isDeclaration() || F.isIntrinsic() || F.hasAddressTaken() ||
      F.isVarArg() || F.size() < 2 || F.size() > 32 ||
      F.arg_size() > MaxOperands || !F.front().phis().empty())
    return B;
  unsigned Count = 0;
  AttributeSet ReturnAttrs = F.getAttributes().getRetAttrs();
  if (ReturnAttrs.hasAttribute(Attribute::SExt) ||
      ReturnAttrs.hasAttribute(Attribute::ZExt) ||
      ReturnAttrs.hasAttribute(Attribute::InReg))
    return B;
  bool SawCall = false;
  for (const Argument &A : F.args()) {
    if (!A.getType()->isFirstClassType() || A.getType()->isTokenTy() ||
        A.hasByValAttr() || A.hasStructRetAttr() || A.hasInAllocaAttr() ||
        A.hasPreallocatedAttr() || A.hasAttribute(Attribute::SExt) ||
        A.hasAttribute(Attribute::ZExt) || A.hasAttribute(Attribute::InReg) ||
        A.hasAttribute(Attribute::Nest) || A.hasAttribute(Attribute::SwiftSelf) ||
        A.hasAttribute(Attribute::SwiftError))
      return B;
  }
  for (const BasicBlock &BB : F) {
    if (BB.hasAddressTaken() || BB.isEHPad())
      return B;
    for (const Instruction &I : BB) {
      if (++Count > MaxCalleeInsts || I.hasMetadata("revenant.opaque") ||
          I.hasMetadata("revenant.region") || isa<AllocaInst>(I) ||
          I.isAtomic() || isSkippableMarker(I))
        return B;
      if (const auto *Ret = dyn_cast<ReturnInst>(&I)) {
        if (B.Ret)
          return B;
        B.Ret = Ret;
      } else if (I.isTerminator()) {
        if (!isa<BranchInst>(I) && !isa<SwitchInst>(I))
          return B;
      } else if (!isPureForMatch(I, SawCall)) {
        return B;
      }
    }
  }
  B.Valid = B.Ret != nullptr;
  return B;
}

struct CFGRegion {
  SmallVector<BasicBlock *, 16> Blocks;
  SmallVector<Instruction *, 64> Insts;
  SmallVector<Value *, 8> Args;
  Instruction *Start = nullptr;
  Instruction *After = nullptr;
  Value *Escape = nullptr;
};

bool matchCFG(const CFGBody &B, Instruction &Start, CFGRegion &R) {
  Function &Callee = *B.Fn;
  Function &Caller = *Start.getFunction();
  if (&Callee == &Caller)
    return false;
  DenseMap<const BasicBlock *, BasicBlock *> Blocks;
  SmallPtrSet<BasicBlock *, 32> Seen;
  SmallVector<const BasicBlock *, 16> Queue{&Callee.front()};
  Blocks[&Callee.front()] = Start.getParent();
  Seen.insert(Start.getParent());
  // Successor order is part of the proof: true/false and switch cases are
  // compared as operands too. A block cannot represent two callee blocks.
  for (unsigned Index = 0; Index < Queue.size(); ++Index) {
    const BasicBlock *C = Queue[Index];
    BasicBlock *T = Blocks.lookup(C);
    if (T->hasAddressTaken() || T->isEHPad())
      return false;
    const Instruction *CT = C->getTerminator();
    Instruction *TT = T->getTerminator();
    if (isa<ReturnInst>(CT))
      continue;
    if (CT->getOpcode() != TT->getOpcode() ||
        CT->getNumSuccessors() != TT->getNumSuccessors())
      return false;
    for (unsigned S = 0; S < CT->getNumSuccessors(); ++S) {
      const BasicBlock *CS = CT->getSuccessor(S);
      BasicBlock *TS = TT->getSuccessor(S);
      auto It = Blocks.find(CS);
      if (It != Blocks.end()) {
        if (It->second != TS)
          return false;
      } else {
        if (!Seen.insert(TS).second)
          return false;
        Blocks[CS] = TS;
        Queue.push_back(CS);
      }
    }
  }
  if (Queue.size() != Callee.size())
    return false; // Unreachable callee blocks are not evidence.
  Matcher M(Callee);
  for (const BasicBlock *C : Queue)
    M.bindLocal(C, Blocks.lookup(C));
  SmallVector<std::pair<const Instruction *, Instruction *>, 64> Pairs;
  R.Start = &Start;
  for (const BasicBlock *C : Queue) {
    BasicBlock *T = Blocks.lookup(C);
    R.Blocks.push_back(T);
    // Only the entry may have predecessors outside the matched graph.
    if (C != &Callee.front()) {
      for (BasicBlock *P : predecessors(T))
        if (!Seen.contains(P))
          return false;
    } else {
      for (BasicBlock *P : predecessors(T))
        if (Seen.contains(P))
          return false;
    }
    auto TI = C == &Callee.front() ? Start.getIterator() : T->begin();
    for (const Instruction &CI : *C) {
      if (isa<ReturnInst>(CI)) {
        if (TI == T->end())
          return false;
        R.After = &*TI;
        break;
      }
      if (TI == T->end())
        return false;
      Instruction *RI = &*TI++;
      if (RI->hasMetadata("revenant.opaque") ||
          RI->hasMetadata("revenant.region") ||
          !CI.isSameOperationAs(RI) ||
          !CI.hasSameSubclassOptionalData(RI))
        return false;
      M.bindLocal(&CI, RI);
      Pairs.push_back({&CI, RI});
      R.Insts.push_back(RI);
    }
    if (C != B.Ret->getParent() && TI != T->end())
      return false;
  }
  // Prebinding makes loop-carried PHIs and forward block references legal.
  // PHI incoming order is irrelevant; incoming edge identity is not.
  for (auto Pair : Pairs) {
    if (const auto *CP = dyn_cast<PHINode>(Pair.first)) {
      auto *TP = cast<PHINode>(Pair.second);
      for (unsigned I = 0; I < CP->getNumIncomingValues(); ++I) {
        BasicBlock *Pred = Blocks.lookup(CP->getIncomingBlock(I));
        int J = TP->getBasicBlockIndex(Pred);
        if (J < 0 || !M.matchOperand(CP->getIncomingValue(I),
                                    TP->getIncomingValue(J)))
          return false;
      }
    } else if (!matchInst(Pair.first, Pair.second, M)) {
      return false;
    }
  }
  auto Locals = M.snapLocals();
  auto Args = M.args();
  const Value *Return = B.Ret->getReturnValue();
  if (Return) {
    if (isa<Constant>(Return))
      R.Escape = const_cast<Value *>(Return);
    else if (const auto *A = dyn_cast<Argument>(Return))
      R.Escape = Args.lookup(A);
    else
      R.Escape = Locals.lookup(Return);
    if (!R.Escape)
      return false;
  }
  SmallPtrSet<Instruction *, 32> Inside;
  for (Instruction *I : R.Insts)
    Inside.insert(I);
  // Only the returned definition may escape. The exit suffix remains in
  // the caller, including its terminator, so its successor PHIs stay intact.
  for (Instruction *I : R.Insts)
    for (User *U : I->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || (!Inside.contains(UI) && I != R.Escape))
        return false;
    }
  DominatorTree DT(Caller);
  for (const Argument &A : Callee.args()) {
    Value *V = Args.lookup(&A);
    if (!V || V->getType() != A.getType())
      return false;
    if (const auto *I = dyn_cast<Instruction>(V))
      if (Inside.contains(I) || !DT.dominates(I, &Start))
        return false;
    R.Args.push_back(V);
  }
  // Prefix instructions in the exit must have no successors of their own.
  // A branch/switch only occurs in fully matched non-return blocks. Thus
  // splitting the suffix creates the sole exit and introduces no side exit.
  return R.After != nullptr && !isa<PHINode>(R.After);
}

void outlineCFG(const CFGBody &B, CFGRegion &R) {
  BasicBlock *Entry = R.Start->getParent();
  BasicBlock *Exit = R.After->getParent();
  BasicBlock *Follow = Exit->splitBasicBlock(R.After->getIterator(),
                                           "uninline.cont");
  CallInst *Call = CallInst::Create(B.Fn->getFunctionType(), B.Fn, R.Args,
                                  "", R.Start->getIterator());
  Call->setCallingConv(B.Fn->getCallingConv());
  // The callee's parameter contracts were consumed by inlining. Do not
  // reintroduce inferred noundef/nonnull/alignment restrictions on inputs.
  // ABI-changing parameter attributes are rejected during analysis.
  for (Instruction *I : R.Insts)
    copyVA(I, Call);
  if (R.Escape && isa<Instruction>(R.Escape)) {
    SmallPtrSet<Instruction *, 32> Inside;
    for (Instruction *I : R.Insts)
      Inside.insert(I);
    R.Escape->replaceUsesWithIf(Call, [&](Use &U) {
      return !Inside.contains(dyn_cast<Instruction>(U.getUser()));
    });
  }
  Instruction *OldTerm = Entry->getTerminator();
  BranchInst *Branch = BranchInst::Create(Follow, OldTerm->getIterator());
  copyVA(OldTerm, Branch);
  // Clear cyclic PHI references before deletion. All outside uses have
  // either been proved absent or replaced by the call.
  for (Instruction *I : R.Insts)
    I->dropAllReferences();
  for (Instruction *I : R.Insts)
    if (I->getParent() == Entry)
      I->eraseFromParent();
  for (BasicBlock *BB : R.Blocks)
    if (BB != Entry)
      BB->dropAllReferences();
  for (BasicBlock *BB : R.Blocks)
    if (BB != Entry)
      BB->eraseFromParent();
}

bool tryUninlineCFG(Function &Caller, const CFGBody &B) {
  if (&Caller == B.Fn)
    return false;
  for (BasicBlock &BB : Caller)
    for (Instruction &I : BB) {
      if (isa<PHINode>(I))
        continue;
      CFGRegion R;
      if (!matchCFG(B, I, R))
        continue;
      outlineCFG(B, R);
      return true; // Iterator invalidation: the driver restarts the function.
    }
  return false;
}
