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
//   B. Scalar copy runs -> memcpy. Adjacent scalar load/store pairs moving
//      the same widths between two bases fold into one llvm.memcpy. The
//      widths/offsets must tile both sides contiguously from offset 0 of
//      the moved range.
//   C. Byte GEPs -> typed struct GEPs. A byte GEP whose base carries a
//      known struct type in the module and whose offset lands exactly on a
//      field start becomes the typed field GEP the pre-IR had. This is the
//      c-clang-o2 `struct_access` rewrite: `gep i8, ptr %0, i64 4` with
//      `%struct.Point = type { i32, i32, ptr }` in the module becomes
//      `gep %struct.Point, ptr %0, i32 0, i32 1`.
//
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
                             uint64_t &Off) -> Type * {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
        APInt ByteOff(DL.getIndexSizeInBits(0), 0);
        // Peel one constant GEP layer; the base may itself be a GEP
        // (test IR threads field GEPs through byte GEPs). Only the
        // outermost constant offset matters for contiguity.
        if (GEP->accumulateConstantOffset(DL, ByteOff)) {
          Base = skipCopies(GEP->getPointerOperand());
          Off = ByteOff.getZExtValue();
          return GEP->getSourceElementType();
        }
        return nullptr;
      }
      // Direct base: an alloca, argument, or global at offset 0.
      const Value *B = skipCopies(Ptr);
      if (auto *AI = dyn_cast<AllocaInst>(B)) {
        Base = AI;
        Off = 0;
        return AI->getAllocatedType();
      }
      if (isa<Argument>(B) || isa<GlobalValue>(B)) {
        Base = B;
        Off = 0;
        return nullptr;
      }
      return nullptr;
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
      Module *M = F.getParent();
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
      Type *STE = baseAndOffset(LI->getPointerOperand(), SB, SO);
      Type *DTE = baseAndOffset(SI->getPointerOperand(), DB, DO);
      (void)STE;
      (void)DTE;
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

  // GEPs erased as chain links of an earlier fold stay in the worklist:
  // skip them by pointer (never dereference first).
  SmallPtrSet<GetElementPtrInst *, 16> DeadGeps;
  for (GetElementPtrInst *GEP : Geps) {
    if (DeadGeps.contains(GEP))
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
          DeadGeps.insert(GEP);
          // Drop peeled inner links that died with the fold,
          // outermost first so uses cascade.
          for (GetElementPtrInst *C : Chain)
            if (C->use_empty()) {
              C->eraseFromParent();
              DeadGeps.insert(C);
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
    // Emit the typed field GEP in front of the byte GEP.
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
      DeadGeps.insert(GEP);
      for (GetElementPtrInst *C : Chain)
        if (C->use_empty()) {
          C->eraseFromParent();
          DeadGeps.insert(C);
        }
      Changed = true;
    }
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
  // Stores erased as siblings of an earlier fold stay in the worklist:
  // skip them by pointer before dereferencing.
  SmallPtrSet<StoreInst *, 16> DeadStores;
  for (StoreInst *SI : CandStores) {
    if (DeadStores.contains(SI))
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
      DeadStores.insert(S2);
      if (E2->use_empty())
        E2->eraseFromParent();
    }
    Changed = true;
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
  // Order: regroup scalar allocas first (creates byte GEPs), fold copy
  // runs (needs scalar pairs), retitle byte GEPs to struct GEPs (needs
  // module struct types), then rebuild split aggregate values.
  // Debug gate (no functional effect): RV_UNSROA_DISABLE=groups,copies,
  // geps,values skips that rebuilder; arr,fld skip only the array- or
  // struct-GEP emission inside rebuildTypedGeps. Used once to bisect a
  // crash; the committed tests cover everything enabled.
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
  }
  if (!(Skip & 1))
    Changed |= rebuildScalarGroups(F, DL);
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
