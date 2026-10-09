// Exact CFG matching deliberately does not undo jump threading or folding.
// Entry may have a caller prefix; the unique return block may have a caller
// suffix. All other blocks, edges, PHIs and operations must match exactly.
// Metadata/constant ranges do not recursively inspect PHI incoming values.
// Prove each arm instead; pessimistic memoization rejects cycles without
// exponential traversal of shared PHI/select graphs.
bool provesReturnRange(Value *V, const ConstantRange &Expected,
                       DenseMap<const Value *, bool> &Cache) {
  auto It = Cache.find(V);
  if (It != Cache.end())
    return It->second;
  Cache[V] = false;
  bool Proved = Expected.contains(computeConstantRange(V, false));
  if (!Proved) {
    if (auto *Phi = dyn_cast<PHINode>(V)) {
      Proved = true;
      for (Value *Incoming : Phi->incoming_values())
        Proved &= provesReturnRange(Incoming, Expected, Cache);
    } else if (auto *Select = dyn_cast<SelectInst>(V)) {
      Proved = provesReturnRange(Select->getTrueValue(), Expected, Cache) &&
               provesReturnRange(Select->getFalseValue(), Expected, Cache);
    }
  }
  Cache[V] = Proved;
  return Proved;
}

struct CFGBody {
  Function *Fn;
  SmallVector<const ReturnInst *, 4> Returns;
  bool Valid = false;
};

CFGBody analyzeCFG(Function &F) {
  CFGBody B{&F};
  if (F.isDeclaration() || F.isIntrinsic() || F.hasAddressTaken() ||
      F.isInterposable() || F.hasFnAttribute(Attribute::ReturnsTwice) ||
      F.hasFnAttribute(Attribute::Convergent) ||
      F.isVarArg() || F.size() < 2 || F.size() > 32 ||
      F.arg_size() > MaxOperands || !F.front().phis().empty())
    return B;
  unsigned Count = 0;
  unsigned Evidence = 0;
  bool HasControlFlow = false;
  AttributeSet ReturnAttrs = F.getAttributes().getRetAttrs();
  if (ReturnAttrs.hasAttribute(Attribute::SExt) ||
      ReturnAttrs.hasAttribute(Attribute::ZExt) ||
      ReturnAttrs.hasAttribute(Attribute::InReg))
    return B;
  for (const Argument &A : F.args()) {
    if (!A.getType()->isFirstClassType() || A.getType()->isTokenTy() ||
        A.hasByValAttr() || A.hasStructRetAttr() || A.hasInAllocaAttr() ||
        A.hasPreallocatedAttr() || A.hasAttribute(Attribute::SExt) ||
        A.hasAttribute(Attribute::ZExt) || A.hasAttribute(Attribute::InReg) ||
        A.hasAttribute(Attribute::Nest) || A.hasAttribute(Attribute::SwiftSelf) ||
        A.hasAttribute(Attribute::SwiftError) ||
        A.hasAttribute(Attribute::NoAlias))
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
        B.Returns.push_back(Ret);
      } else if (I.isTerminator()) {
        if (!isa<BranchInst>(I) && !isa<SwitchInst>(I))
          return B;
        HasControlFlow |= I.getNumSuccessors() > 1;
      } else if (const auto *Call = dyn_cast<CallInst>(&I)) {
        if (!Call->getCalledFunction() || Call->isInlineAsm() ||
            Call->getCalledFunction()->isIntrinsic() ||
            Call->isConvergent() || Call->cannotDuplicate() ||
            Call->isMustTailCall() || Call->hasOperandBundles() ||
            Call->hasFnAttr(Attribute::ReturnsTwice))
          return B;
      } else if (I.isVolatile() || isa<VAArgInst>(I) || isa<FenceInst>(I)) {
        return B;
      }
      Evidence += !I.isTerminator();
    }
  }
  B.Valid = !B.Returns.empty() && (HasControlFlow || Evidence >= 3);
  return B;
}

struct CFGRegion {
  SmallVector<BasicBlock *, 16> Blocks;
  SmallVector<Instruction *, 64> Insts;
  SmallVector<Value *, 8> Args;
  Instruction *Start = nullptr;
  Instruction *After = nullptr;
  Value *Escape = nullptr;
  BasicBlock *Follow = nullptr;
  DenseMap<BasicBlock *, Value *> ReturnValues;
  // A null replacement denotes the reconstructed call result.
  SmallVector<std::pair<PHINode *, Value *>, 4> ExitValues;
};

