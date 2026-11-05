//===- PairSplit.cpp - scalar i128 copy-web splitting ---------------------===//
//
// Part of the Revenant LLVM fork. See PairSplit.h.
//
// v1 scope: stores of i128 whose value resolves to a private web of
// non-zero constants, simple non-volatile loads, and phis of those, where
// every web def is used only inside the web or by the store. Each web
// splits into i64 halves: constants split by shift/trunc, loads become two
// i64 loads at +0/+8, phis become two i64 phis with per-arm halves, and the
// store becomes two i64 stores at +0/+8. Little-endian layout keeps the low
// half at offset 0. Halved accesses use the common alignment of the
// original alignment and the lane offset (never above the i64 ABI
// alignment), so an align-16 pair at +8 becomes align 8, not 16.
//
// Zero constants are skipped: `store i128 0` already emits cleanly, and
// splitting it would double the stores for no gain. Webs shared with other
// stores are skipped (no cross-store coordination in v1): a def with any
// use outside its own web + store pins the web.
//
// New values use direct Create/new calls into their block (no IRBuilder).
//
//===----------------------------------------------------------------------===//

#include "PairSplit.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include <numeric>

using namespace llvm;

namespace revenant {
namespace pairsplit {
namespace {

constexpr char VA_MD[] = "revenant.va";
constexpr char REGION_MD[] = "revenant.region";
constexpr char OPAQUE_MD[] = "revenant.opaque";

void inheritMeta(const Instruction *Src, Instruction *Dst) {
  if (MDNode *N = Src->getMetadata(VA_MD))
    Dst->setMetadata(VA_MD, N);
  if (MDNode *N = Src->getMetadata(REGION_MD))
    Dst->setMetadata(REGION_MD, N);
  if (MDNode *N = Src->getMetadata(OPAQUE_MD))
    Dst->setMetadata(OPAQUE_MD, N);
  Dst->setDebugLoc(Src->getDebugLoc());
}

static Align halfAlign(MaybeAlign A, uint64_t Off, unsigned ABI) {
  uint64_t Known = A ? A->value() : 1;
  uint64_t Common = std::gcd(Known, Off);
  unsigned Clamped = (unsigned)std::min<uint64_t>(Common, ABI);
  if (Clamped == 0)
    Clamped = 1;
  return Align(Clamped);
}

// An i128 value splittable without new computation: constant (halves are
// constants), simple load (halves are new loads), phi (halves are new phis
// once every arm resolves). Anything else (arguments, arithmetic, calls)
// is not splittable.
bool splittableDef(Value *V) {
  if (isa<ConstantInt>(V))
    return true;
  if (auto *LI = dyn_cast<LoadInst>(V))
    return LI->getType()->isIntegerTy(128) && LI->isSimple() &&
           !LI->isVolatile();
  if (auto *P = dyn_cast<PHINode>(V))
    return P->getType()->isIntegerTy(128);
  return false;
}

// Collect the web behind a stored i128 value: the value itself plus,
// transitively, phi arms. Returns false on any non-splittable def or phi
bool collectWeb(Value *V, DenseSet<Value *> &Web, DenseSet<Value *> &EnRoute) {
  if (isa<ConstantInt>(V))
    return true; // constants need no defs
  auto *I = dyn_cast<Instruction>(V);
  if (!I || !splittableDef(V))
    return false;
  if (Web.contains(V))
    return true;
  auto *P = dyn_cast<PHINode>(V);
  if (!P)
    return Web.insert(V).second;
  if (!EnRoute.insert(V).second)
    return false; // phi cycle
  for (Value *Arm : P->incoming_values())
    if (!collectWeb(Arm, Web, EnRoute))
      return false;
  EnRoute.erase(V);
  Web.insert(V);
  return true;
}

// Every web def must be used only inside the web (as a phi arm) or by the
// store being split. Shared webs stay untouched in v1.
bool privateUses(const DenseSet<Value *> &Web, StoreInst *SI) {
  for (Value *D : Web) {
    auto *I = cast<Instruction>(D);
    for (const Use &U : I->uses()) {
      Value *User = U.getUser();
      if (User == SI)
        continue;
      if (Web.contains(User) && isa<PHINode>(User))
        continue;
      return false;
    }
  }
  return true;
}

struct Halves {
  Value *Lo = nullptr;
  Value *Hi = nullptr;
};

bool tryStore(Function &F, StoreInst *SI, unsigned &Changed) {
  if (!SI->getValueOperand()->getType()->isIntegerTy(128))
    return false;
  if (SI->isVolatile() || !SI->isSimple())
    return false;
  if (SI->getPointerAddressSpace() != 0)
    return false;
  Value *V = SI->getValueOperand();
  DenseSet<Value *> Web;
  DenseSet<Value *> EnRoute;
  if (!collectWeb(V, Web, EnRoute))
    return false;
  if (!privateUses(Web, SI))
    return false;
  // Split only webs containing a constant the C emitter cannot materialize
  // (needs more than 64 bits: the u64-literal path fails, e.g. folded
  // pointer pairs). Load-only webs and small constants emit cleanly as one
  // struct assignment, which reads better and stays recognizable for loop
  // folding; splitting them only lengthens the source.
  bool Huge = false;
  auto HugeConst = [&](Value *X) {
    if (auto *C = dyn_cast<ConstantInt>(X))
      if (C->getValue().getActiveBits() > 64)
        Huge = true;
  };
  HugeConst(V);
  for (Value *D : Web)
    if (auto *P = dyn_cast<PHINode>(D))
      for (Value *Arm : P->incoming_values())
        HugeConst(Arm);
  if (!Huge)
    return false;

  LLVMContext &Ctx = F.getContext();
  Type *I8 = Type::getInt8Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  unsigned ABI = F.getParent()->getDataLayout().getABITypeAlign(I64).value();
  DenseMap<Value *, Halves> Pairs;

  // Halves of a constant: low/high 64 bits, no new instructions.
  auto constHalves = [&](ConstantInt *C) {
    APInt Val = C->getValue();
    return Halves{ConstantInt::get(I64, Val.trunc(64)),
                  ConstantInt::get(I64, Val.lshr(64).trunc(64))};
  };

  // Split loads first (phis may reference them as arms).
  for (Value *D : Web) {
    auto *LI = dyn_cast<LoadInst>(D);
    if (!LI)
      continue;
    auto *Off0 = ConstantInt::get(I64, 0);
    auto *G0 = GetElementPtrInst::Create(I8, LI->getPointerOperand(), Off0,
                                         LI->getName() + ".p.lo",
                                         LI->getIterator());
    inheritMeta(LI, G0);
    auto *L0 = new LoadInst(I64, G0, LI->getName() + ".lo", false,
                            LI->getIterator());
    inheritMeta(LI, L0);
    L0->setAlignment(halfAlign(LI->getAlign(), 0, ABI));
    auto *Off8 = ConstantInt::get(I64, 8);
    auto *G8 = GetElementPtrInst::Create(I8, LI->getPointerOperand(), Off8,
                                         LI->getName() + ".p.hi",
                                         LI->getIterator());
    inheritMeta(LI, G8);
    auto *L8 = new LoadInst(I64, G8, LI->getName() + ".hi", false,
                            LI->getIterator());
    inheritMeta(LI, L8);
    L8->setAlignment(halfAlign(LI->getAlign(), 8, ABI));
    Pairs[D] = Halves{L0, L8};
    Changed += 4;
  }

  // Split phis to a fixpoint (arms may be other web phis; cycles bailed in
  // collectWeb, so some phi is always ready until all are done).
  SmallVector<PHINode *, 4> Todo;
  for (Value *D : Web)
    if (auto *P = dyn_cast<PHINode>(D))
      Todo.push_back(P);
  bool Progress = true;
  while (!Todo.empty() && Progress) {
    Progress = false;
    for (auto It = Todo.begin(); It != Todo.end();) {
      PHINode *P = *It;
      SmallVector<std::pair<BasicBlock *, Halves>, 4> Arms;
      bool Ready = true;
      for (unsigned i = 0, N = P->getNumIncomingValues(); i < N; ++i) {
        Value *Arm = P->getIncomingValue(i);
        Halves H;
        if (auto *C = dyn_cast<ConstantInt>(Arm))
          H = constHalves(C);
        else {
          auto Hit = Pairs.find(Arm);
          if (Hit == Pairs.end()) {
            Ready = false;
            break;
          }
          H = Hit->second;
        }
        Arms.push_back({P->getIncomingBlock(i), H});
      }
      if (!Ready) {
        ++It;
        continue;
      }
      auto *Lo = PHINode::Create(I64, Arms.size(), P->getName() + ".lo",
                                 P->getIterator());
      auto *Hi = PHINode::Create(I64, Arms.size(), P->getName() + ".hi",
                                 P->getIterator());
      inheritMeta(P, Lo);
      inheritMeta(P, Hi);
      for (auto &A : Arms) {
        Lo->addIncoming(A.second.Lo, A.first);
        Hi->addIncoming(A.second.Hi, A.first);
      }
      Pairs[P] = Halves{Lo, Hi};
      Changed += 2;
      It = Todo.erase(It);
      Progress = true;
    }
  }
  if (!Todo.empty())
    return false; // unreachable: collectWeb rejects cycles

  Halves Root;
  if (auto *C = dyn_cast<ConstantInt>(V))
    Root = constHalves(C);
  else {
    auto Hit = Pairs.find(V);
    if (Hit == Pairs.end())
      return false;
    Root = Hit->second;
  }

  // Replace the store with two i64 stores at +0/+8.
  auto *SOff0 = ConstantInt::get(I64, 0);
  auto *SG0 = GetElementPtrInst::Create(I8, SI->getPointerOperand(), SOff0,
                                        SI->getName() + ".p.lo",
                                        SI->getIterator());
  inheritMeta(SI, SG0);
  auto *S0 = new StoreInst(Root.Lo, SG0, false,
                           halfAlign(SI->getAlign(), 0, ABI),
                           SI->getIterator());
  inheritMeta(SI, S0);
  auto *SOff8 = ConstantInt::get(I64, 8);
  auto *SG8 = GetElementPtrInst::Create(I8, SI->getPointerOperand(), SOff8,
                                        SI->getName() + ".p.hi",
                                        SI->getIterator());
  inheritMeta(SI, SG8);
  auto *S8 = new StoreInst(Root.Hi, SG8, false,
                           halfAlign(SI->getAlign(), 8, ABI),
                           SI->getIterator());
  inheritMeta(SI, S8);
  Changed += 4;
  SI->eraseFromParent();

  // Erase dead web defs in reverse use order (phis before their loads).
  // Every def was private to the web + store, so all uses are gone. A def
  // leaves the list when it is erased: the next sweep must not touch it.
  SmallVector<Instruction *, 8> Defs;
  for (Value *D : Web)
    Defs.push_back(cast<Instruction>(D));
  bool Swept = true;
  while (Swept) {
    Swept = false;
    for (auto It = Defs.begin(); It != Defs.end();) {
      if (!(*It)->use_empty()) {
        ++It;
        continue;
      }
      (*It)->eraseFromParent();
      It = Defs.erase(It);
      Swept = true;
    }
  }
  return true;
}

} // namespace

unsigned splitPairs(Function &F) {
  unsigned Changed = 0;
  SmallVector<StoreInst *, 8> Stores;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *SI = dyn_cast<StoreInst>(&I))
        if (SI->getValueOperand()->getType()->isIntegerTy(128))
          Stores.push_back(SI);
  for (StoreInst *SI : Stores) {
    if (SI->getParent() == nullptr)
      continue;
    tryStore(F, SI, Changed);
  }
  return Changed;
}

} // namespace pairsplit
} // namespace revenant
