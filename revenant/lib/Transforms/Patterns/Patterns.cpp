//===- Patterns.cpp - inline library-idiom recognition ---------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Port of `crates/patterns` (canon.rs, matcher.rs, apply.rs, norm.rs):
// canonicalize oracle IR into slot shapes, anchor-seeded subgraph search
// on the target function, then rewrite contiguous single-block regions
// above the confidence threshold into `__pat_<helper>(args)` calls.
//
// The catalog comes from prebuilt oracle IR (never built here): either a
// directory of cleaned `.ll` oracle modules (one function per snippet
// variant) pointed at by -revenant-patterns-catalog, or the hand-built
// structural patterns below for the shapes every libc family shares
// (memcpy/memset as llvm intrinsics, strlen/memcmp as direct calls).
// Empty/missing catalog: hand-built patterns still apply; a function with
// no match is left untouched (PreservedAnalyses::all).
//
// Safety rules mirror apply.rs: highest confidence first, claimed regions
// never overlap, only single-block contiguous regions rewrite, at most one
// live-out (which becomes the call destination, typed like its original
// definition), everything else is annotation only (here: no IR change).
// Every replacement inherits the replaced range's !revenant.va provenance
// and debug location (manual copy until revenant::ir::copyInstructionVA
// from RevenantIR lands); !revenant.opaque is never dropped.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Patterns/Patterns.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

namespace revenant {

// Parse-once statics owned by this lib. Nullary pass constructor (the
// Pipeline registration calls PatternsPass()); never required, since
// revenant-opt links every pass into one binary and a shared flag name is
// a fatal cl::opt collision.
static cl::opt<std::string>
    PatternsCatalog("revenant-patterns-catalog",
                    cl::desc("Directory of cleaned oracle .ll modules (or a "
                             "single .ll file) for pattern generation; empty "
                             "uses hand-built patterns only"),
                    cl::init(""));
static cl::opt<double>
    PatternsMinConf("revenant-patterns-min-conf",
                    cl::desc("Minimum match coverage for a __pat_* rewrite"),
                    cl::init(0.9));

namespace {

// ---- name normalization (port of norm.rs) -------------------------------//

// Likely a crate/v0 hash or pdb-ish hex suffix: >=8 chars, all hex, at
// least one non-digit (plain decimal constants survive).
static bool isHashToken(StringRef Tok) {
  if (Tok.size() < 8)
    return false;
  bool AnyNonDigit = false;
  for (char C : Tok) {
    if (!isHexDigit(C))
      return false;
    if (!isDigit(C))
      AnyNonDigit = true;
  }
  return AnyNonDigit;
}

// Flatten without demangling (names already demangled or known-plain).
static std::string flattenName(StringRef Text) {
  std::string Out;
  Out.reserve(Text.size() + 1);
  size_t I = 0;
  while (I < Text.size()) {
    while (I < Text.size() && !isAlnum(Text[I]))
      ++I;
    size_t J = I;
    while (J < Text.size() && isAlnum(Text[J]))
      ++J;
    if (J > I) {
      StringRef Tok = Text.slice(I, J);
      if (!isHashToken(Tok)) {
        for (char C : Tok)
          Out.push_back(toLower(C));
        Out.push_back('_');
      }
      I = J;
    }
  }
  return Out;
}

// Demangle (best effort: rust v0, itanium, MSVC) then flatten to
// `_`-joined lowercase tokens. Mirrors norm(): short form preferred,
// raw name fallback.
static std::string normName(StringRef Name) {
  if (char *R = rustDemangle(Name)) {
    std::string S(R);
    free(R);
    // Rust demangle keeps the hash suffix ("...::hash"); flatten drops it.
    return flattenName(S);
  }
  if (char *R = itaniumDemangle(Name, /*ParseParams=*/false)) {
    std::string S(R);
    free(R);
    return flattenName(S);
  }
  int Status = 0;
  if (char *R = microsoftDemangle(Name, nullptr, &Status)) {
    std::string S(R);
    free(R);
    if (Status == 0)
      return flattenName(S);
  }
  return flattenName(Name);
}

// Tokens carrying no idiom signal: crate paths, type wrappers, generic-arg
// crumbs. Everything else is content.
static bool isStopToken(StringRef Tok) {
  static const char *Stop[] = {
      "alloc",   "core",    "std",    "vec",    "string", "str",   "impl",
      "option",  "result",  "ops",    "iter",   "traits", "iterator",
      "iterators", "adapter", "adapters", "accum", "slice", "lang", "rt",
      "sys",     "backtrace", "start", "closure", "env",   "enum2", "struct",
      "ref",     "mut",     "const",  "class",  "fn",     "ptr",   "int",
      "extern",  "vector",  "basic",  "allocator", "char", "bool",  "void",
      "i8",      "i16",     "i32",    "i64",    "i128",   "isize", "u8",
      "u16",     "u32",     "u64",    "u128",   "usize",  "f32",   "f64",
      "aebh",    "h",       "e",      "b",      "x",      "v",     "pat",
  };
  if (Tok.size() < 3)
    return true;
  for (const char *S : Stop)
    if (Tok == S)
      return true;
  bool AllDigit = true;
  for (char C : Tok)
    if (!isDigit(C))
      AllDigit = false;
  if (AllDigit)
    return true;
  // Short hex crumbs like `b7a6` in mangled names.
  if (Tok.size() >= 4) {
    bool AllHex = true, AnyDigit = false;
    for (char C : Tok) {
      if (!isHexDigit(C))
        AllHex = false;
      if (isDigit(C))
        AnyDigit = true;
    }
    if (AllHex && AnyDigit)
      return true;
  }
  return false;
}

static SmallVector<std::string, 8> contentTokens(StringRef Norm) {
  SmallVector<std::string, 8> Out;
  size_t I = 0;
  while (I <= Norm.size()) {
    size_t J = Norm.find('_', I);
    if (J == StringRef::npos)
      J = Norm.size();
    StringRef Tok = Norm.slice(I, J);
    if (!Tok.empty() && !isStopToken(Tok))
      Out.push_back(Tok.str());
    I = J + 1;
  }
  return Out;
}

// Both args already normalized. Full containment first; then every content
// token of the needle must appear in the target.
static bool calleeMatch(StringRef PatNorm, StringRef TargetNorm) {
  if (PatNorm.empty())
    return true;
  if (TargetNorm.contains(PatNorm) || PatNorm.contains(TargetNorm))
    return true;
  auto Needle = contentTokens(PatNorm);
  if (Needle.empty())
    return false;
  auto Hay = contentTokens(TargetNorm);
  for (auto &N : Needle)
    if (!is_contained(Hay, N))
      return false;
  return true;
}

// ---- canonical pattern IR (port of model.rs + canon.rs) -----------------//

enum class PValKind { Slot, Int, Kw, FAny, Glob, Any };

struct PVal {
  PValKind Kind = PValKind::Any;
  uint32_t Id = 0;    // Slot or Kw id
  int64_t Int = 0;    // Int literal
  static PVal slot(uint32_t S) { return {PValKind::Slot, S, 0}; }
  static PVal integer(int64_t K) { return {PValKind::Int, 0, K}; }
  static PVal kw(uint32_t S) { return {PValKind::Kw, S, 0}; }
  static PVal fany() { return {PValKind::FAny, 0, 0}; }
  static PVal glob() { return {PValKind::Glob, 0, 0}; }
  static PVal any() { return {PValKind::Any, 0, 0}; }
};

enum class PInsnKind { Bin, Cmp, Load, Store, GEP, Cast, Call, Select, PHI };

struct PInsn {
  PInsnKind Kind = PInsnKind::Bin;
  uint32_t Dst = UINT32_MAX; // defined slot, or UINT32_MAX for void/store
  unsigned Opcode = 0;       // Bin/Call-target-class/Cmp pred/Cast kind
  unsigned TypeBits = 0;     // 0 means "any type" (erased)
  bool IsIntrinsicCall = false;
  std::string CalleeNeedle; // normalized; empty + Opcode 1 = indirect
  SmallVector<std::string, 2> AltNeedles;
  SmallVector<PVal, 4> Ops;
};

struct PBlock {
  uint32_t Id = 0;
  SmallVector<PInsn, 8> Insns;
  // Terminator: 0=any, 1=br, 2=cbr, 3=ret, 4=unreachable.
  unsigned TermKind = 0;
  PVal TermVal; // cbr cond or ret value
  uint32_t TermT = 0, TermF = 0;
};

enum class RequiredKind { All, Calls, CallsAndGuard };

struct Pattern {
  std::string Name;   // e.g. "rust::vec_push" (may carry @O3 suffix)
  std::string Helper; // e.g. "vec_push" -> rewritten as __pat_vec_push
  SmallVector<PBlock, 4> Blocks;
  SmallVector<size_t, 8> Required; // flat insn indices that must match
  double MinCov = 0.3;
};

// Type kept by canonicalization: only sub-64 ints survive lifting (port of
// Canon::ty). Returns bit width, 0 = any.
static unsigned canonType(Type *T) {
  if (auto *IT = dyn_cast<IntegerType>(T))
    return IT->getBitWidth() < 64 ? IT->getBitWidth() : 0;
  return 0;
}

struct CanonCtx {
  DenseMap<const Value *, uint32_t> Slots;
  uint32_t Next = 0;
  DenseMap<int64_t, unsigned> KCount;
  DenseMap<int64_t, uint32_t> KSlots;
  uint32_t NextK = 0;

