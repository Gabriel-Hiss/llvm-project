//===- Unsroa.cpp - rebuild aggregates SROA/instcombine split -----------===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Inverts the two halves of aggregate scalarization the corpus shows:
//
//   SROA splits an aggregate alloca into one scalar alloca per field, with
//   the field index carried by the alloca name (`p.sroa.0`, `a.sroa.3`).
//   instcombine then flattens typed field GEPs
//   (`gep %struct.Point, ptr %p, i32 0, i32 1`) into byte GEPs
//   (`gep i8, ptr %p, i64 4`). Full -O2 on c-clang-o2 `struct_access`
//   shows the end state: no aggregate alloca survives for the split object,
//   every access is a byte GEP plus a scalar load/store.
//
// This pass rebuilds three shapes, each checked against the DataLayout:
//
//   A. Scalar alloca groups -> aggregate alloca. Alloc parented in the entry
//      block whose names share a `<base>.sroa.<n>` stem fold into one
//      `[N x i8]`-sized alloca named `<base>`; every load/store of a member
//      becomes a byte-GEP into the aggregate. Alive2 sketch: the members
//      are disjoint slots of one dead object; joining them into one alloca
//      of the union extent and addressing each slot by its byte offset is
//      a refinement because no member is address-taken (checked) and the
//      offsets are disjoint (checked).
//   B. Scalar copy runs -> memcpy. Adjacent scalar load/store pairs
//      moving the same widths between two bases fold into one
//      llvm.memcpy, but only when the run is explainable as a source
//      struct copy: either it covers a whole known object (alloca/global
//      of matching size), a field-aligned window of a struct-typed base,
//      or -- for opaque roots -- a byte window whose size and internal
//      boundaries match a module struct type. Byte-dispatch copy code
//      (CRT memcpy expansions) has no such evidence and is left scalar.
//   C. Byte GEPs -> typed struct GEPs. A byte GEP whose base carries a
//      known struct type in the module and whose offset lands exactly on
//      a field start becomes the typed field GEP the pre-IR had. This is
//      the c-clang-o2 `struct_access` rewrite: `gep i8, ptr %0, i64 4`
//      with `%struct.Point = type { i32, i32, ptr }` in the module
//      becomes `gep %struct.Point, ptr %0, i32 0, i32 1`.
//   D. Split aggregate values back to aggregate stores (extractvalue-fed
//      scalar store webs covering one struct).
//   E. Field-access clusters -> synthesized struct types. Lifted modules
//      have no struct types, so C never fires there; E rebuilds the
//      shape instead. Two or more distinct constant-offset scalar
//      accesses through one pointer root (an argument, an inttoptr, a
//      PHI, or a byte-array alloca window) with non-overlapping
//      footprints become a `struct.agg.*` type and struct GEPs, matching
//      TypeRecovery's "two offsets make a struct" rule. Uniform-stride
//      runs read as arrays; adjacent same-type pairs and copy-routine
//      roots (several inttoptrs of one value) are vetoed.
// Sharing with TypeRecovery: the grouping rule (constant byte offsets
// through one base, two or more distinct offsets) matches the FieldAccess
// rule in TypeRecovery/TypeInference.cpp, so a group this pass rebuilds is
// a struct TypeRecovery would also infer. This pass only uses the module
// DataLayout plus in-function GEP footprints; it never reads DI metadata.
//
// Safety rails: volatile/atomic accesses veto their group; address-taken
// members veto rebuild A; overlapping or out-of-order copy pairs veto B;
// a byte offset that does not land on a field boundary vetoes C for that
// GEP. !revenant.va is copied from the replaced instruction to every new
// one; !revenant.opaque calls and !revenant.region terminators are never
// touched.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Unsroa/Unsroa.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Casting.h"

#include <cstdlib>
#include <optional>

using namespace llvm;

namespace revenant {
namespace {

// Copy !revenant.va from Src to Dst when present.
void copyVA(const Instruction *Src, Instruction *Dst) {
  if (!Src || !Dst)
    return;
  if (MDNode *VA = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", VA);
}

bool isOpaqueCall(const Instruction &I) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB || !CB->getCalledFunction())
    return false;
  return CB->getMetadata("revenant.opaque") != nullptr;
}

// Strip pointer-preserving copies to find the address root.
const Value *skipCopies(const Value *V) {
  const Value *Cur = V;
  SmallPtrSet<const Value *, 8> Seen;
  while (Cur && Seen.insert(Cur).second) {
    const Value *Next = nullptr;
    if (const auto *BC = dyn_cast<BitCastInst>(Cur))
      Next = BC->getOperand(0);
    else if (const auto *GEP = dyn_cast<GetElementPtrInst>(Cur)) {
      if (GEP->hasAllZeroIndices())
        Next = GEP->getPointerOperand();
      else
        break;
    } else {
      break;
    }
    if (!Next || Next == Cur)
      break;
    if (Next->getType()->isPointerTy() != Cur->getType()->isPointerTy())
      break;
    Cur = Next;
  }
  return Cur;
}

// Resolve Ptr to (ultimate base, total byte offset) through constant
// byte-GEP layers. Returns nullptr on veto (non-byte step or a
// variable-indexed base underneath: the address walks). Unlike the fold
// path's peel, multi-use links are fine: footprints only observe.
const Value *resolveByteBase(const Value *Ptr, const DataLayout &DL,
                             LLVMContext &Ctx, uint64_t &Total) {
  Total = 0;
  const Value *Cur = Ptr;
  while (const auto *GEP = dyn_cast<GetElementPtrInst>(Cur)) {
    if (GEP->getSourceElementType() != Type::getInt8Ty(Ctx) ||
        !GEP->hasAllConstantIndices())
      return nullptr;
    APInt Off(DL.getIndexSizeInBits(0), 0);
    if (!GEP->accumulateConstantOffset(DL, Off))
      return nullptr;
    Total += Off.getZExtValue();
    Cur = GEP->getPointerOperand();
  }
  return Cur;
}

// Resolve Ptr to (ultimate base, total byte offset) through ANY constant-
// index GEP layers (byte or typed-element). Returns nullptr when a GEP
// index varies: the address walks, so a fixed window means nothing.
// Unlike resolveByteBase this accepts element GEPs because rebuilder E
// reads footprints from the typed `element` GEPs LiftNormalize emits.
const Value *resolveFootprintBase(const Value *Ptr, const DataLayout &DL,
                                  uint64_t &Total) {
  Total = 0;
  const Value *Cur = Ptr;
  SmallPtrSet<const Value *, 8> Seen;
  while (const auto *GEP = dyn_cast<GetElementPtrInst>(Cur)) {
    if (!Seen.insert(GEP).second || !GEP->hasAllConstantIndices())
      return nullptr;
    APInt Off(DL.getIndexSizeInBits(0), 0);
    if (!GEP->accumulateConstantOffset(DL, Off) || Off.isNegative() ||
        Off.getActiveBits() > 64)
      return nullptr;
    uint64_t Step = Off.getZExtValue();
    if (Step > UINT64_MAX - Total)
      return nullptr;
    Total += Step;
    Cur = GEP->getPointerOperand();
  }
  return Cur;

}

// Peel a root down to the value that produced its address: inttoptr and
// casts are transparent, constant-offset arithmetic too. Used to detect
// copy-routine roots: a size-switch memcpy materializes one inttoptr per
// case, all scalarizing to the same argument.
const Value *scalarizePtrRoot(const Value *V) {
  const Value *Cur = V;
  SmallPtrSet<const Value *, 8> Seen;
  while (Cur && Seen.insert(Cur).second) {
    const Value *Next = nullptr;
    if (const auto *Cast = dyn_cast<CastInst>(Cur)) {
      if (Cast->getOpcode() == Instruction::IntToPtr ||
          Cast->getOpcode() == Instruction::BitCast ||
          Cast->getOpcode() == Instruction::AddrSpaceCast)
        Next = Cast->getOperand(0);
    } else if (const auto *BO = dyn_cast<BinaryOperator>(Cur)) {
      if ((BO->getOpcode() == Instruction::Add ||
           BO->getOpcode() == Instruction::Sub) &&
          isa<ConstantInt>(BO->getOperand(1)))
        Next = BO->getOperand(0);
    } else if (const auto *GEP = dyn_cast<GetElementPtrInst>(Cur)) {
      if (GEP->hasAllConstantIndices())
        Next = GEP->getPointerOperand();
    }
    if (!Next || Next == Cur)
      break;
    Cur = Next;
  }
  return Cur;
}

// Erase P when it is a now-dead address instruction (GEP or cast), then
// recurse into its pointer operand while that stays single-use and dead.
void eraseDeadAddressChain(Value *P) {
  while (auto *I = dyn_cast_or_null<Instruction>(P)) {
    if (!I->use_empty())
      return;
    Value *Next = nullptr;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(I))
      Next = GEP->getPointerOperand();
    else if (auto *Cast = dyn_cast<CastInst>(I))
      Next = Cast->getOperand(0);
    else
      return;
    I->eraseFromParent();
    P = Next;
  }
}

// True when the alloca's address escapes anywhere except simple loads,
// stores, lifetime markers, and zero-offset GEPs.
bool isAddressTaken(AllocaInst *AI) {
  SmallVector<const Value *, 8> Work{AI};
  SmallPtrSet<const Value *, 16> Seen;
  while (!Work.empty()) {
    const Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (const User *U : V->users()) {
      const auto *I = dyn_cast<Instruction>(U);
      if (!I)
        return true;
      switch (I->getOpcode()) {
      case Instruction::Load:
        break;
      case Instruction::Store:
        break;
      case Instruction::BitCast:
        Work.push_back(cast<BitCastInst>(I));
        break;
      case Instruction::GetElementPtr:
        if (cast<GetElementPtrInst>(I)->hasAllZeroIndices())
          Work.push_back(cast<GetElementPtrInst>(I));
        else
          return true;
        break;
      case Instruction::Call: {
        const auto *II = dyn_cast<IntrinsicInst>(I);
        if (!II)
          return true;
        switch (II->getIntrinsicID()) {
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
          break;
        default:
          return true;
        }
        break;
      }
      default:
        return true;
      }
    }
  }
  return false;
}