bool matchCFG(const CFGBody &B, Instruction &Start, CFGRegion &R) {
  Function &Callee = *B.Fn;
  Function &Caller = *Start.getFunction();
  if (&Callee == &Caller)
    return false;
  if (Callee.hasFnAttribute(Attribute::NullPointerIsValid) !=
      Caller.hasFnAttribute(Attribute::NullPointerIsValid))
    return false;
  for (StringRef Name : {"denormal-fp-math", "denormal-fp-math-f32"})
    if (Callee.getFnAttribute(Name) != Caller.getFnAttribute(Name))
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
        if (B.Returns.size() == 1) {
          R.After = &*TI;
        } else {
          auto *Branch = dyn_cast<BranchInst>(&*TI++);
          if (!Branch || !Branch->isUnconditional() ||
              Branch->hasMetadata("revenant.opaque") ||
              Branch->hasMetadata("revenant.region"))
            return false;
          BasicBlock *Follow = Branch->getSuccessor(0);
          if (R.Follow && R.Follow != Follow)
            return false;
          R.Follow = Follow;
          R.Insts.push_back(Branch);
        }
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
      for (StringRef Name : {"range", "nonnull", "align", "dereferenceable",
                             "dereferenceable_or_null", "noundef",
                             "invariant.load", "invariant.group", "alias.scope",
                             "noalias", "fpmath", "nofpclass"})
        if (CI.getMetadata(Name) != RI->getMetadata(Name))
          return false;
      M.bindLocal(&CI, RI);
      Pairs.push_back({&CI, RI});
      R.Insts.push_back(RI);
    }
    if ((B.Returns.size() > 1 || !isa<ReturnInst>(C->getTerminator())) &&
        TI != T->end())
      return false;
  }
  // Prebinding makes loop-carried PHIs and forward block references legal.
  // PHI incoming order is irrelevant; incoming edge identity is not.
  for (auto Pair : Pairs) {
    if (const auto *CC = dyn_cast<CallInst>(Pair.first)) {
      const auto *TC = cast<CallInst>(Pair.second);
      if (CC->getAttributes() != TC->getAttributes() ||
          CC->getTailCallKind() != TC->getTailCallKind())
        return false;
    }
    if (const auto *Load = dyn_cast<LoadInst>(Pair.first))
      if (Load->getAlign() != cast<LoadInst>(Pair.second)->getAlign())
        return false;
    if (const auto *Store = dyn_cast<StoreInst>(Pair.first))
      if (Store->getAlign() != cast<StoreInst>(Pair.second)->getAlign())
        return false;
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
  for (const ReturnInst *Ret : B.Returns) {
    Value *V = M.boundValue(Ret->getReturnValue());
    if (Ret->getReturnValue() && !V)
      return false;
    if (B.Returns.size() == 1)
      R.Escape = V;
    else
      R.ReturnValues[Blocks.lookup(Ret->getParent())] = V;
  }
  SmallPtrSet<Instruction *, 32> Inside;
  for (Instruction *I : R.Insts)
    Inside.insert(I);
  if (R.Follow) {
    if (Seen.contains(R.Follow))
      return false;
    for (PHINode &Phi : R.Follow->phis()) {
      bool IsReturn = !Callee.getReturnType()->isVoidTy();
      Value *Uniform = nullptr;
      bool IsUniform = true;
      for (auto Item : R.ReturnValues) {
        int Index = Phi.getBasicBlockIndex(Item.first);
        if (Index < 0)
          return false;
        Value *V = Phi.getIncomingValue(Index);
        IsReturn &= V == Item.second;
        if (!Uniform)
          Uniform = V;
        else
          IsUniform &= V == Uniform;
      }
      if (IsReturn) {
        R.ExitValues.push_back({&Phi, nullptr});
      } else {
        auto *I = dyn_cast<Instruction>(Uniform);
        if (!IsUniform || (I && Inside.contains(I)))
          return false;
        R.ExitValues.push_back({&Phi, Uniform});
      }
    }
  }
  // Only the return may escape, either directly into the unique exit
  // suffix or through proved return PHIs at the shared continuation.
  for (Instruction *I : R.Insts)
    for (User *U : I->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI)
        return false;
      if (Inside.contains(UI) || (!R.Follow && I == R.Escape))
        continue;
      bool ReturnUse = false;
      for (auto Item : R.ExitValues)
        ReturnUse |= Item.first == UI && !Item.second;
      if (!ReturnUse)
        return false;
    }
  DominatorTree DT(Caller);
  for (Attribute Attr : Callee.getAttributes().getRetAttrs()) {
    if (!Attr.isEnumAttribute() && !Attr.isConstantRangeAttribute())
      return false;
    Attribute::AttrKind Kind = Attr.getKindAsEnum();
    if (Kind != Attribute::NoUndef && Kind != Attribute::Range)
      return false;
    for (const ReturnInst *Ret : B.Returns) {
      Value *V = M.boundValue(Ret->getReturnValue());
      Instruction *Context = R.Follow
                                 ? Blocks.lookup(Ret->getParent())->getTerminator()
                                 : R.After;
      if (!V)
        return false;
      if (Kind == Attribute::NoUndef &&
          !isGuaranteedNotToBeUndefOrPoison(V, nullptr, Context, &DT))
        return false;
      if (Kind == Attribute::Range) {
        DenseMap<const Value *, bool> Cache;
        if (!provesReturnRange(V, Attr.getValueAsConstantRange(), Cache))
          return false;
      }
    }
  }
  for (auto Item : R.ExitValues)
    if (const auto *I = dyn_cast_or_null<Instruction>(Item.second))
      if (!DT.dominates(I, &Start))
        return false;
  for (const Argument &A : Callee.args()) {
    Value *V = M.boundValue(&A);
    if (!V || V->getType() != A.getType())
      return false;
    if (const auto *I = dyn_cast<Instruction>(V))
      if (Inside.contains(I) || !DT.dominates(I, &Start))
        return false;
    // Inlining can erase argument UB contracts. Reintroducing a call must
    // not make a formerly defined input undefined. Check noundef directly;
    // for the other input restrictions require an identical caller contract.
    if (A.hasAttribute(Attribute::NoUndef) &&
        !isGuaranteedNotToBeUndefOrPoison(V, nullptr, &Start, &DT))
      return false;
    AttributeSet Contracts = Callee.getAttributes().getParamAttrs(A.getArgNo());
    for (Attribute Attr : Contracts) {
      if (!Attr.isEnumAttribute() && !Attr.isIntAttribute() &&
          !Attr.isConstantRangeAttribute())
        continue;
      Attribute::AttrKind Kind = Attr.getKindAsEnum();
      if (Kind != Attribute::NonNull && Kind != Attribute::Alignment &&
          Kind != Attribute::Dereferenceable &&
          Kind != Attribute::DereferenceableOrNull && Kind != Attribute::Range &&
          Kind != Attribute::NoFPClass && Kind != Attribute::NoAlias)
        continue;
      const auto *Input = dyn_cast<Argument>(V);
      if (!Input ||
          Input->getParent()->getAttributes().getParamAttr(Input->getArgNo(),
                                                         Kind) != Attr)
        return false;
    }
    R.Args.push_back(V);
  }
  // Prefix instructions in the exit must have no successors of their own.
  // A branch/switch only occurs in fully matched non-return blocks. Thus
  // splitting the suffix creates the sole exit and introduces no side exit.
  return R.Follow || (R.After != nullptr && !isa<PHINode>(R.After));
}