  uint32_t slotOf(const Value *V) {
    auto It = Slots.find(V);
    if (It != Slots.end())
      return It->second;
    uint32_t S = Next++;
    Slots[V] = S;
    return S;
  }

  PVal valOf(const Value *V) {
    if (auto *C = dyn_cast<ConstantInt>(V)) {
      int64_t K = C->getSExtValue();
      if (K >= -1 && K <= 1)
        return PVal::integer(K);
      bool Repeated = KCount.lookup(K) > 1;
      if (Repeated) {
        auto It = KSlots.find(K);
        if (It == KSlots.end()) {
          uint32_t S = NextK++;
          KSlots[K] = S;
          return PVal::kw(S);
        }
        return PVal::kw(It->second);
      }
      return PVal::kw(NextK++);
    }
    if (isa<ConstantFP>(V))
      return PVal::fany();
    if (isa<GlobalValue>(V))
      return PVal::glob();
    if (isa<UndefValue>(V) || isa<PoisonValue>(V))
      return PVal::any();
    // Constant expressions: xor(x,x) zero and ptrtoint @global keep their
    // meaning (mirrors basic_val); anything else is identity by pointer.
    if (auto *CE = dyn_cast<ConstantExpr>(V)) {
      if (CE->getOpcode() == Instruction::Xor &&
          CE->getOperand(0) == CE->getOperand(1))
        return PVal::integer(0);
      if (CE->getOpcode() == Instruction::PtrToInt &&
          isa<GlobalValue>(CE->getOperand(0)))
        return PVal::glob();
    }
    return PVal::slot(slotOf(V));
  }

  void collectK(const Value *V) {
    if (auto *C = dyn_cast<ConstantInt>(V)) {
      int64_t K = C->getSExtValue();
      if (K < -1 || K > 1)
        ++KCount[K];
    }
  }
};

static bool isKeptIntrinsic(StringRef Name) {
  // Mirrors KEEP_INTRINSICS in generate.rs: anchors or semantics-bearing.
  static const char *Keep[] = {
      "llvm.mem",          "llvm.vector.reduce", "llvm.abs",
      "llvm.smax",         "llvm.smin",          "llvm.umax",
      "llvm.umin",         "llvm.uadd.with",     "llvm.usub.with",
      "llvm.umul.with",    "llvm.sadd.with",     "llvm.ssub.with",
      "llvm.smul.with",    "llvm.ctlz",          "llvm.cttz",
      "llvm.ctpop",        "llvm.bswap",         "llvm.fma",
  };
  for (const char *K : Keep)
    if (Name.starts_with(K))
      return true;
  return false;
}

// Erase llvm.* calls not in KEEP_INTRINSICS so the reader never sees
// metadata/operand-bundle operands (port of sanitize()).
static void sanitizeFunction(Function &F) {
  SmallVector<Instruction *, 8> Dead;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        Function *Callee = CI->getCalledFunction();
        if (!Callee)
          continue;
        StringRef N = Callee->getName();
        if (N.starts_with("llvm.") && !isKeptIntrinsic(N))
          Dead.push_back(CI);
      }
  for (Instruction *I : Dead)
    I->eraseFromParent();
}

// Unnamed blocks (frontend `%9:`) collapse every block identity: give them
// stable synthetic names before readback (port of oracle_func).
static void nameUnnamedBlocks(Function &F) {
  unsigned I = 0;
  for (BasicBlock &BB : F) {
    if (!BB.hasName())
      BB.setName("pb" + Twine(I));
    ++I;
  }
}

static PInsn canonInsn(CanonCtx &Cx, Instruction &I,
                       ArrayRef<std::string> AltNeedles) {
  PInsn P{};
  auto setDst = [&](Value *V) { P.Dst = Cx.slotOf(V); };
  if (auto *BI = dyn_cast<BinaryOperator>(&I)) {
    P.Kind = PInsnKind::Bin;
    P.Opcode = BI->getOpcode();
    P.TypeBits = canonType(BI->getType());
    setDst(BI);
    P.Ops.push_back(Cx.valOf(BI->getOperand(0)));
    P.Ops.push_back(Cx.valOf(BI->getOperand(1)));
  } else if (auto *CI = dyn_cast<CmpInst>(&I)) {
    P.Kind = PInsnKind::Cmp;
    P.Opcode = CI->getPredicate();
    P.TypeBits = canonType(CI->getOperand(0)->getType());
    setDst(CI);
    P.Ops.push_back(Cx.valOf(CI->getOperand(0)));
    P.Ops.push_back(Cx.valOf(CI->getOperand(1)));
  } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
    P.Kind = PInsnKind::Load;
    P.TypeBits = canonType(LI->getType());
    setDst(LI);
    P.Ops.push_back(Cx.valOf(LI->getPointerOperand()));
  } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
    P.Kind = PInsnKind::Store;
    P.TypeBits = canonType(SI->getValueOperand()->getType());
    P.Dst = UINT32_MAX;
    P.Ops.push_back(Cx.valOf(SI->getValueOperand()));
    P.Ops.push_back(Cx.valOf(SI->getPointerOperand()));
  } else if (auto *GI = dyn_cast<GetElementPtrInst>(&I)) {
    P.Kind = PInsnKind::GEP;
    setDst(GI);
    P.Ops.push_back(Cx.valOf(GI->getPointerOperand()));
    for (Value *Idx : GI->indices())
      P.Ops.push_back(Cx.valOf(Idx));
  } else if (auto *CI = dyn_cast<CastInst>(&I)) {
    P.Kind = PInsnKind::Cast;
    P.Opcode = CI->getOpcode();
    P.TypeBits = canonType(CI->getDestTy());
    setDst(CI);
    P.Ops.push_back(Cx.valOf(CI->getOperand(0)));
  } else if (auto *CI = dyn_cast<CallInst>(&I)) {
    P.Kind = PInsnKind::Call;
    Function *Callee = CI->getCalledFunction();
    if (Callee && (Callee->getName().starts_with("llvm.") ||
                   Callee->getName().starts_with("__x86_"))) {
      P.IsIntrinsicCall = true;
      P.CalleeNeedle = Callee->getName().str();
    } else if (Callee) {
      P.CalleeNeedle = normName(Callee->getName());
      for (auto &A : AltNeedles)
        P.AltNeedles.push_back(A);
    } else {
      P.CalleeNeedle = ""; // indirect
      P.Opcode = 1;
    }
    if (!CI->getType()->isVoidTy())
      setDst(CI);
    else
      P.Dst = UINT32_MAX;
    for (Value *A : CI->args())
      P.Ops.push_back(Cx.valOf(A));
  } else if (auto *SI = dyn_cast<SelectInst>(&I)) {
    P.Kind = PInsnKind::Select;
    setDst(SI);
    P.Ops.push_back(Cx.valOf(SI->getCondition()));
    P.Ops.push_back(Cx.valOf(SI->getTrueValue()));
    P.Ops.push_back(Cx.valOf(SI->getFalseValue()));
  } else if (auto *PI = dyn_cast<PHINode>(&I)) {
    P.Kind = PInsnKind::PHI;
    setDst(PI);
    for (unsigned K = 0, E = PI->getNumIncomingValues(); K < E; ++K)
      P.Ops.push_back(Cx.valOf(PI->getIncomingValue(K)));
  } else {
    // Region-only instructions without shape (alloca) are skipped by the
    // caller, like canon.rs.
    P.Kind = PInsnKind::Bin;
    P.Opcode = UINT_MAX;
  }
  return P;
}