// <stem, index> for `stem.sroa.index` / `stem.sroa.index.addr` names.
bool parseSroaName(StringRef Name, std::string &Stem, uint64_t &Index) {
  size_t Pos = Name.rfind(".sroa.");
  if (Pos == StringRef::npos)
    return false;
  StringRef Tail = Name.substr(Pos + 6);
  if (Tail.consume_back(".addr")) {
  }
  unsigned long long N = 0;
  if (Tail.empty() || Tail.getAsInteger(10, N))
    return false;
  Stem = Name.substr(0, Pos).str();
  Index = N;
  return true;
}

// Field layout for a struct type: byte offset + size per top-level field.
struct FieldLayout {
  uint64_t Offset = 0;
  uint64_t Size = 0;
  Type *Ty = nullptr;
};

bool structFieldLayout(StructType *ST, const DataLayout &DL,
                       SmallVectorImpl<FieldLayout> &Out) {
  const StructLayout *SL = DL.getStructLayout(ST);
  for (unsigned i = 0, e = ST->getNumElements(); i < e; ++i) {
    FieldLayout F;
    F.Offset = SL->getElementOffset(i);
    F.Ty = ST->getElementType(i);
    F.Size = DL.getTypeAllocSize(F.Ty);
    if (F.Size == 0)
      return false;
    Out.push_back(F);
  }
  return true;
}

// (Kind, Bits) for scalar access types: 0 = integer, 1 = pointer,
// 2 = float. Anything else (vectors, aggregates) has no struct-field
// meaning and never matches.
bool accessKindBits(Type *T, unsigned &Kind, unsigned &Bits) {
  if (T->isIntegerTy()) {
    Kind = 0;
    Bits = T->getIntegerBitWidth();
    return true;
  }
  if (T->isPointerTy()) {
    Kind = 1;
    Bits = 0;
    return true;
  }
  if (T->isFloatTy()) {
    Kind = 2;
    Bits = T->getPrimitiveSizeInBits();
    return true;
  }
  return false;
}
bool rebuildScalarGroups(Function &F, const DataLayout &DL) {
  // Group entry-block allocas by stem. StringMap has no stable iteration
  // order, so keep a parallel order vector for determinism.
  StringMap<SmallVector<AllocaInst *, 4>> Groups;
  SmallVector<std::string, 4> Order;
  BasicBlock &Entry = F.getEntryBlock();
  for (Instruction &I : Entry) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI || !AI->hasName())
      continue;
    std::string Stem;
    uint64_t Index = 0;
    if (!parseSroaName(AI->getName(), Stem, Index))
      continue;
    auto It = Groups.find(Stem);
    if (It == Groups.end()) {
      Order.push_back(Stem);
      Groups[Stem].push_back(AI);
    } else {
      It->second.push_back(AI);
    }
  }

  bool Changed = false;
  for (const std::string &Key : Order) {
    SmallVector<AllocaInst *, 4> &Members = Groups[Key];
    if (Members.size() < 2)
      continue;

    // Every member must be a plain scalar slot: no address-taken, no
    // volatile/atomic users, element type not aggregate.
    uint64_t Total = 0;
    bool Veto = false;
    for (AllocaInst *AI : Members) {
      Type *ET = AI->getAllocatedType();
      if (ET->isAggregateType() || ET->isEmptyTy()) {
        Veto = true;
        break;
      }
      if (isAddressTaken(AI)) {
        Veto = true;
        break;
      }
      for (const User *U : AI->users()) {
        if (const auto *LI = dyn_cast<LoadInst>(U)) {
          if (LI->isVolatile() || LI->isAtomic()) {
            Veto = true;
            break;
          }
        } else if (const auto *SI = dyn_cast<StoreInst>(U)) {
          if (SI->isVolatile() || SI->isAtomic()) {
            Veto = true;
            break;
          }
        } else if (const auto *II = dyn_cast<IntrinsicInst>(U)) {
          if (II->getIntrinsicID() != Intrinsic::lifetime_start &&
              II->getIntrinsicID() != Intrinsic::lifetime_end) {
            Veto = true;
            break;
          }
        } else {
          Veto = true;
          break;
        }
      }
      if (Veto)
        break;
      Total += DL.getTypeAllocSize(ET);
    }
    if (Veto || Total == 0)
      continue;

    // Build one i8 aggregate of the summed extent, aligned to the max
    // member alignment.
    Align AggAlign(1);
    for (AllocaInst *AI : Members)
      AggAlign = std::max(AggAlign, AI->getAlign());
    LLVMContext &Ctx = F.getContext();
    Type *AggTy = ArrayType::get(Type::getInt8Ty(Ctx), Total);
    IRBuilder<> B(Members.front());
    AllocaInst *Agg =
        B.CreateAlloca(AggTy, nullptr, Key.empty() ? "rv.aggsroa" : Key);
    Agg->setAlignment(AggAlign);

    // Assign packed offsets in index order, then rewrite each member
    // access as a byte GEP into the aggregate.
    llvm::sort(Members, [](AllocaInst *A, AllocaInst *B) {
      std::string SA, SB;
      uint64_t IA = 0, IB = 0;
      parseSroaName(A->getName(), SA, IA);
      parseSroaName(B->getName(), SB, IB);
      return IA < IB;
    });
    uint64_t Off = 0;
    for (AllocaInst *AI : Members) {
      uint64_t Size = DL.getTypeAllocSize(AI->getAllocatedType());
      Type *ET = AI->getAllocatedType();
      SmallVector<Instruction *, 8> Users;
      for (User *U : AI->users()) {
        if (auto *I = dyn_cast<Instruction>(U))
          Users.push_back(I);
      }
      bool LocalVeto = false;
      for (Instruction *U : Users) {
        if (isa<LoadInst>(U) || isa<StoreInst>(U))
          continue;
        if (auto *II = dyn_cast<IntrinsicInst>(U)) {
          if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
              II->getIntrinsicID() == Intrinsic::lifetime_end)
            continue;
        }
        LocalVeto = true;
        break;
      }
      if (LocalVeto)
        continue;
      IRBuilder<> UB(AI);
      Value *BasePtr = UB.CreateBitCast(Agg, PointerType::getUnqual(Ctx));
      Value *Slot = UB.CreateConstInBoundsGEP1_64(
          Type::getInt8Ty(Ctx), BasePtr, Off);
      Instruction *SlotI = cast<Instruction>(Slot);
      copyVA(AI, SlotI);
      Value *Typed = UB.CreateBitCast(
          Slot, PointerType::get(Ctx, AI->getAddressSpace()));
      Instruction *TypedI = cast<Instruction>(Typed);
      copyVA(AI, TypedI);
      for (Instruction *U : Users) {
        if (auto *LI = dyn_cast<LoadInst>(U)) {
          LI->setOperand(0, Typed);
        } else if (auto *SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() == AI)
            SI->setOperand(1, Typed);
        } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
          II->setOperand(0, Typed);
        }
      }
      // Drop the member when nothing references it anymore.
      if (AI->use_empty()) {
        copyVA(AI, Agg);
        AI->eraseFromParent();
        Changed = true;
      }
      Off += Size;
    }
  }
  return Changed;
}

// True when byte offset B inside T (whose object starts at Base) falls
// on an elementary boundary: a scalar/field edge at any nesting depth.
// Copy runs never split an elementary field, so a boundary inside a
// leaf scalar is not a copy boundary.
bool typeBoundaryAt(Type *T, const DataLayout &DL, uint64_t Base,
                    uint64_t B, unsigned Depth = 0) {
  if (B == Base)
    return true;
  uint64_t Sz = DL.getTypeAllocSize(T);
  if (B == Base + Sz)
    return true;
  if (B < Base || B >= Base + Sz || Depth >= 8)
    return false;
  if (auto *ST = dyn_cast<StructType>(T)) {
    const StructLayout *SL = DL.getStructLayout(ST);
    for (unsigned i = 0, e = ST->getNumElements(); i < e; ++i) {
      uint64_t FO = Base + SL->getElementOffset(i);
      if (B >= FO && B <= FO + DL.getTypeAllocSize(ST->getElementType(i)))
        if (typeBoundaryAt(ST->getElementType(i), DL, FO, B, Depth + 1))
          return true;
    }
    return false;
  }
  if (auto *AT = dyn_cast<ArrayType>(T)) {
    uint64_t ES = DL.getTypeAllocSize(AT->getElementType());
    if (ES == 0)
      return false;
    uint64_t EBase = Base + (B - Base) / ES * ES;
    return typeBoundaryAt(AT->getElementType(), DL, EBase, B, Depth + 1);
  }
  return false; // inside a scalar leaf
}

// True when [RelOff, RelOff+Width) covers whole elementary fields of ST:
// both edges and every internal boundary of the folded run land on
// field boundaries at any depth. A scalarized copy of a struct never
// splits a leaf field, so a run whose pairs cut through one cannot be a
// struct copy.
bool fieldAlignedWindow(StructType *ST, const DataLayout &DL,
                        uint64_t RelOff, uint64_t Width,
                        ArrayRef<uint64_t> Bounds) {
  if (!typeBoundaryAt(ST, DL, 0, RelOff) ||
      !typeBoundaryAt(ST, DL, 0, RelOff + Width))
    return false;
  for (uint64_t B : Bounds)
    if (B != 0 && !typeBoundaryAt(ST, DL, 0, RelOff + B))
      return false;
  return true;
}