void outlineCFG(const CFGBody &B, CFGRegion &R) {
  BasicBlock *Entry = R.Start->getParent();
  BasicBlock *Follow = R.Follow;
  if (!Follow)
    Follow = R.After->getParent()->splitBasicBlock(R.After->getIterator(),
                                                 "uninline.cont");
  CallInst *Call = CallInst::Create(B.Fn->getFunctionType(), B.Fn, R.Args,
                                  "", R.Start->getIterator());
  Call->setCallingConv(B.Fn->getCallingConv());
  // The callee's parameter contracts were consumed by inlining. Do not
  // reintroduce inferred noundef/nonnull/alignment restrictions on inputs.
  // ABI-changing parameter attributes are rejected during analysis.
  for (Instruction *I : R.Insts)
    copyVA(I, Call);
  for (auto Item : R.ExitValues) {
    PHINode *Phi = Item.first;
    for (unsigned I = Phi->getNumIncomingValues(); I-- > 0;)
      if (R.ReturnValues.count(Phi->getIncomingBlock(I)))
        Phi->removeIncomingValue(I, false);
    Phi->addIncoming(Item.second ? Item.second : Call, Entry);
  }
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
      if (isa<PHINode>(I) ||
          !B.Fn->front().front().isSameOperationAs(&I) ||
          !B.Fn->front().front().hasSameSubclassOptionalData(&I))
        continue;
      CFGRegion R;
      if (!matchCFG(B, I, R))
        continue;
      outlineCFG(B, R);
      return true; // Iterator invalidation: the driver restarts the function.
    }
  return false;
}