// Canonicalize one oracle function into a Pattern (port of canonicalize).
// Returns false when the function yields no canonical instructions.
static bool canonicalizeFunction(Function &F, const std::string &Name,
                                 const std::string &Helper,
                                 RequiredKind Req, double MinCov,
                                 ArrayRef<std::string> AltNeedles,
                                 Pattern &Out) {
  sanitizeFunction(F);
  nameUnnamedBlocks(F);
  CanonCtx Cx;
  // First walk: constant occurrence counts for Kw slot sharing.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        Cx.collectK(Op);
    if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator()))
      if (BI->isConditional())
        Cx.collectK(BI->getCondition());
    if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
      if (Value *R = RI->getReturnValue())
        Cx.collectK(R);
  }
  for (Argument &A : F.args())
    Cx.slotOf(&A);

  DenseMap<BasicBlock *, uint32_t> Bid;
  uint32_t NextBid = 0;
  for (BasicBlock &BB : F)
    Bid[&BB] = NextBid++;

  Out.Name = Name;
  Out.Helper = Helper;
  Out.MinCov = MinCov;
  Out.Blocks.clear();
  Out.Required.clear();
  for (BasicBlock &BB : F) {
    PBlock PB;
    PB.Id = Bid[&BB];
    for (Instruction &I : BB) {
      if (isa<AllocaInst>(&I))
        continue; // kept out of the DAG, like AtomicRmw/CmpXchg in canon.rs
      if (isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I))
        continue;
      PInsn PI = canonInsn(Cx, I, AltNeedles);
      if (PI.Opcode == UINT_MAX && PI.Kind == PInsnKind::Bin)
        continue;
      PB.Insns.push_back(std::move(PI));
    }
    if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
      if (BI->isConditional()) {
        PB.TermKind = 2;
        PB.TermVal = Cx.valOf(BI->getCondition());
        PB.TermT = Bid[BI->getSuccessor(0)];
        PB.TermF = Bid[BI->getSuccessor(1)];
      } else {
        PB.TermKind = 1;
        PB.TermT = Bid[BI->getSuccessor(0)];
      }
    } else if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
      PB.TermKind = 3;
      if (Value *R = RI->getReturnValue())
        PB.TermVal = Cx.valOf(R);
      else
        PB.TermVal = PVal::any();
    } else if (isa<UnreachableInst>(BB.getTerminator())) {
      PB.TermKind = 4;
    } else {
      PB.TermKind = 0; // switch and friends: don't care
    }
    Out.Blocks.push_back(std::move(PB));
  }

  size_t Total = 0;
  for (auto &B : Out.Blocks)
    Total += B.Insns.size();
  if (Total == 0)
    return false;
  auto flat = [&](size_t BI, size_t II) {
    size_t N = II;
    for (size_t K = 0; K < BI; ++K)
      N += Out.Blocks[K].Insns.size();
    return N;
  };
  if (Req == RequiredKind::All) {
    for (size_t I = 0; I < Total; ++I)
      Out.Required.push_back(I);
  } else {
    for (size_t BI = 0; BI < Out.Blocks.size(); ++BI)
      for (size_t II = 0; II < Out.Blocks[BI].Insns.size(); ++II)
        if (Out.Blocks[BI].Insns[II].Kind == PInsnKind::Call)
          Out.Required.push_back(flat(BI, II));
    if (Req == RequiredKind::CallsAndGuard) {
      // The cmp/select feeding each Cbr whose block holds a required call.
      for (size_t BI = 0; BI < Out.Blocks.size(); ++BI) {
        PBlock &B = Out.Blocks[BI];
        if (B.TermKind != 2 || B.TermVal.Kind != PValKind::Slot)
          continue;
        for (size_t II = 0; II < B.Insns.size(); ++II)
          if (B.Insns[II].Dst == B.TermVal.Id) {
            size_t F = flat(BI, II);
            if (!is_contained(Out.Required, F))
              Out.Required.push_back(F);
          }
      }
    }
  }
  if (Out.Required.empty())
    for (size_t I = 0; I < Total; ++I)
      Out.Required.push_back(I);
  return true;
}

// ---- matcher (port of matcher.rs) --------------------------------------//

using Loc = std::pair<size_t, size_t>; // (block idx, insn idx) in target

struct TargetIndex {
  Function *F = nullptr;
  SmallVector<BasicBlock *, 16> Blocks;
  DenseMap<BasicBlock *, size_t> BlockIdx;
  DenseMap<Instruction *, Loc> Def; // value-defining insn -> its location
  DenseMap<Value *, SmallVector<Loc, 4>> Uses;
  SmallVector<SmallVector<size_t, 4>, 16> Succ, Pred;

  explicit TargetIndex(Function &Func) : F(&Func) {
    for (BasicBlock &BB : Func) {
      BlockIdx[&BB] = Blocks.size();
      Blocks.push_back(&BB);
    }
    Succ.resize(Blocks.size());
    Pred.resize(Blocks.size());
    for (size_t BI = 0; BI < Blocks.size(); ++BI) {
      BasicBlock *BB = Blocks[BI];
      // Def sites with exact insn indices.
      size_t II = 0;
      for (Instruction &I : *BB) {
        if (!I.getType()->isVoidTy())
          Def[&I] = {BI, II};
        ++II;
      }
      Instruction *Term = BB->getTerminator();
      for (unsigned S = 0, E = Term->getNumSuccessors(); S < E; ++S) {
        auto It = BlockIdx.find(Term->getSuccessor(S));
        if (It != BlockIdx.end()) {
          Succ[BI].push_back(It->second);
          Pred[It->second].push_back(BI);
        }
      }
    }
    // Operand reads with exact insn indices (terminator reads use SIZE_MAX).
    Uses.clear();
    for (size_t BI = 0; BI < Blocks.size(); ++BI) {
      size_t II = 0;
      for (Instruction &I : *Blocks[BI]) {
        for (Value *Op : I.operands()) {
          if (isa<Constant>(Op) || isa<BasicBlock>(Op) || isa<MetadataAsValue>(Op))
            continue;
          Uses[Op].push_back({BI, II});
        }
        ++II;
      }
      if (Instruction *T = Blocks[BI]->getTerminator()) {
        for (Value *Op : T->operands()) {
          if (isa<Constant>(Op) || isa<BasicBlock>(Op) || isa<MetadataAsValue>(Op))
            continue;
          Uses[Op].push_back({BI, SIZE_MAX});
        }
      }
    }
  }

  Instruction *insn(Loc L) const {
    auto It = Blocks[L.first]->begin();
    std::advance(It, L.second);
    return &*It;
  }
  size_t insnCount(size_t BI) const {
    size_t N = 0;
    for (auto &I : *Blocks[BI]) {
      (void)I;
      ++N;
    }
    return N;
  }
};