// True when the folded window [RelOff, RelOff+Width) at Root is provably
// an aggregate region on this side:
//  - a whole known object: alloca/global sized exactly Width at offset 0
//    (aggregate, or a scalar slot of >=16 bytes the compiler would have
//    copied as a unit),
//  - a field-aligned window of a struct-typed base, or
//  - for an opaque root (argument, inttoptr, call result, ...): a
//    byte window starting at 0 whose size and internal boundaries match
//    an identified module struct -- the shape rebuilder E writes for
//    real field clusters. The matching type is returned in Match so the
//    caller can require both sides to agree on one layout.
// Byte-dispatch code (CRT memcpy size switches over opaque inttoptr
// roots) satisfies none of these and must not fold.
bool copyWindowEvidence(Module &M, const DataLayout &DL, const Value *Root,
                        uint64_t RelOff, uint64_t Width,
                        ArrayRef<uint64_t> Bounds, StructType *&Match) {
  Match = nullptr;
  Type *ObjTy = nullptr;
  if (const auto *AI = dyn_cast<AllocaInst>(Root))
    ObjTy = AI->getAllocatedType();
  else if (const auto *GV = dyn_cast<GlobalValue>(Root))
    ObjTy = GV->getValueType();
  if (ObjTy) {
    if (auto *ST = dyn_cast<StructType>(ObjTy))
      return !ST->isOpaque() &&
             fieldAlignedWindow(ST, DL, RelOff, Width, Bounds);
    uint64_t ObjSize = DL.getTypeAllocSize(ObjTy);
    if (ObjSize == 0 || RelOff != 0 || Width != ObjSize)
      return false;
    return ObjTy->isAggregateType() || Width >= 16;
  }
  if (RelOff != 0)
    return false;
  for (StructType *ST : M.getIdentifiedStructTypes()) {
    if (ST->isOpaque() || ST->isPacked() || ST->getNumElements() < 2)
      continue;
    if (DL.getTypeAllocSize(ST) != Width)
      continue;
    if (fieldAlignedWindow(ST, DL, 0, Width, Bounds)) {
      Match = ST;
      return true;
    }
  }
  return false;
}

// Corroboration for opaque-side matches: Root must show field traffic
// beyond the copy run -- loads, or stores whose value is not itself a
// load (a load/store pair is copy traffic, not evidence), hitting >=2
// distinct fields of ST with the exact field types. Size alone is a
// coincidence (a 20-byte memcpy is not a {i64,i64,i32}); non-copy field
// traffic on the same root is what makes the window a struct copy.
bool hasFieldEvidence(Function &F, const DataLayout &DL, const Value *Root,
                      StructType *ST) {
  const StructLayout *SL = DL.getStructLayout(ST);
  SmallVector<char, 8> Seen(ST->getNumElements(), 0);
  unsigned SeenCount = 0;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      Value *Ptr = nullptr;
      Type *Ty = nullptr;
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->isVolatile() || LI->isAtomic())
          continue;
        // A load feeding one store is copy traffic, not field traffic.
        if (LI->hasOneUse() && isa<StoreInst>(*LI->user_begin()))
          continue;
        Ptr = LI->getPointerOperand();
        Ty = LI->getType();
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->isVolatile() || SI->isAtomic() ||
            isa<LoadInst>(SI->getValueOperand()))
          continue;
        Ptr = SI->getPointerOperand();
        Ty = SI->getValueOperand()->getType();
      } else {
        continue;
      }
      uint64_t Off = 0;
      const Value *R = resolveFootprintBase(skipCopies(Ptr), DL, Off);
      if (!R || skipCopies(R) != Root)
        continue;
      for (unsigned i = 0, e = ST->getNumElements(); i < e; ++i)
        if (SL->getElementOffset(i) == Off &&
            ST->getElementType(i) == Ty && !Seen[i]) {
          Seen[i] = 1;
          if (++SeenCount >= 2)
            return true;
        }
    }
  }
  return false;
}

// Rebuild B: fold adjacent scalar load/store copy runs into llvm.memcpy.
// Returns true on any fold.
bool rebuildCopyRuns(Function &F, const DataLayout &DL) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    // Collect scalar stores in order; a copy run is a maximal run of
    // `store (load src_i), dst_i` pairs with contiguous offsets on both
    // sides. Offsets come from constant byte GEPs (or direct allocas at
    // offset 0).
    struct CopyPair {
      LoadInst *Load = nullptr;
      StoreInst *Store = nullptr;
      const Value *SrcBase = nullptr;
      const Value *DstBase = nullptr;
      uint64_t SrcOff = 0;
      uint64_t DstOff = 0;
      uint64_t Width = 0;
    };
    auto baseAndOffset = [&](Value *Ptr, const Value *&Base,
                             uint64_t &Off) -> bool {
      // Peel every constant-index GEP layer; the base may itself be a
      // GEP (test IR threads field GEPs through byte GEPs). Only the
      // total constant offset matters for contiguity.
      const Value *Root = resolveFootprintBase(skipCopies(Ptr), DL, Off);
      if (!Root)
        return false;
      Base = skipCopies(Root);
      return true;
    };

    SmallVector<CopyPair, 8> Run;
    auto flushRun = [&]() {
      if (Run.size() < 2) {
        Run.clear();
        return;
      }
      // Verify contiguity from offset 0 of the moved range on both sides
      // and uniform scalar widths that tile without gaps.
      const Value *SB = Run.front().SrcBase;
      const Value *DB = Run.front().DstBase;
      uint64_t Total = 0;
      for (const CopyPair &P : Run)
        Total += P.Width;
      uint64_t SrcCursor = Run.front().SrcOff;
      uint64_t DstCursor = Run.front().DstOff;
      for (const CopyPair &P : Run) {
        if (P.SrcBase != SB || P.DstBase != DB ||
            P.SrcOff != SrcCursor || P.DstOff != DstCursor) {
          Run.clear();
          return;
        }
        if (P.Load->isVolatile() || P.Load->isAtomic() ||
            P.Store->isVolatile() || P.Store->isAtomic()) {
          Run.clear();
          return;
        }
        SrcCursor += P.Width;
        DstCursor += P.Width;
      }
      // Aggregate evidence: a fold must be explainable as one source
      // struct copy -- a field-aligned window of a struct-typed base, a
      // whole known object, or a byte window matching a module struct
      // type. Both bases must address the same relative offset. Byte-
      // dispatch copy code carries no such evidence and stays scalar.
      // When opaque roots match a module struct type, both sides must
      // agree on one layout and show corroborating field traffic.
      if (Run.front().SrcOff != Run.front().DstOff) {
        Run.clear();
        return;
      }
      SmallVector<uint64_t, 8> Bounds;
      for (const CopyPair &P : Run)
        Bounds.push_back(P.SrcOff - Run.front().SrcOff);
      StructType *SrcST = nullptr, *DstST = nullptr;
      Module *M = F.getParent();
      if (!copyWindowEvidence(*M, DL, SB, Run.front().SrcOff, Total,
                              Bounds, SrcST) ||
          !copyWindowEvidence(*M, DL, DB, Run.front().DstOff, Total,
                              Bounds, DstST)) {
        Run.clear();
        return;
      }
      if (SrcST && DstST) {
        // Both sides opaque: the window must match ONE layout and each
        // root must show corroborating field traffic -- size alone
        // proves nothing (a 20-byte memcpy is not a {i64,i64,i32}).
        // When only one side matched a type, the other's known-object
        // evidence already anchors the layout.
        if (SrcST != DstST || !hasFieldEvidence(F, DL, SB, SrcST) ||
            !hasFieldEvidence(F, DL, DB, DstST)) {
          Run.clear();
          return;
        }
      }
      // Emit one memcpy before the first store. The declaration is built
      // manually and left bare (no attributes): IRBuilder's CreateMemCpy
      // attaches the intrinsic's canonical attribute list, whose interning
      // does not survive the plugin DLL boundary in this build (opt's
      // verifier reports "Attribute list does not match Module context").
      // The verifier skips empty attribute lists, the `llvm.memcpy.*`
      // name still identifies the intrinsic, and later stock passes
      // (inferattrs) re-derive canonical attributes by name.
      StoreInst *First = Run.front().Store;
      IRBuilder<> B(First);
      LLVMContext &Ctx = M->getContext();
      auto *I8Ptr = PointerType::getUnqual(Ctx);
      FunctionType *McTy = FunctionType::get(
          Type::getVoidTy(Ctx), {I8Ptr, I8Ptr, B.getInt64Ty(), B.getInt1Ty()},
          false);
      Function *McFn = M->getFunction("llvm.memcpy.p0.p0.i64");
      if (McFn) {
        // Reuse only a well-formed intrinsic declaration; never touch its
        // attributes. Anything else means the module is mid-upgrade: bail.
        if (McFn->getIntrinsicID() != Intrinsic::memcpy ||
            McFn->getFunctionType() != McTy) {
          Run.clear();
          return;
        }
      } else {
        McFn = Function::Create(McTy, GlobalValue::ExternalLinkage, 0,
                                "llvm.memcpy.p0.p0.i64", M);
        McFn->setAttributes(AttributeList());
      }
      Value *DstRaw = First->getPointerOperand();
      Value *SrcRaw = Run.front().Load->getPointerOperand();
      Value *DstByte = DstRaw;
      if (DstRaw->getType() != I8Ptr) {
        DstByte = B.CreateBitCast(DstRaw, I8Ptr);
        copyVA(First, cast<Instruction>(DstByte));
      }
      Value *SrcByte = SrcRaw;
      if (SrcRaw->getType() != I8Ptr) {
        SrcByte = B.CreateBitCast(SrcRaw, I8Ptr);
        copyVA(Run.front().Load, cast<Instruction>(SrcByte));
      }
      // The first pair's pointers already address the run start: pairs
      // were required contiguous from Run.front(), so no rebase needed.
      CallInst *Mc = B.CreateCall(McFn, {DstByte, SrcByte,
                                          B.getInt64(Total), B.getFalse()});
      copyVA(First, Mc);
      for (const CopyPair &P : Run) {
        // Harvest the address math feeding each pair before erasing the
        // memory ops: now-dead GEPs would otherwise linger.
        auto *LP = dyn_cast<GetElementPtrInst>(P.Load->getPointerOperand());
        auto *SP = dyn_cast<GetElementPtrInst>(P.Store->getPointerOperand());
        P.Store->eraseFromParent();
        if (P.Load->use_empty())
          P.Load->eraseFromParent();
        if (LP && LP->use_empty())
          LP->eraseFromParent();
        if (SP && SP != LP && SP->use_empty())
          SP->eraseFromParent();
      }
      Changed = true;
      Run.clear();
    };

    for (Instruction &I : BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || SI->isVolatile() || SI->isAtomic() || isOpaqueCall(I)) {
        // Pure address math, plain loads, and lifetime markers cannot
        // clobber the copy, so they don't break the run (real copies
        // interleave GEPs and loads between the pair stores). Anything
        // that may write memory flushes it.
        if (isa<GetElementPtrInst>(I) || isa<BitCastInst>(I))
          continue;
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          if (!LI->isVolatile() && !LI->isAtomic())
            continue;
        }
        if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
          if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
              II->getIntrinsicID() == Intrinsic::lifetime_end)
            continue;
        }
        if (!Run.empty())
          flushRun();
        continue;
      }
      auto *LI = dyn_cast<LoadInst>(SI->getValueOperand());
      if (!LI || LI->isVolatile() || LI->isAtomic() ||
          !LI->hasOneUse() || LI->getParent() != &BB) {
        if (!Run.empty())
          flushRun();
        continue;
      }
      Type *LT = LI->getType();
      if (!LT->isIntegerTy() && !LT->isFloatTy() && !LT->isPointerTy()) {
        if (!Run.empty())
          flushRun();
        continue;
      }
      const Value *SB = nullptr, *DB = nullptr;
      uint64_t SO = 0, DO = 0;
      bool SOk = baseAndOffset(LI->getPointerOperand(), SB, SO);
      bool DOk = baseAndOffset(SI->getPointerOperand(), DB, DO);
      if (!SOk || !DOk)
        SB = DB = nullptr;
      if (!SB || !DB || SB == DB) {
        if (!Run.empty())
          flushRun();
        continue;
      }
      // Width must equal the loaded type store size; reject padding gaps
      // by requiring the next pair to continue exactly where this ends
      // (checked at flush).
      uint64_t W = DL.getTypeStoreSize(LT);
      if (!Run.empty()) {
        const CopyPair &Prev = Run.back();
        if (Prev.SrcBase != SB || Prev.DstBase != DB ||
            SO != Prev.SrcOff + Prev.Width || DO != Prev.DstOff + Prev.Width)
          flushRun();
      }
      Run.push_back({LI, SI, SB, DB, SO, DO, W});
    }
    if (!Run.empty())
      flushRun();
  }
  return Changed;
}

// Rebuild C: byte GEPs back to typed struct field GEPs using the module's
// struct types. Matches c-clang-o2 `struct_access`: `gep i8, ptr %0,
// i64 4` with `%struct.Point = { i32, i32, ptr }` becomes the field-1 GEP.
bool rebuildTypedGeps(Function &F, const DataLayout &DL, unsigned Skip) {
  Module *M = F.getParent();
  if (!M)
    return false;
  // Index struct types by (size, field-offset, field-type-size) so a byte
  // offset resolves to (struct, field) candidates. A sorted vector, not
  // a DenseMap: the table is tiny (~100 entries) and this keeps all
  // ownership inside the pass.
  struct Cand {
    StructType *ST = nullptr;
    unsigned Field = 0;
  };
  struct Entry {
    uint64_t Key = 0;
    Cand C;
  };
  SmallVector<Entry, 128> ByOffset;
  for (StructType *ST : M->getIdentifiedStructTypes()) {
    if (ST->isOpaque() || ST->isPacked())
      continue;
    SmallVector<FieldLayout, 8> Fields;
    if (!structFieldLayout(ST, DL, Fields))
      continue;
    uint64_t StructSize = DL.getTypeAllocSize(ST);
    for (unsigned i = 0; i < Fields.size(); ++i) {
      // Key: struct size + field offset + field size. A byte GEP matches
      // when the access width equals the field size below.
      uint64_t Key = (StructSize << 32) | (Fields[i].Offset << 16) |
                     std::min<uint64_t>(Fields[i].Size, 0xFFFF);
      ByOffset.push_back({Key, {ST, i}});
    }
  }

  bool Changed = false;
  SmallVector<GetElementPtrInst *, 16> Geps;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *GEP = dyn_cast<GetElementPtrInst>(&I);
      if (!GEP || isOpaqueCall(I))
        continue;
      if (GEP->getSourceElementType()->isStructTy())
        continue;
      if (!GEP->hasAllConstantIndices())
        continue;
      Geps.push_back(GEP);
    }
  // Access footprint per base pointer: (root, offset, kind, bits) for
  // every load/store through a resolvable byte address. A no-evidence
  // struct retitle needs a second footprint offset into the SAME struct
  // (TypeRecovery's "two offsets make a struct" rule): a lone offset
  // through an opaque pointer is as likely vtable indexing or a spill
  // as a field (virtual_dispatch's second vtable slot, rustc spills).
  struct Footprint {
    const Value *Root = nullptr;
    uint64_t Off = 0;
    unsigned Kind = 0;
    unsigned Bits = 0;
  };
  SmallVector<Footprint, 32> Prints;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      Value *Ptr = nullptr;
      Type *AccTy = nullptr;
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->isVolatile() || LI->isAtomic())
          continue;
        Ptr = LI->getPointerOperand();
        AccTy = LI->getType();
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->isVolatile() || SI->isAtomic())
          continue;
        Ptr = SI->getPointerOperand();
        AccTy = SI->getValueOperand()->getType();
      } else {
        continue;
      }
      unsigned K = 0, B = 0;
      if (!accessKindBits(AccTy, K, B))
        continue;
      uint64_t Total = 0;
      const Value *Root =
          resolveByteBase(Ptr, DL, F.getContext(), Total);
      if (!Root)
        continue;
      Prints.push_back({Root, Total, K, B});
    }

  // Erased GEPs are tracked by index into the fixed worklist. Pointer-keyed
  // sets become stale after eraseFromParent and can collide with recycled
  // instruction addresses.
  SmallVector<bool, 16> DeadIdx(Geps.size(), false);
  auto isChainDead = [&](GetElementPtrInst *C) {
    for (size_t i = 0, e = Geps.size(); i < e; ++i)
      if (DeadIdx[i] && Geps[i] == C)
        return true;
    return false;
  };
  for (size_t GI = 0, GE = Geps.size(); GI < GE; ++GI) {
    GetElementPtrInst *GEP = Geps[GI];
    // Never dereference a dead entry: eraseFromParent frees the object.
    if (DeadIdx[GI])
      continue;
    // Peel a chain of single-use byte GEPs: `%39 = gep i8, (%38 = gep
    // i8, base, 4), 0` addresses base+4, and the inner GEP dies with the
    // fold. Only pure byte steps with no other users peel: a variable
    // base underneath vetoes the whole chain (hash_vec's walking i32
    // window must never gain a struct label).
    APInt ByteOff(DL.getIndexSizeInBits(0), 0);
    if (!GEP->accumulateConstantOffset(DL, ByteOff))
      continue;
    GetElementPtrInst *ChainHead = GEP;
    SmallVector<GetElementPtrInst *, 4> Chain;
    bool ChainVeto = false;
    while (auto *BaseGEP =
               dyn_cast<GetElementPtrInst>(ChainHead->getPointerOperand())) {
      if (BaseGEP->getSourceElementType() != Type::getInt8Ty(F.getContext()) ||
          !BaseGEP->hasAllConstantIndices() || !BaseGEP->hasOneUse()) {
        ChainVeto = true;
        break;
      }
      APInt BaseOff(DL.getIndexSizeInBits(0), 0);
      if (!BaseGEP->accumulateConstantOffset(DL, BaseOff)) {
        ChainVeto = true;
        break;
      }
      ByteOff += BaseOff;
      Chain.push_back(BaseGEP);
      ChainHead = BaseGEP;
    }
    // Any veto (non-byte step, multi-use step, or a variable-indexed base
    // under the chain) means the address walks: never retitle.
    if (ChainVeto)
      continue;
    uint64_t Off = ByteOff.getZExtValue();
    // The effective base is the chain head's base; inner links die with
    // the fold when nothing else uses them.
    Value *EffBase = ChainHead->getPointerOperand();
    // Find the single user access type: all load/store users must agree
    // on one type for the field match to be sound. The kind (int vs
    // pointer vs float) must match the field's kind: rustc spills every
    // local as a byte array, and size-only matching invents struct
    // references the pre-IR never had (e.g. an i64 spill pair read as a
    // `{ptr, i64}` field pair).
    Type *AccessTy = nullptr;
    bool Mixed = false;
    for (User *U : GEP->users()) {
      Type *T = nullptr;
      if (auto *LI = dyn_cast<LoadInst>(U))
        T = LI->getType();
      else if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getPointerOperand() != GEP)
          continue;
        T = SI->getValueOperand()->getType();
      } else {
        Mixed = true;
        break;
      }
      if (!AccessTy)
        AccessTy = T;
      else if (AccessTy != T) {
        Mixed = true;
        break;
      }
    }
    if (Mixed || !AccessTy)
      continue;
    uint64_t Width = DL.getTypeStoreSize(AccessTy);
    if (Width == 0)
      continue;
    unsigned AccessKind = 0, AccessBits = 0;
    if (!accessKindBits(AccessTy, AccessKind, AccessBits))
      continue;
    // Array-element case: when the base is an alloca/global of `[N x T]`
    // and the offset lands exactly on an element whose store size equals
    // the access width, rebuild the array element GEP the pre-IR had
    // (c-clang-o1 main: `gep i8, %7, 12` off a `[16 x i32]` alloca becomes
    // `gep [16 x i32], %7, 0, 3`). Sound: one element at an element
    // boundary is exactly what the byte access reads/writes. This runs
    // before struct matching: array bases rarely appear in the struct
    // table, and the base type here is exact evidence.
    const Value *BaseRoot = skipCopies(EffBase);
    Type *BaseAllocTy = nullptr;
    if (const auto *AI = dyn_cast<AllocaInst>(BaseRoot))
      BaseAllocTy = AI->getAllocatedType();
    else if (const auto *GV = dyn_cast<GlobalValue>(BaseRoot))
      BaseAllocTy = GV->getValueType();
    if (!(Skip & 16))
      if (auto *ArrTy = dyn_cast_or_null<ArrayType>(BaseAllocTy)) {
      Type *ElemTy = ArrTy->getElementType();
      uint64_t ElemSize = DL.getTypeAllocSize(ElemTy);
      // Byte elements are excluded: indexing a byte spill with a byte
      // GEP adds no information (rustc pre-IR never uses array GEPs).
      if (ElemSize > 1 && Off % ElemSize == 0 &&
          DL.getTypeStoreSize(ElemTy) == Width &&
          Off / ElemSize < ArrTy->getNumElements()) {
        IRBuilder<> B(GEP);
        Value *Base = EffBase;
        Value *Zero = B.getInt64(0);
        Value *Index = B.getInt64(Off / ElemSize);
        auto *Typed = cast<GetElementPtrInst>(
            B.CreateInBoundsGEP(ArrTy, Base, {Zero, Index}));
        copyVA(GEP, Typed);
        if (Typed->getType() != GEP->getType()) {
          auto *Cast = B.CreateBitCast(Typed, GEP->getType());
          copyVA(GEP, cast<Instruction>(Cast));
          GEP->replaceAllUsesWith(Cast);
        } else {
          GEP->replaceAllUsesWith(Typed);
        }
        if (GEP->use_empty()) {
          GEP->eraseFromParent();
          DeadIdx[GI] = true;
          for (GetElementPtrInst *C : Chain)
            if (!isChainDead(C) && C->use_empty()) {
              C->eraseFromParent();
              for (size_t i = 0, e = Geps.size(); i < e; ++i)
                if (Geps[i] == C)
                  DeadIdx[i] = true;
            }
          Changed = true;
        }
        continue;
      }
    }
    // Match (struct, field) by offset+width across every struct size. A
    // base whose allocated type is known and not a struct vetoes the
    // match: rustc lowers every local to a byte-array spill, and guessing
    // a struct there invents types the pre-IR never had. Opaque bases
    // (arguments, loaded pointers) fall through to the unanimity rule
    // below, which is what fires struct_access(ptr %0).
    if (BaseAllocTy && !BaseAllocTy->isStructTy())
      continue;
    SmallVector<Cand, 2> Cands;
    for (const Entry &E : ByOffset) {
      uint64_t KeyOff = (E.Key >> 16) & 0xFFFF;
      uint64_t KeyW = E.Key & 0xFFFF;
      if (KeyOff != Off || KeyW != Width)
        continue;
      // The field's kind must match the access kind: same-size layouts
      // with different kinds (an i64 spill pair vs a `{ptr, i64}` struct)
      // are different types, and the pre-IR never confuses them.
      Type *FieldTy = E.C.ST->getElementType(E.C.Field);
      unsigned FieldKind = 0, FieldBits = 0;
      if (!accessKindBits(FieldTy, FieldKind, FieldBits))
        continue;
      if (FieldKind != AccessKind)
        continue;
      if (FieldKind != 1 && FieldBits != AccessBits)
        continue;
      Cands.push_back(E.C);
    }
    if (Cands.empty())
      continue;
    // Prefer the candidate whose struct matches the base alloca type;
    // else require uniqueness (struct-size class is ambiguous, so only
    // fire when every candidate agrees on field index AND field type).
    Cand Pick = Cands.front();
    bool Ambiguous = false;
    bool FoundBase = false;
    for (Cand C : Cands) {
      if (BaseAllocTy == C.ST) {
        Pick = C;
        FoundBase = true;
        break;
      }
    }
    if (!FoundBase) {
      // No base evidence: fire only when all candidates name the same
      // field index with equal field types (same layout modulo name).
      for (Cand C : Cands) {
        if (C.Field != Pick.Field ||
            C.ST->getElementType(C.Field) !=
                Pick.ST->getElementType(Pick.Field)) {
          Ambiguous = true;
          break;
        }
      }
      if (Ambiguous)
        continue;
      // Corroboration: the same base must show another footprint offset
      // into the SAME struct with matching kind. A lone offset through
      // an opaque pointer is as likely vtable indexing (virtual_dispatch
      // reads slot 1 at +8 through a loaded vtable pointer) as a field.
      bool Corroborated = false;
      SmallVector<FieldLayout, 8> PickFields;
      if (structFieldLayout(Pick.ST, DL, PickFields)) {
        for (const Footprint &FP : Prints) {
          if (FP.Root != BaseRoot || FP.Off == Off)
            continue;
          for (unsigned f = 0; f < PickFields.size(); ++f) {
            Type *FT = Pick.ST->getElementType(f);
            unsigned FK = 0, FB = 0;
            if (PickFields[f].Offset != FP.Off ||
                !accessKindBits(FT, FK, FB))
              continue;
            if (FK != FP.Kind || (FK != 1 && FB != FP.Bits))
              continue;
            Corroborated = true;
            break;
          }
          if (Corroborated)
            break;
        }
      }
      if (!Corroborated)
        continue;
    }
    if (Skip & 32)
      continue;
    IRBuilder<> B(GEP);
    Value *Base = EffBase;
    Value *Zero = B.getInt32(0);
    Value *Field = B.getInt32(Pick.Field);
    auto *Typed =
        cast<GetElementPtrInst>(B.CreateInBoundsGEP(Pick.ST, Base, {Zero, Field}));
    copyVA(GEP, Typed);
    // Preserve the byte GEP's result type when it differs (opaque ptr is
    // the norm, so this is usually a drop-in).
    if (Typed->getType() != GEP->getType()) {
      auto *Cast = B.CreateBitCast(Typed, GEP->getType());
      copyVA(GEP, cast<Instruction>(Cast));
      GEP->replaceAllUsesWith(Cast);
    } else {
      GEP->replaceAllUsesWith(Typed);
    }
    // Keep the old GEP dead until DCE; erase when unreferenced, with
    // peeled inner links that died with the fold.
    if (GEP->use_empty()) {
      GEP->eraseFromParent();
      DeadIdx[GI] = true;
      for (GetElementPtrInst *C : Chain)
        if (!isChainDead(C) && C->use_empty()) {
          C->eraseFromParent();
          for (size_t i = 0, e = Geps.size(); i < e; ++i)
            if (Geps[i] == C)
              DeadIdx[i] = true;
        }
      Changed = true;
    }
  }
  return Changed;
}