static bool isCommutative(unsigned Op) {
  return Op == Instruction::Add || Op == Instruction::Mul ||
         Op == Instruction::And || Op == Instruction::Or ||
         Op == Instruction::Xor || Op == Instruction::FAdd ||
         Op == Instruction::FMul;
}

// icmp pred of `pred b a` equivalent to `pred a b`.
static unsigned swapPred(unsigned P) {
  switch (P) {
  case CmpInst::ICMP_ULT: return CmpInst::ICMP_UGT;
  case CmpInst::ICMP_ULE: return CmpInst::ICMP_UGE;
  case CmpInst::ICMP_UGT: return CmpInst::ICMP_ULT;
  case CmpInst::ICMP_UGE: return CmpInst::ICMP_ULE;
  case CmpInst::ICMP_SLT: return CmpInst::ICMP_SGT;
  case CmpInst::ICMP_SLE: return CmpInst::ICMP_SGE;
  case CmpInst::ICMP_SGT: return CmpInst::ICMP_SLT;
  case CmpInst::ICMP_SGE: return CmpInst::ICMP_SLE;
  case CmpInst::FCMP_OLT: return CmpInst::FCMP_OGT;
  case CmpInst::FCMP_OLE: return CmpInst::FCMP_OGE;
  case CmpInst::FCMP_OGT: return CmpInst::FCMP_OLT;
  case CmpInst::FCMP_OGE: return CmpInst::FCMP_OLE;
  case CmpInst::FCMP_ULT: return CmpInst::FCMP_UGT;
  case CmpInst::FCMP_ULE: return CmpInst::FCMP_UGE;
  case CmpInst::FCMP_UGT: return CmpInst::FCMP_ULT;
  case CmpInst::FCMP_UGE: return CmpInst::FCMP_ULE;
  default: return P;
  }
}

static bool typeOk(unsigned PatBits, Type *T) {
  if (PatBits == 0)
    return true;
  auto *IT = dyn_cast<IntegerType>(T);
  return IT && IT->getBitWidth() == PatBits;
}

static bool calleeOk(const PInsn &P, CallInst *CI) {
  Function *Callee = CI->getCalledFunction();
  if (P.IsIntrinsicCall) {
    if (!Callee)
      return false;
    StringRef N = Callee->getName();
    // An llvm.* intrinsic can also show up as a plain Direct name.
    return N.contains(StringRef(P.CalleeNeedle)) ||
           StringRef(flattenName(N)).contains(P.CalleeNeedle);
  }
  if (P.Opcode == 1) // indirect pattern
    return Callee == nullptr;
  if (!Callee)
    return false;
  std::string TN = normName(Callee->getName());
  if (calleeMatch(P.CalleeNeedle, TN))
    return true;
  for (auto &A : P.AltNeedles)
    if (calleeMatch(A, TN))
      return true;
  return false;
}

struct MatchState {
  DenseMap<uint32_t, Value *> Vals; // pattern slot -> target value
  DenseMap<uint32_t, int64_t> Kws;  // const-wildcard slot -> constant
  DenseMap<size_t, Loc> Insns;      // pattern flat idx -> target loc
  DenseSet<size_t> Deferred, Skipped;
  DenseMap<uint32_t, size_t> Blks; // pattern block id -> target block idx
  DenseSet<Loc> Used;
};

static bool unifyVal(MatchState &St, const PVal &PV, Value *TV) {
  switch (PV.Kind) {
  case PValKind::Any:
    return true;
  case PValKind::FAny:
    return isa<ConstantFP>(TV);
  case PValKind::Glob:
    return isa<GlobalValue>(TV);
  case PValKind::Int: {
    auto *C = dyn_cast<ConstantInt>(TV);
    return C && C->getSExtValue() == PV.Int;
  }
  case PValKind::Kw: {
    auto *C = dyn_cast<ConstantInt>(TV);
    if (!C)
      return false;
    auto It = St.Kws.find(PV.Id);
    if (It != St.Kws.end())
      return It->second == C->getSExtValue();
    St.Kws[PV.Id] = C->getSExtValue();
    return true;
  }
  case PValKind::Slot: {
    auto It = St.Vals.find(PV.Id);
    if (It != St.Vals.end())
      return It->second == TV;
    St.Vals[PV.Id] = TV;
    return true;
  }
  }
  return false;
}

struct SearchCtx {
  const Pattern *P = nullptr;
  TargetIndex *T = nullptr;
  SmallVector<size_t, 16> FlatOrder;
  DenseSet<uint32_t> DefSlots;
  const DenseSet<Loc> *Excluded = nullptr;
  unsigned Steps = 0;

  bool bindDst(MatchState &St, uint32_t S, Value *TDst) const {
    auto It = St.Vals.find(S);
    if (It != St.Vals.end())
      return It->second == TDst;
    St.Vals[S] = TDst;
    return true;
  }

  bool allowedBlock(const MatchState &St, size_t TB) const {
    if (St.Blks.empty())
      return true;
    for (auto &KV : St.Blks) {
      if (KV.second == TB)
        return true;
      if (is_contained(T->Succ[KV.second], TB) ||
          is_contained(T->Pred[KV.second], TB))
        return true;
    }
    return false;
  }

  bool insnOk(MatchState &St, const PInsn &PI, Instruction *TI) const {
    // dst consistency first: an already-bound slot forces the def site.
    if (PI.Dst != UINT32_MAX) {
      auto It = St.Vals.find(PI.Dst);
      if (It != St.Vals.end() && It->second != TI)
        return false;
    }
    auto opVal = [&](unsigned K) -> Value * {
      return TI->getOperand(K);
    };
    switch (PI.Kind) {
    case PInsnKind::Bin: {
      auto *BI = dyn_cast<BinaryOperator>(TI);
      if (!BI || BI->getOpcode() != PI.Opcode || !typeOk(PI.TypeBits, BI->getType()))
        return false;
      if (!bindDst(St, PI.Dst, BI))
        return false;
      MatchState S1 = St;
      if (unifyVal(S1, PI.Ops[0], opVal(0)) && unifyVal(S1, PI.Ops[1], opVal(1))) {
        St = S1;
        return true;
      }
      if (isCommutative(PI.Opcode)) {
        MatchState S2 = St;
        if (unifyVal(S2, PI.Ops[0], opVal(1)) && unifyVal(S2, PI.Ops[1], opVal(0))) {
          St = S2;
          return true;
        }
      }
      return false;
    }
    case PInsnKind::Cmp: {
      auto *CI = dyn_cast<CmpInst>(TI);
      if (!CI || !typeOk(PI.TypeBits, CI->getOperand(0)->getType()))
        return false;
      if (!bindDst(St, PI.Dst, CI))
        return false;
      if (CI->getPredicate() == PI.Opcode) {
        MatchState S1 = St;
        if (unifyVal(S1, PI.Ops[0], opVal(0)) && unifyVal(S1, PI.Ops[1], opVal(1))) {
          St = S1;
          return true;
        }
      }
      if (swapPred(PI.Opcode) == CI->getPredicate()) {
        MatchState S2 = St;
        if (unifyVal(S2, PI.Ops[0], opVal(1)) && unifyVal(S2, PI.Ops[1], opVal(0))) {
          St = S2;
          return true;
        }
      }
      return false;
    }
    case PInsnKind::Load: {
      auto *LI = dyn_cast<LoadInst>(TI);
      if (!LI || !typeOk(PI.TypeBits, LI->getType()))
        return false;
      return bindDst(St, PI.Dst, LI) && unifyVal(St, PI.Ops[0], LI->getPointerOperand());
    }
    case PInsnKind::Store: {
      auto *SI = dyn_cast<StoreInst>(TI);
      if (!SI || !typeOk(PI.TypeBits, SI->getValueOperand()->getType()))
        return false;
      return unifyVal(St, PI.Ops[0], SI->getValueOperand()) &&
             unifyVal(St, PI.Ops[1], SI->getPointerOperand());
    }
    case PInsnKind::GEP: {
      auto *GI = dyn_cast<GetElementPtrInst>(TI);
      if (!GI || GI->getNumIndices() != PI.Ops.size() - 1)
        return false;
      if (!bindDst(St, PI.Dst, GI) || !unifyVal(St, PI.Ops[0], GI->getPointerOperand()))
        return false;
      unsigned K = 0;
      for (Value *Idx : GI->indices()) {
        if (!unifyVal(St, PI.Ops[1 + K], Idx))
          return false;
        ++K;
      }
      return true;
    }
    case PInsnKind::Cast: {
      auto *CI = dyn_cast<CastInst>(TI);
      if (!CI || CI->getOpcode() != PI.Opcode || !typeOk(PI.TypeBits, CI->getDestTy()))
        return false;
      return bindDst(St, PI.Dst, CI) && unifyVal(St, PI.Ops[0], CI->getOperand(0));
    }
    case PInsnKind::Call: {
      auto *CI = dyn_cast<CallInst>(TI);
      if (!CI || !calleeOk(PI, CI))
        return false;
      if (PI.Dst != UINT32_MAX && !CI->getType()->isVoidTy()) {
        if (!bindDst(St, PI.Dst, CI))
          return false;
      }
      // Extra target args tolerated: prefix zip only.
      for (size_t K = 0; K < PI.Ops.size() && K < CI->arg_size(); ++K)
        if (!unifyVal(St, PI.Ops[K], CI->getArgOperand(K)))
          return false;
      return true;
    }
    case PInsnKind::Select: {
      auto *SI = dyn_cast<SelectInst>(TI);
      if (!SI)
        return false;
      return bindDst(St, PI.Dst, SI) &&
             unifyVal(St, PI.Ops[0], SI->getCondition()) &&
             unifyVal(St, PI.Ops[1], SI->getTrueValue()) &&
             unifyVal(St, PI.Ops[2], SI->getFalseValue());
    }
    case PInsnKind::PHI: {
      auto *PN = dyn_cast<PHINode>(TI);
      if (!PN)
        return false;
      if (!bindDst(St, PI.Dst, PN))
        return false;
      // Pattern incoming count need not equal target's; each pattern
      // incoming for an already-mapped block must unify with the target
      // incoming for the mapped block name.
      for (size_t K = 0; K < PI.Ops.size() && K < PN->getNumIncomingValues(); ++K)
        if (!unifyVal(St, PI.Ops[K], PN->getIncomingValue(K)))
          return false;
      return true;
    }
    }
    return false;
  }

  SmallVector<Loc, 64> candidates(const MatchState &St, const PInsn &PI) const {
    // Fast path: dst slot already bound -> its def insn.
    if (PI.Dst != UINT32_MAX) {
      auto It = St.Vals.find(PI.Dst);
      if (It != St.Vals.end()) {
        if (auto *I = dyn_cast<Instruction>(It->second)) {
          auto D = T->Def.find(I);
          if (D != T->Def.end() && !T->Def.empty()) {
            Loc L = D->second;
            if (!Excluded->contains(L))
              return {L};
            return {};
          }
        }
      }
    }
    auto kindOk = [&](Instruction *I) -> bool {
      switch (PI.Kind) {
      case PInsnKind::Bin:
        if (auto *B = dyn_cast<BinaryOperator>(I))
          return B->getOpcode() == PI.Opcode;
        return false;
      case PInsnKind::Cmp:
        return isa<CmpInst>(I);
      case PInsnKind::Load:
        return isa<LoadInst>(I);
      case PInsnKind::Store:
        return isa<StoreInst>(I);
      case PInsnKind::GEP:
        return isa<GetElementPtrInst>(I);
      case PInsnKind::Cast:
        if (auto *C = dyn_cast<CastInst>(I))
          return C->getOpcode() == PI.Opcode;
        return false;
      case PInsnKind::Call:
        if (auto *C = dyn_cast<CallInst>(I))
          return calleeOk(PI, C);
        return false;
      case PInsnKind::Select:
        return isa<SelectInst>(I);
      case PInsnKind::PHI:
        return isa<PHINode>(I);
      }
      return false;
    };
    SmallVector<Loc, 64> Out;
    for (size_t BI = 0; BI < T->Blocks.size(); ++BI) {
      if (!allowedBlock(St, BI))
        continue;
      size_t II = 0;
      for (Instruction &I : *T->Blocks[BI]) {
        if (isa<AllocaInst>(&I)) {
          ++II;
          continue;
        }
        Loc L{BI, II};
        if (!Excluded->contains(L) && !St.Used.contains(L) && kindOk(&I))
          Out.push_back(L);
        ++II;
      }
    }
    llvm::sort(Out);
    if (Out.size() > 64)
      Out.resize(64);
    return Out;
  }

  const PInsn &patInsn(size_t Flat) const {
    size_t N = Flat;
    for (auto &B : P->Blocks) {
      if (N < B.Insns.size())
        return B.Insns[N];
      N -= B.Insns.size();
    }
    llvm_unreachable("bad flat index");
  }
  std::pair<size_t, size_t> locate(size_t Flat) const {
    size_t N = Flat;
    for (size_t BI = 0; BI < P->Blocks.size(); ++BI) {
      if (N < P->Blocks[BI].Insns.size())
        return {BI, N};
      N -= P->Blocks[BI].Insns.size();
    }
    llvm_unreachable("bad flat index");
  }
  size_t patLen() const {
    size_t N = 0;
    for (auto &B : P->Blocks)
      N += B.Insns.size();
    return N;
  }

  // Next pattern insn: most-constrained first; deferred last.
  std::optional<size_t> nextInsn(const MatchState &St) const {
    std::optional<std::pair<unsigned, size_t>> Best, BestDef;
    for (size_t FI : FlatOrder) {
      if (St.Insns.count(FI) || St.Skipped.count(FI))
        continue;
      auto [PBI, PII] = locate(FI);
      const PInsn &PI = P->Blocks[PBI].Insns[PII];
      unsigned Bound = 0;
      for (auto &Op : PI.Ops)
        if (Op.Kind == PValKind::Slot && St.Vals.count(Op.Id))
          ++Bound;
      if (PI.Dst != UINT32_MAX && St.Vals.count(PI.Dst))
        ++Bound;
      unsigned Score = Bound * 4 + (is_contained(P->Required, FI) ? 2 : 0);
      if (St.Deferred.count(FI)) {
        if (!BestDef || Score > BestDef->first)
          BestDef = {{Score, FI}};
      } else if (!Best || Score > Best->first) {
        Best = {{Score, FI}};
      }
    }
    if (Best)
      return Best->second;
    if (BestDef)
      return BestDef->second;
    return std::nullopt;
  }