// Rebuild E: field-access clusters -> synthesized struct types.
//
// Lifted modules carry no struct types, so rebuilder C's module-type
// matching never fires there. The aggregate evidence still exists: a
// pointer root whose scalar loads/stores hit two or more distinct
// constant offsets is a struct to TypeRecovery ("two offsets make a
// struct"), and to the pre-IR it was one. E rebuilds that shape: it
// groups constant-offset footprints by their root value, vetoes the
// shapes that are not aggregates, synthesizes `struct.agg.<fn>.<n>`
// from the survivors, and retitles each access to a struct field GEP.
//
// Cluster rules, per root:
//  - eligibility: an Argument or Instruction root (inttoptr, PHI, call),
//    or an alloca whose allocated type is a byte container ([N x i8]/i8)
//    or a struct. Globals never cluster (module arrays and vtables are
//    not local objects) and neither do constants.
//  - >=2 distinct offsets, each accessed with one unanimous scalar type;
//    offsets+widths never overlap; extent <= 256 bytes.
//  - struct-likeness: a dense uniform run of >=3 same-type offsets reads
//    as an array (unrolled dot products) and is vetoed; an adjacent
//    same-type pair likewise. Mixed kinds, mixed widths, or a gap pass.
//  - copy-routine veto: distinct roots that scalarize to the same value
//    are per-case windows of byte-dispatch code (CRT memcpy switches
//    inttoptr the same argument in every case); all of them drop.
//  - alloca windows: every foreign access overlapping the window must be
//    contained in one field or cover the whole window. An access cutting
//    across a field edge means the window was guessed wrong.
//
// Emission: the observed window [Lo, Hi) becomes the struct; gaps become
// [N x i8] padding fields. Each member access gets
// `gep %struct.agg.*, base-at-Lo, 0, field` built before it; the old
// pointer chain dies with the rewrite. The layout is checked against the
// DataLayout; a mismatch (ABI padding disagreeing with observed gaps)
// abandons the cluster. Field types keep what the IR observed --
// pointer fields lifted as i64 stay i64; TypeRecovery/ABI passes refine.
bool isStackCookieStore(const StoreInst &SI, const DataLayout &DL) {
  auto *Xor = dyn_cast<BinaryOperator>(SI.getValueOperand());
  if (!Xor || Xor->getOpcode() != Instruction::Xor)
    return false;
  uint64_t DstOff = 0;
  const Value *DstRoot =
      resolveFootprintBase(skipCopies(SI.getPointerOperand()), DL, DstOff);
  if (!DstRoot)
    return false;
  auto IsStackPtr = [](const Value *V) {
    SmallPtrSet<const Value *, 8> Seen;
    while (V && Seen.insert(V).second) {
      if (isa<AllocaInst>(V))
        return true;
      if (const auto *Cast = dyn_cast<CastInst>(V))
        V = Cast->getOperand(0);
      else if (const auto *GEP = dyn_cast<GetElementPtrInst>(V))
        V = GEP->getPointerOperand();
      else
        break;
    }
    return false;
  };
  for (const Value *Operand : Xor->operand_values())
    if (auto *PTI = dyn_cast<PtrToIntInst>(Operand))
      if (IsStackPtr(PTI->getPointerOperand()))
        return true;
  return false;
}

bool rebuildFieldClusters(Function &F, const DataLayout &DL) {
  Module *M = F.getParent();
  if (!M)
    return false;
  LLVMContext &Ctx = F.getContext();

  struct Access {
    Instruction *Inst = nullptr;
    Value *Ptr = nullptr;
    const Value *Root = nullptr;
    uint64_t Off = 0;
    uint64_t Width = 0;
    Type *Ty = nullptr;
    unsigned Kind = 0;
    unsigned Bits = 0;
    bool Store = false;
    bool Member = false; // scalar access that can join a cluster
  };
  SmallVector<Access, 32> Accesses;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      Value *Ptr = nullptr;
      Type *Ty = nullptr;
      bool IsStore = false;
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->isVolatile() || LI->isAtomic())
          continue;
        Ptr = LI->getPointerOperand();
        Ty = LI->getType();
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->isVolatile() || SI->isAtomic() ||
            isStackCookieStore(*SI, DL))
          continue;
        Ptr = SI->getPointerOperand();
        Ty = SI->getValueOperand()->getType();
        IsStore = true;
      } else {
        continue;
      }
      // An access that already walks a struct-typed GEP carries the
      // pre-IR's own type; it is neither evidence nor foreign.
      bool StructStep = false;
      for (const Value *V = Ptr; auto *GEP = dyn_cast<GetElementPtrInst>(V);
           V = GEP->getPointerOperand())
        if (GEP->getSourceElementType()->isStructTy()) {
          StructStep = true;
          break;
        }
      if (StructStep)
        continue;
      uint64_t Off = 0;
      const Value *Root = resolveFootprintBase(skipCopies(Ptr), DL, Off);
      if (!Root || !Root->getType()->isPointerTy())
        continue;
      Root = skipCopies(Root);
      Access A;
      A.Inst = &I;
      A.Ptr = Ptr;
      A.Root = Root;
      A.Off = Off;
      A.Store = IsStore;
      A.Ty = Ty;
      A.Width = DL.getTypeStoreSize(Ty);
      // i128+ is an SSE spill slot in lifted code, not a C field: wide
      // integers may be foreign accesses but never join a cluster.
      if (accessKindBits(Ty, A.Kind, A.Bits) && A.Width > 0 &&
          !(Ty->isIntegerTy() && Ty->getIntegerBitWidth() > 64))
        A.Member = true;
      Accesses.push_back(A);
    }
  }
  if (Accesses.empty())
    return false;

  // Group access indices per root, first-seen order for determinism.
  DenseMap<const Value *, SmallVector<unsigned, 8>> ByRoot;
  SmallVector<const Value *, 8> RootOrder;
  for (unsigned i = 0, e = Accesses.size(); i < e; ++i) {
    const Value *R = Accesses[i].Root;
    auto It = ByRoot.find(R);
    if (It == ByRoot.end()) {
      RootOrder.push_back(R);
      ByRoot[R].push_back(i);
    } else {
      It->second.push_back(i);
    }
  }

  // Copy-routine veto: roots sharing one scalarized producer are
  // per-case windows, not objects.
  DenseMap<const Value *, unsigned> ScalarUsers;
  SmallVector<const Value *, 8> RootScalar;
  for (const Value *R : RootOrder) {
    const Value *S = isa<Argument>(R) ? R : scalarizePtrRoot(R);
    RootScalar.push_back(S);
    ++ScalarUsers[S];
  }

  auto eligibleRoot = [](const Value *Root) -> bool {
    if (const auto *AI = dyn_cast<AllocaInst>(Root)) {
      Type *AT = AI->getAllocatedType();
      if (auto *Arr = dyn_cast<ArrayType>(AT))
        return Arr->getElementType()->isIntegerTy(8);
      return AT->isIntegerTy(8) || AT->isStructTy();
    }
    if (isa<Constant>(Root))
      return false;
    return isa<Instruction>(Root) || isa<Argument>(Root);
  };

  bool Changed = false;
  unsigned Synth = 0;
  for (size_t RI = 0, RE = RootOrder.size(); RI < RE; ++RI) {
    const Value *Root = RootOrder[RI];
    if (ScalarUsers[RootScalar[RI]] > 1 || !eligibleRoot(Root))
      continue;
    const SmallVector<unsigned, 8> &Idxs = ByRoot[Root];

    // Cluster fields keyed by offset: scalar members only, unanimous
    // type per offset.
    struct Field {
      uint64_t Off = 0;
      uint64_t Width = 0;
      Type *Ty = nullptr;
      SmallVector<unsigned, 4> Idx;
    };
    SmallVector<Field, 8> Fields;
    SmallVector<unsigned, 8> Demoted; // member accesses demoted to foreign
    {
      SmallVector<unsigned, 8> MemberIdx;
      for (unsigned I : Idxs)
        if (Accesses[I].Member)
          MemberIdx.push_back(I);
      // Wider first at each offset: the widest access claims the field,
      // narrower same-offset reads (an i8 peek into an i32) demote.
      llvm::sort(MemberIdx, [&](unsigned A, unsigned B) {
        if (Accesses[A].Off != Accesses[B].Off)
          return Accesses[A].Off < Accesses[B].Off;
        return Accesses[A].Width > Accesses[B].Width;
      });
      bool Veto = false;
      for (size_t k = 0; k < MemberIdx.size();) {
        size_t kEnd = k;
        uint64_t Off = Accesses[MemberIdx[k]].Off;
        while (kEnd < MemberIdx.size() &&
               Accesses[MemberIdx[kEnd]].Off == Off)
          ++kEnd;
        const Access &A0 = Accesses[MemberIdx[k]];
        // Same-offset disagreement: the widest access owns the field,
        // narrower ones demote to foreign peeks.
        SmallVector<unsigned, 4> GroupIdx;
        bool GroupVeto = false;
        for (size_t m = k; m < kEnd; ++m) {
          const Access &A = Accesses[MemberIdx[m]];
          if (A.Ty == A0.Ty) {
            GroupIdx.push_back(MemberIdx[m]);
            continue;
          }
          if (A.Width < A0.Width) {
            Demoted.push_back(MemberIdx[m]);
            continue;
          }
          GroupVeto = true; // disagreeing wider-than-owner type
          break;
        }
        if (GroupVeto || GroupIdx.empty()) {
          Veto = true;
          break;
        }
        // Overlap with earlier fields: contained or whole-field overlay
        // demotes the whole group; an edge cut vetoes the cluster.
        uint64_t GEnd = Off + A0.Width;
        bool Overlaps = !Fields.empty() && Off < Fields.back().Off +
                                                  Fields.back().Width;
        if (Overlaps) {
          bool InsideSome = false, Covers = true;
          for (const Field &Fd : Fields) {
            uint64_t FS = Fd.Off, FE = Fd.Off + Fd.Width;
            if (GEnd <= FS || Off >= FE)
              continue;
            if (Off >= FS && GEnd <= FE)
              InsideSome = true;
            else if (!(Off <= FS && GEnd >= FE))
              Covers = false; // edge cut
          }
          if (!Covers && !InsideSome) {
            Veto = true;
            break;
          }
          for (unsigned I : GroupIdx)
            Demoted.push_back(I);
        } else {
          Field Fd;
          Fd.Off = Off;
          Fd.Width = A0.Width;
          Fd.Ty = A0.Ty;
          Fd.Idx = GroupIdx;
          Fields.push_back(std::move(Fd));
        }
        k = kEnd;
      }
      for (unsigned I : Demoted)
        Accesses[I].Member = false;
      if (Veto)
        continue;
    }
    if (Fields.size() < 2)
      continue;
    bool Veto = false;
    uint64_t Lo = Fields.front().Off;
    uint64_t Hi = Fields.back().Off + Fields.back().Width;
    // A large byte-array frame is a set of unrelated local objects. Carve
    // small neighboring runs rather than treating the entire frame as one
    // aggregate; only touch fields in the chosen run.
    if (isa<AllocaInst>(Root)) {
      size_t BestStart = 0, BestEnd = 0;
      auto ConfirmsRun = [&](size_t Start, size_t End) {
        for (BasicBlock &CallBB : F)
          for (Instruction &CallI : CallBB) {
            auto *CB = dyn_cast<CallBase>(&CallI);
            Function *Callee = CB ? CB->getCalledFunction() : nullptr;
            if (!Callee || Callee->isDeclaration())
              continue;
            for (unsigned AI = 0; AI < CB->arg_size(); ++AI) {
              uint64_t CallOff = 0;
              if (resolveFootprintBase(skipCopies(CB->getArgOperand(AI)),
                                       DL, CallOff) != Root ||
                  CallOff != Fields[Start].Off || AI >= Callee->arg_size())
                continue;
              Argument *CA = Callee->getArg(AI);
              bool AllSeen = true;
              for (size_t FI = Start; FI < End; ++FI) {
                const Field &Fd = Fields[FI];
                bool Seen = false;
                for (BasicBlock &CBB : *Callee)
                  for (Instruction &CI : CBB) {
                    Value *Ptr = nullptr;
                    Type *AT = nullptr;
                    if (auto *LI = dyn_cast<LoadInst>(&CI)) {
                      Ptr = LI->getPointerOperand();
                      AT = LI->getType();
                    } else if (auto *SI = dyn_cast<StoreInst>(&CI)) {
                      Ptr = SI->getPointerOperand();
                      AT = SI->getValueOperand()->getType();
                    }
                    if (!Ptr || !AT)
                      continue;
                    bool SameStorage =
                        AT == Fd.Ty ||
                        (DL.getTypeStoreSize(AT) == Fd.Width &&
                         DL.getTypeStoreSize(Fd.Ty) == Fd.Width &&
                         ((AT->isPointerTy() && Fd.Ty->isIntegerTy(64)) ||
                          (Fd.Ty->isPointerTy() &&
                           AT->isIntegerTy(64))));
                    if (!SameStorage)
                      continue;
                    uint64_t AccessOff = 0;
                    if (resolveFootprintBase(skipCopies(Ptr), DL,
                                             AccessOff) == CA &&
                        AccessOff == Fd.Off - Fields[Start].Off) {
                      Seen = true;
                      if (AT->isPointerTy())
                        Fields[FI].Ty = AT;
                    }
                  }
                AllSeen &= Seen;
              }
              if (AllSeen)
                return true;
            }
          }
        return false;
      };
      for (size_t Start = 0; Start < Fields.size();) {
        size_t End = Start + 1;
        while (End < Fields.size() &&
               Fields[End].Off - (Fields[End - 1].Off +
                                   Fields[End - 1].Width) <= 16)
          ++End;
        if (End - Start > BestEnd - BestStart &&
            ConfirmsRun(Start, End)) {
          BestStart = Start;
          BestEnd = End;
        }
        Start = End;
      }
      if (BestEnd == BestStart)
        continue;
      SmallVector<Field, 8> Local;
      for (size_t I = BestStart; I < BestEnd; ++I)
        Local.push_back(std::move(Fields[I]));
      Fields = std::move(Local);
    }
    Lo = Fields.front().Off;
    Hi = Fields.back().Off + Fields.back().Width;
    if (Hi - Lo > 256)
      continue;
    if (isa<AllocaInst>(Root)) {
      bool CalleeConfirms = false;
      for (BasicBlock &CallBB : F)
        for (Instruction &CallI : CallBB) {
          auto *CB = dyn_cast<CallBase>(&CallI);
          Function *Callee = CB ? CB->getCalledFunction() : nullptr;
          if (!Callee || Callee->isDeclaration())
            continue;
          for (unsigned AI = 0; AI < CB->arg_size(); ++AI) {
            uint64_t CallOff = 0;
            const Value *CallRoot = resolveFootprintBase(
                skipCopies(CB->getArgOperand(AI)), DL, CallOff);
            if (CallRoot != Root || CallOff != Lo ||
                AI >= Callee->arg_size())
              continue;
            Argument *CA = Callee->getArg(AI);
            bool AllFieldsSeen = true;
            for (const Field &Fd : Fields) {
              bool Seen = false;
              for (BasicBlock &CBB : *Callee)
                for (Instruction &CI : CBB) {
                  Value *Ptr = nullptr;
                  Type *AT = nullptr;
                  if (auto *LI = dyn_cast<LoadInst>(&CI)) {
                    Ptr = LI->getPointerOperand();
                    AT = LI->getType();
                  } else if (auto *SI = dyn_cast<StoreInst>(&CI)) {
                    Ptr = SI->getPointerOperand();
                    AT = SI->getValueOperand()->getType();
                  }
                  if (!Ptr || AT != Fd.Ty)
                    continue;
                  uint64_t AccessOff = 0;
                  if (resolveFootprintBase(skipCopies(Ptr), DL, AccessOff) ==
                          CA &&
                      AccessOff == Fd.Off - Lo)
                    Seen = true;
                }
              AllFieldsSeen &= Seen;
            }
            CalleeConfirms |= AllFieldsSeen;
          }
        }
      if (!CalleeConfirms)
        continue;
    }
    if (!isa<AllocaInst>(Root)) {
      auto *RA = dyn_cast<Argument>(Root);
      if (!RA)
        continue;
      unsigned ConfirmingCallers = 0;
      for (Function &Caller : *M)
        if (!Caller.isDeclaration())
          for (BasicBlock &CallerBB : Caller)
            for (Instruction &CI : CallerBB) {
              auto *CB = dyn_cast<CallBase>(&CI);
              if (!CB || CB->getCalledFunction() != &F ||
                  RA->getArgNo() >= CB->arg_size())
                continue;
              uint64_t BaseOff = 0;
              const Value *Base = resolveFootprintBase(
                  skipCopies(CB->getArgOperand(RA->getArgNo())), DL, BaseOff);
              if (!Base)
                continue;
              bool AllSeen = true;
              for (const Field &Fd : Fields) {
                bool Seen = false;
                for (BasicBlock &CallerBlock : Caller)
                  for (Instruction &AccessI : CallerBlock) {
                    Value *Ptr = nullptr;
                    Type *AccessTy = nullptr;
                    if (auto *LI = dyn_cast<LoadInst>(&AccessI)) {
                      Ptr = LI->getPointerOperand();
                      AccessTy = LI->getType();
                    } else if (auto *SI = dyn_cast<StoreInst>(&AccessI)) {
                      Ptr = SI->getPointerOperand();
                      AccessTy = SI->getValueOperand()->getType();
                    }
                    if (!Ptr || AccessTy != Fd.Ty)
                      continue;
                    uint64_t Off = 0;
                    if (resolveFootprintBase(skipCopies(Ptr), DL, Off) ==
                            Base &&
                        Off == BaseOff + Fd.Off - Lo)
                      Seen = true;
                  }
                AllSeen &= Seen;
              }
              ConfirmingCallers += AllSeen;
            }
      if (ConfirmingCallers < 1)
        continue;
    }
    // Struct-likeness: veto a dense uniform run of >=3 offsets (an
    // unrolled array) and an adjacent same-type pair.
    if (Fields.size() >= 3) {
      const Field &A = Fields[0];
      bool Uniform = true;
      for (size_t k = 1; k < Fields.size(); ++k) {
        if (Fields[k].Off != Fields[k - 1].Off + Fields[k - 1].Width ||
            Fields[k].Ty != A.Ty) {
          Uniform = false;
          break;
        }
      }
      if (Uniform)
        continue;
    } else {
      const Field &A = Fields[0], &Bf = Fields[1];
      if (Bf.Off == A.Off + A.Width && A.Ty == Bf.Ty)
        continue;
    }
    // Alloca windows: foreign accesses (non-scalar or demoted members)
    // must not cut a field edge. Each is tolerated only when fully
    // inside one field (a byte peek into a field) or a union of whole
    // fields (an i64 overlay of two adjacent i32 fields).
    if (isa<AllocaInst>(Root)) {
      for (unsigned I : Idxs) {
        const Access &A = Accesses[I];
        bool IsMember = false;
        for (const Field &Fd : Fields)
          if (Fd.Off == A.Off && Fd.Ty == A.Ty && A.Member)
            IsMember = true;
        if (IsMember)
          continue;
        uint64_t AEnd = A.Off + std::max<uint64_t>(A.Width, 1);
        if (AEnd <= Lo || A.Off >= Hi)
          continue;
        bool Ok = false;
        for (const Field &Fd : Fields) {
          uint64_t FS = Fd.Off, FE = Fd.Off + Fd.Width;
          if (AEnd <= FS || A.Off >= FE)
            continue;
          if (A.Off >= FS && AEnd <= FE) {
            Ok = true; // inside one field
            break;
          }
          if ((A.Off > FS && A.Off < FE) || (AEnd > FS && AEnd < FE)) {
            Ok = false; // edge cut
            break;
          }
          Ok = true; // covers this field whole; keep checking
        }
        if (!Ok) {
          Veto = true;
          break;
        }
      }
      if (Veto)
        continue;
    }

    // Prefer an existing module struct whose layout reproduces the
    // observed window exactly: every observed (offset, type) must be a
    // field, and no field may start inside the window without being
    // observed (a missed member means the cluster is a different
    // struct). Only when nothing matches do we synthesize
    // `struct.agg.<fn>.<n>`: observed fields plus [N x i8] gap padding.
    SmallVector<Type *, 8> Elems;
    DenseMap<uint64_t, unsigned> OffToIdx;
    Type *I8 = Type::getInt8Ty(Ctx);
    StructType *ST = nullptr;
    bool Reused = false;
    for (StructType *Cand : M->getIdentifiedStructTypes()) {
      if (Cand->isOpaque() || Cand->isPacked())
        continue;
      const StructLayout *CL = DL.getStructLayout(Cand);
      bool Match = true;
      for (unsigned i = 0, e = Cand->getNumElements(); i < e; ++i) {
        uint64_t CO = CL->getElementOffset(i);
        if (CO < Lo || CO >= Hi)
          continue;
        bool Seen = false;
        for (const Field &Fd : Fields)
          if (Fd.Off == CO && Fd.Ty == Cand->getElementType(i))
            Seen = true;
        if (!Seen) {
          Match = false;
          break;
        }
      }
      if (!Match)
        continue;
      bool AllSeen = true;
      for (const Field &Fd : Fields) {
        bool Seen = false;
        for (unsigned i = 0, e = Cand->getNumElements(); i < e; ++i)
          if (CL->getElementOffset(i) == Fd.Off &&
              Cand->getElementType(i) == Fd.Ty)
            Seen = true;
        if (!Seen) {
          AllSeen = false;
          break;
        }
      }
      if (!AllSeen)
        continue;
      ST = Cand;
      Reused = true;
      for (unsigned i = 0, e = ST->getNumElements(); i < e; ++i)
        for (const Field &Fd : Fields)
          if (CL->getElementOffset(i) == Fd.Off)
            OffToIdx[Fd.Off] = i;
      break;
    }
    if (!ST) {
      uint64_t Cursor = Lo;
      for (const Field &Fd : Fields) {
        if (Fd.Off > Cursor)
          Elems.push_back(ArrayType::get(I8, Fd.Off - Cursor));
        OffToIdx[Fd.Off] = Elems.size();
        Elems.push_back(Fd.Ty);
        Cursor = Fd.Off + Fd.Width;
      }
      ST = StructType::create(
          Ctx, Elems,
          "struct.agg." + (F.getName() + "." + Twine(Synth++)).str());
      const StructLayout *SL = DL.getStructLayout(ST);
      bool LayoutOk = true;
      for (const Field &Fd : Fields)
        if (SL->getElementOffset(OffToIdx[Fd.Off]) != Fd.Off - Lo) {
          LayoutOk = false;
          break;
        }
      if (!LayoutOk)
        continue;
    }

    // Base addressing the struct start: for a reused type the fields
    // are absolute offsets off the root, so the base is the root
    // itself; for a synthesized window [Lo, Hi) it is root + Lo (a
    // shared byte GEP placed right after the root).
    Value *BaseAtLo = const_cast<Value *>(Root);
    if (!Reused && Lo != 0) {
      Instruction *IP = nullptr;
      if (auto *RI = dyn_cast<Instruction>(BaseAtLo))
        if (std::optional<BasicBlock::iterator> OptIP =
                RI->getInsertionPointAfterDef())
          IP = &**OptIP;
      if (!IP)
        IP = &*F.getEntryBlock().getFirstInsertionPt();
      IRBuilder<> LB(IP);
      auto *LG = cast<GetElementPtrInst>(
          LB.CreateGEP(I8, BaseAtLo, LB.getInt64(Lo), "rv.agg.base"));
      if (auto *RI = dyn_cast<Instruction>(Root))
        copyVA(RI, LG);
      BaseAtLo = LG;
    }

    // Retitle each member access to a field GEP; dead address chains go
    // with it.
    for (const Field &Fd : Fields) {
      for (unsigned I : Fd.Idx) {
        Access &A = Accesses[I];
        IRBuilder<> AB(A.Inst);
        auto *Typed = cast<GetElementPtrInst>(AB.CreateGEP(
            ST, BaseAtLo,
            {AB.getInt32(0), AB.getInt32(OffToIdx[Fd.Off])}, "rv.field"));
        // Preserve the original pointer's nowrap flags (the lifted IR
        // marks frame and object GEPs `inbounds nuw`).
        if (auto *PG = dyn_cast<GetElementPtrInst>(A.Ptr))
          Typed->setNoWrapFlags(PG->getNoWrapFlags());
        if (auto *PI = dyn_cast<Instruction>(A.Ptr))
          copyVA(PI, Typed);
        else
          copyVA(A.Inst, Typed);
        A.Inst->setOperand(A.Store ? 1 : 0, Typed);
        eraseDeadAddressChain(A.Ptr);
      }
    }
    Changed = true;
  }
  return Changed;
}