  void search(MatchState &St, std::optional<std::pair<size_t, MatchState>> &Best) {
    constexpr unsigned StepBudget = 6000;
    constexpr unsigned MaxBranch = 12;
    if (++Steps > StepBudget)
      return;
    bool ReqOk = true;
    for (size_t R : P->Required)
      if (!St.Insns.count(R)) {
        ReqOk = false;
        break;
      }
    if (ReqOk && (!Best || St.Insns.size() > Best->first))
      Best = {{St.Insns.size(), St}};
    auto Next = nextInsn(St);
    if (!Next)
      return;
    size_t FI = *Next;
    auto [PBI, PII] = locate(FI);
    const PInsn &PI = P->Blocks[PBI].Insns[PII];
    bool IsRequired = is_contained(P->Required, FI);
    SmallVector<std::tuple<unsigned, Loc, MatchState>, 16> Viable;
    for (Loc L : candidates(St, PI)) {
      Instruction *TI = T->insn(L);
      MatchState S2 = St;
      if (insnOk(S2, PI, TI)) {
        unsigned NewBinds =
            (unsigned)(S2.Vals.size() - St.Vals.size() + S2.Kws.size() - St.Kws.size());
        for (auto &KV : S2.Vals)
          if (!St.Vals.count(KV.first) && DefSlots.count(KV.first) &&
              !isa<Instruction>(KV.second))
            NewBinds += 20; // def-slot folded to a constant: legal, last resort
        Viable.push_back({NewBinds, L, S2});
      }
    }
    llvm::sort(Viable, [](auto &A, auto &B) {
      if (std::get<0>(A) != std::get<0>(B))
        return std::get<0>(A) < std::get<0>(B);
      return std::get<1>(A) < std::get<1>(B);
    });
    unsigned Branches = 0;
    for (auto &V : Viable) {
      if (Branches++ >= MaxBranch)
        break;
      MatchState S2 = std::get<2>(V);
      S2.Insns[FI] = std::get<1>(V);
      S2.Used.insert(std::get<1>(V));
      S2.Blks.try_emplace(PBI, std::get<1>(V).first);
      search(S2, Best);
      if (Steps > StepBudget)
        return;
    }
    // Optional insn that cannot match now: defer once, skip on retry.
    if (!IsRequired) {
      if (!St.Deferred.erase(FI))
        St.Deferred.insert(FI);
      else
        St.Skipped.insert(FI);
      search(St, Best);
    }
  }

  // Pattern-internal edges between two mapped blocks must exist between
  // their images; Cbr conditions unify softly (a mismatch does not kill
  // the match: the guard insn already matched as an anchor).
  bool termsOk(MatchState &St) const {
    for (size_t BI = 0; BI < P->Blocks.size(); ++BI) {
      auto It = St.Blks.find((uint32_t)BI);
      if (It == St.Blks.end())
        continue;
      size_t TB = It->second;
      const PBlock &PB = P->Blocks[BI];
      if (PB.TermKind == 1) {
        auto Q = St.Blks.find(PB.TermT);
        if (Q != St.Blks.end() && !is_contained(T->Succ[TB], Q->second))
          return false;
      } else if (PB.TermKind == 2) {
        if (Instruction *Term = T->Blocks[TB]->getTerminator())
          if (auto *BI2 = dyn_cast<BranchInst>(Term))
            if (BI2->isConditional())
              unifyVal(St, PB.TermVal, BI2->getCondition());
        for (uint32_t Q : {PB.TermT, PB.TermF}) {
          auto QI = St.Blks.find(Q);
          if (QI != St.Blks.end() && !is_contained(T->Succ[TB], QI->second))
            return false;
        }
      } else if (PB.TermKind == 4) {
        if (!isa<UnreachableInst>(T->Blocks[TB]->getTerminator()))
          return false;
      }
    }
    return true;
  }
};

struct RawMatch {
  const Pattern *Pat = nullptr;
  double Cov = 0;
  MatchState St;
};

// One match attempt; Excluded = target insns consumed by earlier
// occurrences of the same pattern.
static std::optional<RawMatch> matchOne(Function &F, const Pattern &P,
                                        TargetIndex &T,
                                        const DenseSet<Loc> &Excluded) {
  SearchCtx Ctx;
  Ctx.P = &P;
  Ctx.T = &T;
  Ctx.Excluded = &Excluded;
  for (size_t R : P.Required)
    Ctx.FlatOrder.push_back(R);
  size_t Total = 0;
  for (auto &B : P.Blocks)
    Total += B.Insns.size();
  for (size_t I = 0; I < Total; ++I)
    if (!is_contained(P.Required, I))
      Ctx.FlatOrder.push_back(I);
  for (auto &B : P.Blocks)
    for (auto &I : B.Insns)
      if (I.Dst != UINT32_MAX)
        Ctx.DefSlots.insert(I.Dst);
  MatchState St;
  std::optional<std::pair<size_t, MatchState>> Best;
  Ctx.search(St, Best);
  if (!Best)
    return std::nullopt;
  MatchState BS = Best->second;
  if (!Ctx.termsOk(BS))
    return std::nullopt;
  double Cov = Total ? (double)BS.Insns.size() / (double)Total : 0;
  if (Cov < P.MinCov)
    return std::nullopt;
  return RawMatch{&P, Cov, BS};
}

// ---- catalog ------------------------------------------------------------//

struct SnipDesc {
  const char *Name;   // e.g. "rust::vec_push"
  const char *Filter; // function name substring in the oracle module
  const char *Helper; // e.g. "vec_push" -> __pat_vec_push
  RequiredKind Req;
  double MinCov;
  const char *Alts[6];
};

// The catalog: 7 rust + 4 c + 3 cpp snippets, mirroring SNIPS in
// crates/patterns/src/snippets.rs. Rust yields O0 and O3 variants.
static const SnipDesc Snips[] = {
    {"rust::vec_push", "pat_vec_push", "vec_push", RequiredKind::CallsAndGuard,
     0.35, {"grow_one", "grow_amortized", "finish_grow", "_push", nullptr}},
    {"rust::vec_index", "pat_vec_index", "bounds_check", RequiredKind::Calls,
     0.3, {"panic_bounds_check", "vec_index", "slice_index", nullptr}},
    {"rust::slice_bounds", "pat_slice_index", "bounds_check",
     RequiredKind::Calls, 0.3, {"panic_bounds_check", "slice_index", "_index",
                                nullptr}},
    {"rust::option_unwrap", "pat_option_unwrap", "unwrap",
     RequiredKind::Calls, 0.3, {"unwrap_failed", "expect_failed", "unwrap",
                                nullptr}},
    {"rust::iter_sum", "pat_iter_sum", "iter_sum", RequiredKind::Calls, 0.3,
     {"sum", "fold", "iter_sum", nullptr, nullptr}},
    {"rust::string_from", "pat_string_from", "string_from",
     RequiredKind::Calls, 0.3, {"string_from", "to_owned", "from_str",
                                "to_string", nullptr}},
    {"rust::format", "pat_format", "format", RequiredKind::Calls, 0.3,
     {"format", "fmt__arguments", "fmt__new", "write_fmt", nullptr}},
    {"c::strlen", "pat_strlen", "strlen", RequiredKind::Calls, 0.4,
     {"strlen", "strnlen", nullptr}},
    {"c::memset", "pat_memset", "memset", RequiredKind::Calls, 0.4,
     {"memset", nullptr}},
    {"c::memcpy", "pat_memcpy", "memcpy", RequiredKind::Calls, 0.4,
     {"memcpy", "memmove", nullptr}},
    {"c::memcmp", "pat_memcmp", "memcmp", RequiredKind::Calls, 0.4,
     {"memcmp", "strcmp", nullptr}},
    {"cpp::vector_push_back", "pat_vpush", "vector_push_back",
     RequiredKind::CallsAndGuard, 0.35, {"emplace_reallocate", "change_array",
                                         "xlength", "push_back", nullptr}},
    {"cpp::string_move", "pat_smove", "string_move", RequiredKind::Calls,
     0.35, {"memcpy", "basic_string", "move", nullptr}},
    {"cpp::string_append", "pat_sappend", "string_append",
     RequiredKind::Calls, 0.35, {"append", "strlen", "basic_string", nullptr}},
};