// Rebuild split aggregate values: insertvalue chains that were scalarized
// into per-field SSA plus extractvalue uses. When a call/return/insertvalue
// web carries every field of a struct value as scalars that all feed one
// aggregate consumer (or vice versa), rebuild the aggregate value at the
// boundary. Conservative core: rebuild extractvalue(load aggregate)
// pairs that instcombine turned into scalar loads from byte GEPs is
// covered by rebuild C; here handle byval-arg promotion leftovers: a call
// whose consecutive scalar args all come from adjacent field loads of one
// base, where the callee takes the aggregate by value, regains the
// aggregate load. Since lifted callees rarely keep aggregate signatures,
// the practical rewrite is the reverse direction the corpus shows at O1:
// `load <struct> + extractvalue` fields that each feed scalar uses stay,
// but a store of an aggregate built from scalar inserts becomes one
// aggregate store.
bool rebuildSplitValues(Function &F, const DataLayout &DL) {
  bool Changed = false;
  // Collect candidate stores first; erasing siblings while walking the
  // block would invalidate the iteration.
  SmallVector<StoreInst *, 16> CandStores;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || SI->isVolatile() || SI->isAtomic() || isOpaqueCall(I))
        continue;
      auto *EI = dyn_cast<ExtractValueInst>(SI->getValueOperand());
      if (!EI || EI->getNumIndices() != 1)
        continue;
      Value *Agg = EI->getAggregateOperand();
      auto *AggTy = dyn_cast<StructType>(Agg->getType());
      if (!AggTy || AggTy->isOpaque() || AggTy->getNumElements() < 2)
        continue;
      CandStores.push_back(SI);
    }

  SmallPtrSet<Value *, 8> Done;
  // Stores erased as siblings of an earlier fold stay in the worklist.
  // Tracked by index: eraseFromParent frees the object and LLVM recycles
  // the memory, so a freed pointer must never sit in a pointer-keyed set
  // (DenseMap "Key already in new map" on address reuse).
  SmallVector<bool, 16> DeadStoreIdx(CandStores.size(), false);
  for (size_t SIi = 0, SIE = CandStores.size(); SIi < SIE; ++SIi) {
    StoreInst *SI = CandStores[SIi];
    if (DeadStoreIdx[SIi])
      continue;
    auto *EI = dyn_cast<ExtractValueInst>(SI->getValueOperand());
    if (!EI)
      continue;
    Value *Agg = EI->getAggregateOperand();
    if (!Done.insert(Agg).second)
      continue;
    auto *AggTy = cast<StructType>(Agg->getType());
    unsigned NF = AggTy->getNumElements();
    SmallVector<StoreInst *, 8> Siblings;
    SmallVector<unsigned, 8> Idx;
    for (User *U : Agg->users()) {
      auto *E2 = dyn_cast<ExtractValueInst>(U);
      if (!E2 || !E2->hasOneUse() || E2->getNumIndices() != 1)
        continue;
      auto *S2 = dyn_cast<StoreInst>(*E2->user_begin());
      if (!S2 || !S2->getParent() || S2->isVolatile() || S2->isAtomic())
        continue;
      Siblings.push_back(S2);
      Idx.push_back(*E2->idx_begin());
    }
    if (Siblings.size() != NF)
      continue;
    SmallVector<FieldLayout, 8> Fields;
    if (!structFieldLayout(AggTy, DL, Fields))
      continue;
    bool Covers = true;
    SmallVector<bool, 8> Seen(NF, false);
    const Value *DstBase = nullptr;
    for (size_t k = 0; k < Siblings.size(); ++k) {
      unsigned FL = Idx[k];
      if (FL >= NF || Seen[FL]) {
        Covers = false;
        break;
      }
      Seen[FL] = true;
      auto *GEP =
          dyn_cast<GetElementPtrInst>(Siblings[k]->getPointerOperand());
      if (!GEP) {
        Covers = false;
        break;
      }
      APInt BO(DL.getIndexSizeInBits(0), 0);
      if (!GEP->accumulateConstantOffset(DL, BO) ||
          BO.getZExtValue() != Fields[FL].Offset) {
        Covers = false;
        break;
      }
      const Value *B = skipCopies(GEP->getPointerOperand());
      if (!DstBase)
        DstBase = B;
      else if (DstBase != B) {
        Covers = false;
        break;
      }
    }
    for (bool S : Seen)
      Covers = Covers && S;
    if (!Covers || !DstBase)
      continue;
    StoreInst *First = Siblings.front();
    for (StoreInst *S2 : Siblings)
      if (S2->comesBefore(First))
        First = S2;
    // Destination must be the aggregate start: a struct-typed pointer
    // the GEPs were derived from, or an alloca of exactly this struct.
    Value *DstPtr = nullptr;
    if (First->getPointerOperand()->getType() ==
        PointerType::get(F.getContext(), 0)) {
      if (auto *FGEP =
              dyn_cast<GetElementPtrInst>(First->getPointerOperand())) {
        APInt BO(DL.getIndexSizeInBits(0), 0);
        if (FGEP->accumulateConstantOffset(DL, BO) &&
            BO.getZExtValue() == 0)
          DstPtr = FGEP->getPointerOperand();
      } else if (isa<AllocaInst>(skipCopies(First->getPointerOperand()))) {
        DstPtr = const_cast<Value *>(skipCopies(First->getPointerOperand()));
      } else {
        DstPtr = First->getPointerOperand();
      }
    }
    if (!DstPtr) {
      const auto *AI = dyn_cast<AllocaInst>(DstBase);
      if (!AI || AI->getAllocatedType() != AggTy)
        continue;
      DstPtr = const_cast<Value *>(DstBase);
    }
    IRBuilder<> B(First);
    Value *DstTyped = DstPtr;
    if (DstPtr->getType() != PointerType::get(F.getContext(), 0)) {
      DstTyped = B.CreateBitCast(DstPtr, PointerType::get(F.getContext(), 0));
      copyVA(First, cast<Instruction>(DstTyped));
    }
    StoreInst *NS = B.CreateStore(Agg, DstTyped);
    copyVA(First, NS);
    for (StoreInst *S2 : Siblings) {
      auto *E2 = cast<ExtractValueInst>(S2->getValueOperand());
      S2->eraseFromParent();
      for (size_t i = 0, e = CandStores.size(); i < e; ++i)
        if (CandStores[i] == S2)
          DeadStoreIdx[i] = true;
      if (E2->use_empty())
        E2->eraseFromParent();
    }
  }
  return Changed;
}

} // namespace