// Hand-built structural patterns for the shapes every libc family shares.
// These need no oracle: memcpy/memset appear as llvm intrinsics even at
// O0, strlen/memcmp as direct calls. Oracle-generated variants (with
// shape context around the anchor) complement these when a catalog loads.
static void addHandbuilt(SmallVectorImpl<Pattern> &Out) {
  auto callPat = [&](const char *Name, const char *Helper,
                     const char *Needle, bool Intrinsic) {
    Pattern P;
    P.Name = Name;
    P.Helper = Helper;
    P.MinCov = 0.4;
    PBlock B;
    B.Id = 0;
    PInsn C;
    C.Kind = PInsnKind::Call;
    C.Dst = UINT32_MAX;
    C.IsIntrinsicCall = Intrinsic;
    C.CalleeNeedle = Needle;
    B.Insns.push_back(C);
    B.TermKind = 0;
    P.Blocks.push_back(B);
    P.Required.push_back(0);
    Out.push_back(P);
  };
  callPat("c::strlen", "strlen", "strlen", false);
  callPat("c::memset", "memset", "llvm.memset", true);
  callPat("c::memcpy", "memcpy", "llvm.memcpy", true);
  callPat("c::memcmp", "memcmp", "memcmp", false);
}

// Load one oracle module and canonicalize every snippet function found in
// it. Suffix disambiguates per-opt-level variants of one snippet.
static void catalogFromModule(Module &M, StringRef Suffix,
                              SmallVectorImpl<Pattern> &Out) {
  for (const SnipDesc &S : Snips) {
    Function *Pick = nullptr;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      if (F.getName().contains(StringRef(S.Filter))) {
        Pick = &F;
        break;
      }
    }
    if (!Pick) {
      // Match the filter against demangled names too (rustc v0 mangling).
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        if (StringRef(normName(F.getName())).contains(S.Filter)) {
          Pick = &F;
          break;
        }
      }
    }
    if (!Pick)
      continue;
    SmallVector<std::string, 4> Alts;
    for (const char *A : S.Alts) {
      if (!A)
        break;
      Alts.push_back(A);
    }
    Pattern P;
    std::string Name = S.Name + Suffix.str();
    if (canonicalizeFunction(*Pick, Name, S.Helper, S.Req, S.MinCov, Alts, P))
      Out.push_back(std::move(P));
  }
}

static void loadCatalog(SmallVectorImpl<Pattern> &Out) {
  addHandbuilt(Out);
  // Plugin DLLs cannot always register cl::opt flags with the host opt
  // (option parsing happens before the plugin loads), so the catalog
  // path also reads from the environment. The flag wins when set;
  // revenant-opt (static link) parses flags normally.
  std::string Catalog = PatternsCatalog;
  if (Catalog.empty()) {
    if (const char *E = getenv("REVENANT_PATTERNS_CATALOG"))
      Catalog = E;
  }
  double MinConf = PatternsMinConf;
  if (const char *E = getenv("REVENANT_PATTERNS_MIN_CONF"))
    MinConf = strtod(E, nullptr);
  (void)MinConf;
  if (Catalog.empty())
    return;
  SMDiagnostic Err;
  SmallVector<std::string, 8> Files;
  if (sys::fs::is_directory(Catalog)) {
    std::error_code EC;
    for (sys::fs::directory_iterator It(Catalog, EC), End;
         It != End && !EC; It.increment(EC)) {
      StringRef P = It->path();
      if (P.ends_with(".ll"))
        Files.push_back(P.str());
    }
    llvm::sort(Files);
  } else {
    Files.push_back(Catalog);
  }
  LLVMContext Ctx;
  for (std::string &Path : Files) {
    auto Mod = parseIRFile(Path, Err, Ctx);
    if (!Mod) {
      Err.print("revenant-patterns", errs());
      continue;
    }
    // Suffix from the file stem (|O3 / |O2), like emit_pattern's origin.
    StringRef Stem = sys::path::stem(StringRef(Path));
    std::string Suffix;
    if (Stem.contains("O3") || Stem.contains("O2"))
      Suffix = Stem.contains("O3") ? "@O3" : "@O2";
    else if (Stem.contains("O0"))
      Suffix = "@O0";
    catalogFromModule(*Mod, Suffix, Out);
  }
}

// ---- rewrite (port of apply.rs) -----------------------------------------//

static MDNode *vaOf(Instruction &I) {
  return I.getMetadata("revenant.va");
}

// Copy !revenant.va provenance (and the debug location) onto a new
// instruction. Manual until revenant::ir::copyInstructionVA lands.
static void copyVA(Instruction *Dst, MDNode *VA, DebugLoc DL) {
  if (VA)
    Dst->setMetadata("revenant.va", VA);
  if (DL)
    Dst->setDebugLoc(DL);
  // !revenant.opaque is never dropped; replacement calls carry none, so
  // there is nothing to forward here. Kept as a named step so the future
  // contract helper has one call site to replace.
}

struct Candidate {
  const Pattern *Pat = nullptr;
  const RawMatch *M = nullptr;
  double Cov = 0;
};