PreservedAnalyses UnsroaPass::run(Function &F, FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  Module *M = F.getParent();
  if (!M)
    return PreservedAnalyses::all();
  const DataLayout &DL = M->getDataLayout();

  bool Changed = false;
  // Order: regroup scalar allocas first (creates byte GEPs), synthesize
  // field-cluster structs (writes the struct types rebuilder B's
  // aggregate evidence reads), fold proven copy runs, retitle byte GEPs
  // to struct GEPs (needs module struct types), then rebuild split
  // aggregate values.
  // Debug gate (no functional effect): RV_UNSROA_DISABLE=groups,copies,
  // geps,values,clusters skips that rebuilder; arr,fld skip only the
  // array- or struct-GEP emission inside rebuildTypedGeps.
  unsigned Skip = 0;
  if (const char *Dis = std::getenv("RV_UNSROA_DISABLE")) {
    StringRef S(Dis);
    if (S.contains("groups"))
      Skip |= 1;
    if (S.contains("copies"))
      Skip |= 2;
    if (S.contains("geps"))
      Skip |= 4;
    if (S.contains("values"))
      Skip |= 8;
    if (S.contains("arr"))
      Skip |= 16;
    if (S.contains("fld"))
      Skip |= 32;
    if (S.contains("clusters"))
      Skip |= 64;
  }
  if (!(Skip & 1))
    Changed |= rebuildScalarGroups(F, DL);
  if (!(Skip & 64))
    Changed |= rebuildFieldClusters(F, DL);
  if (!(Skip & 2))
    Changed |= rebuildCopyRuns(F, DL);
  if (!(Skip & 4))
    Changed |= rebuildTypedGeps(F, DL, Skip);
  if (!(Skip & 8))
    Changed |= rebuildSplitValues(F, DL);

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`, same convention as SsaOut:
// compiled only for the standalone DLL so the static revenant-opt link
// never exports llvmGetPassPluginInfo twice.
#ifdef REVENANT_BUILD_PLUGIN
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantTransformUnsroa",
          LLVM_VERSION_STRING, [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-unsroa") {
                    PM.addPass(revenant::UnsroaPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