// Single-block contiguous region above threshold becomes one
// `__pat_<helper>(args)` call. Args are the bound free slots (slots never
// defined by a matched insn) in slot order; live-outs are matched dsts
// still read outside the region (terminator uses count as outside).
static bool rewriteMatch(Function &F, TargetIndex &T, const Pattern &P,
                         const MatchState &St, double MinConf) {
  if (St.Insns.size() == 0 || P.Blocks.empty())
    return false;
  // All matched locs in one block?
  size_t BI = St.Insns.begin()->second.first;
  SmallVector<size_t, 8> Idxs;
  for (auto &KV : St.Insns) {
    if (KV.second.first != BI)
      return false; // multi-block: annotate only
    Idxs.push_back(KV.second.second);
  }
  llvm::sort(Idxs);
  Idxs.erase(std::unique(Idxs.begin(), Idxs.end()), Idxs.end());
  size_t Count = T.insnCount(BI);
  if (Idxs.front() + Idxs.size() != Idxs.back() + 1)
    return false; // straddles unmatched insns: leave untouched
  if (Idxs.back() >= Count)
    return false;

  // Slot defs: pattern slots defined by a matched insn.
  DenseSet<uint32_t> DefinedByMatch;
  for (auto &KV : St.Insns) {
    size_t FI = KV.first;
    size_t N = FI;
    for (auto &B : P.Blocks) {
      if (N < B.Insns.size()) {
        if (B.Insns[N].Dst != UINT32_MAX)
          DefinedByMatch.insert(B.Insns[N].Dst);
        break;
      }
      N -= B.Insns.size();
    }
  }
  // Args: bound slots never defined by a matched insn, slot order. Any
  // value kind works (arguments, constants, globals); only the helper
  // call's own result (bound to a region insn) is excluded by the def
  // set. Constants bound through Kw slots are not Values in St.Vals;
  // recover them from the matched anchor call operands below.
  SmallVector<std::pair<uint32_t, Value *>, 8> Args;
  for (auto &KV : St.Vals)
    if (!DefinedByMatch.count(KV.first))
      Args.push_back({KV.first, KV.second});
  // Anchor call operands first: the pattern's required call sites carry
  // the routine arguments (dst/src/len). Sort bound slots by first use
  // in a required call so the helper signature reads naturally.
  DenseMap<Value *, unsigned> AnchorPos;
  for (size_t R : P.Required) {
    auto It = St.Insns.find(R);
    if (It == St.Insns.end())
      continue;
    size_t N = R;
    for (auto &B : P.Blocks) {
      if (N >= B.Insns.size()) {
        N -= B.Insns.size();
        continue;
      }
      if (B.Insns[N].Kind == PInsnKind::Call) {
        if (Instruction *TI = T.insn(It->second))
          if (auto *CI = dyn_cast<CallInst>(TI)) {
            for (Value *A : CI->args())
              if (!AnchorPos.count(A))
                AnchorPos[A] = (unsigned)AnchorPos.size();
          }
      }
      break;
    }
  }
  llvm::sort(Args, [&](auto &A, auto &B) {
    unsigned PA = AnchorPos.lookup(A.second), PB = AnchorPos.lookup(B.second);
    bool HA = AnchorPos.count(A.second), HB = AnchorPos.count(B.second);
    if (HA != HB)
      return HA > HB;
    if (HA && PA != PB)
      return PA < PB;
    return A.first < B.first;
  });

  // Live-outs: matched dst values read outside the region.
  DenseSet<Instruction *> Region;
  for (auto &KV : St.Insns)
    Region.insert(T.insn(KV.second));
  SmallVector<Instruction *, 2> LiveOuts;
  for (auto &KV : St.Insns) {
    Instruction *I = T.insn(KV.second);
    if (I->getType()->isVoidTy())
      continue;
    bool Outside = false;
    for (User *U : I->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (!UI || !Region.contains(UI)) {
        Outside = true;
        break;
      }
    }
    // Terminator uses count as outside (the region never has terminators).
    auto UIT = T.Uses.find(I);
    if (UIT != T.Uses.end())
      for (Loc L : UIT->second)
        if (L.second == SIZE_MAX)
          Outside = true;
    if (Outside)
      LiveOuts.push_back(I);
  }
  if (LiveOuts.size() > 1)
    return false;
  // Safety: every non-live region insn must be dead outside the helper
  // call's future operands. Args are values the helper keeps reading, so
  // uses by a region insn that itself feeds Args are fine; anything else
  // reading a dead region insn would dangle after the erase.
  DenseSet<Value *> Keep;
  for (auto &A : Args)
    Keep.insert(A.second);
  for (auto &KV : St.Insns) {
    Instruction *I = T.insn(KV.second);
    if (!LiveOuts.empty() && I == LiveOuts[0])
      continue;
    for (User *U : I->users()) {
      auto *UI = dyn_cast<Instruction>(U);
      if (UI && Region.contains(UI))
        continue;
      if (Keep.contains(I))
        continue; // helper call keeps reading it
      return false;
    }
  }
  // Collect region pointers BEFORE inserting the helper call: insertion
  // shifts block indices, and T.insn() walks by position.
  SmallVector<Instruction *, 8> Dead;
  for (size_t II : Idxs)
    Dead.push_back(T.insn({BI, II}));
  DenseSet<Instruction *> DeadSet(Dead.begin(), Dead.end());
  // Live-out must be a region insn; anything else is a stale index bug.
  Instruction *Live = LiveOuts.empty() ? nullptr : LiveOuts[0];
  if (Live && !DeadSet.contains(Live))
    return false;
  BasicBlock *BB = T.Blocks[BI];
  Instruction *First = Dead.front();
  MDNode *VA = vaOf(*First);
  DebugLoc DL = First->getDebugLoc();
  LLVMContext &Ctx = F.getContext();
  Module *M = F.getParent();

  // Build the helper call type: void, or the live-out's type.
  Type *RetTy = Type::getVoidTy(Ctx);
  if (!LiveOuts.empty())
    RetTy = LiveOuts[0]->getType();
  SmallVector<Type *, 8> ArgTys;
  SmallVector<Value *, 8> ArgVals;
  for (auto &A : Args) {
    ArgTys.push_back(A.second->getType());
    ArgVals.push_back(A.second);
  }
  if (ArgVals.empty()) {
    // Anchor-only pattern (no operand slots to bind): the helper takes
    // the matched required call's arguments, so __pat_strlen(s) keeps s.
    for (size_t R : P.Required) {
      auto It = St.Insns.find(R);
      if (It == St.Insns.end())
        continue;
      size_t N = R;
      for (auto &B : P.Blocks) {
        if (N >= B.Insns.size()) {
          N -= B.Insns.size();
          continue;
        }
        if (B.Insns[N].Kind == PInsnKind::Call) {
          if (Instruction *TI = T.insn(It->second))
            if (auto *CI = dyn_cast<CallInst>(TI)) {
              Function *CF = CI->getCalledFunction();
              unsigned NArgs = CI->arg_size();
              // llvm.memcpy/memset carry a trailing i1 isVolatile flag
              // that is not a routine operand: drop it so the helper
              // reads (dst, src, len) / (dst, val, len).
              if (CF && CF->getName().starts_with("llvm.mem") && NArgs > 0)
                --NArgs;
              for (unsigned K = 0; K < NArgs; ++K) {
                Value *A = CI->getArgOperand(K);
                // One entry per value: two anchors sharing an operand
                // must not duplicate the argument.
                bool Seen = false;
                for (Value *E : ArgVals)
                  if (E == A) {
                    Seen = true;
                    break;
                  }
                if (Seen)
                  continue;
                ArgTys.push_back(A->getType());
                ArgVals.push_back(A);
              }
            }
        }
        break;
      }
    }
  }
  FunctionType *FTy = FunctionType::get(RetTy, ArgTys, false);
  std::string HName = "__pat_" + P.Helper;
  FunctionCallee HC = M->getOrInsertFunction(HName, FTy);
  IRBuilder<> B(First);
  CallInst *Call = B.CreateCall(HC, ArgVals);
  copyVA(Call, VA, DL);
  if (Live) {
    // The single live-out name becomes the call destination: RAUW keeps
    // every SSA use consistent.
    Live->replaceAllUsesWith(Call);
    Call->setName(Live->getName());
  }
  // Erase the region: with a live-out, RAUW first (every outside use now
  // reads the helper call), then erase every region insn including the
  // old def site. Without one, just erase. The use check happens before
  // RAUW above, so no mid-rewrite bailout can leave a half-written block.
  for (auto *I : reverse(Dead))
    if (I != Live)
      I->eraseFromParent();
  if (Live)
    Live->eraseFromParent();
  (void)MinConf;
  return true;
}

} // namespace

PreservedAnalyses PatternsPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  SmallVector<Pattern, 16> Catalog;
  loadCatalog(Catalog);
  if (Catalog.empty())
    return PreservedAnalyses::all();

  TargetIndex T(F);
  // Per-pattern occurrences (earlier hits exclude their insns, like
  // match_function's MAX_OCCURRENCES loop).
  struct Hit {
    const Pattern *Pat;
    RawMatch M;
  };
  SmallVector<Hit, 16> Hits;
  for (const Pattern &P : Catalog) {
    DenseSet<Loc> Excluded;
    for (unsigned Occ = 0; Occ < 16; ++Occ) {
      auto One = matchOne(F, P, T, Excluded);
      if (!One)
        break;
      for (auto &KV : One->St.Insns)
        Excluded.insert(KV.second);
      Hits.push_back({&P, std::move(*One)});
    }
  }
  if (Hits.empty())
    return PreservedAnalyses::all();
  // Highest confidence first; regions of selected matches never overlap.
  llvm::sort(Hits, [](const Hit &A, const Hit &B) {
    return A.M.Cov > B.M.Cov;
  });
  DenseSet<Loc> Claimed;
  bool Changed = false;
  TargetIndex *TP = &T;
  for (auto &H : Hits) {
    if (H.M.Cov < PatternsMinConf)
      continue;
    bool Overlap = false;
    for (auto &KV : H.M.St.Insns)
      if (Claimed.contains(KV.second)) {
        Overlap = true;
        break;
      }
    if (Overlap)
      continue;
    // Rebuild the index when a rewrite changed block contents.
    if (Changed) {
      T = TargetIndex(F);
      TP = &T;
      // Re-verify claimed locs still exist: a changed function shifts
      // indices, so stop after the first rewrite per function. Later
      // matches re-run on the next pass invocation (fixed point lives in
      // the driver, like apply_matches' plan-then-rebuild split).
      break;
    }
    if (rewriteMatch(F, *TP, *H.Pat, H.M.St, PatternsMinConf)) {
      for (auto &KV : H.M.St.Insns)
        Claimed.insert(KV.second);
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-patterns` name in Pipeline/Register.cpp, so the entry point is
// compiled only for the standalone plugin DLL.
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Compiler.h"

#ifndef LLVM_REVENANT_PATTERNS_LINK_INTO_TOOLS
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
    llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantPatterns", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-patterns") {
                    PM.addPass(revenant::PatternsPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif // LLVM_REVENANT_PATTERNS_LINK_INTO_TOOLS
#endif // REVENANT_BUILD_PLUGIN
