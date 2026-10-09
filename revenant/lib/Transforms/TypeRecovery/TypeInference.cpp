//===- TypeInference.cpp - Signedness/pointer/struct inference -*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pure inference over one LLVM function: vote collection and type choice.
// No DIBuilder use here; the caller turns the results into metadata.
//
// Signedness mirrors the reference structurer (`crates/structure`):
// `mark_unsigned` pre-scans for unsigned compares and marks both operands
// unsigned, and expression emission joins signedness with unsigned sticky
// (one unsigned witness wins; signed witnesses only matter when nothing
// voted unsigned). Evidence sources, in priority order:
//
//   1. ICmp predicate on an operand: unsigned predicate votes unsigned,
//      signed predicate votes signed. Eq/Ne abstain.
//   2. Unsigned integer arithmetic: UDiv/URem/LShr vote their LHS unsigned;
//      AShr votes its LHS signed.
//   3. Casts: ZExt votes its result unsigned-flavored (zero extension is
//      the unsigned widen), SExt votes its result signed. Trunc keeps the
//      source vote. SIToFP/FPToSI vote signed, UIToFP/FPToUI vote unsigned.
//   4. Loads: the loaded type's own vote (a load of i32 feeds i32 evidence).
//
// Union-find over copies (same-representation bitcasts, selects, phis, and
// zero-offset GEPs): a vote on any web member applies to the whole web, so
// declaration order never changes the outcome.
//
// Pointers mirror `def_var`/`expr_ty` in the reference: a value defined by
// Alloca or IntToPtr, or observed only through loads/stores/GEPs/calls that
// take it by address, is a pointer. Its pointee starts as the element type
// of the defining operation and joins across uses (widest integer wins;
// integer vs float conflict falls back to a byte).
//
// Structs come from constant-offset field access patterns: GEPs with all
// constant indices (via accumulateConstantOffset) plus constant-offset
// load/store users through one base pointer each contribute (offset, size,
// access type). Two or more distinct offsets make a struct; a single offset
// stays a scalar pointer.
//
//===----------------------------------------------------------------------===//

#include "TypeInference.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

using namespace llvm;

namespace revenant {

static const Value *skipCopies(const Value *V) {
  const Value *Cur = V;
  SmallPtrSet<const Value *, 8> Seen;
  while (Seen.insert(Cur).second) {
    const Value *Next = nullptr;
    if (const auto *BC = dyn_cast<BitCastInst>(Cur))
      Next = BC->getOperand(0);
    else if (const auto *SI = dyn_cast<SelectInst>(Cur)) {
      // Transparent only when both arms agree after skipping; handled by the
      // union-find in the caller, here just stop.
      (void)SI;
      break;
    } else if (const auto *GEP = dyn_cast<GetElementPtrInst>(Cur)) {
      if (GEP->hasAllZeroIndices())
        Next = GEP->getPointerOperand();
      else
        break;
    } else {
      break;
    }
    if (!Next || Next == Cur)
      break;
    // Only skip pointer-preserving copies; int<->ptr casts change the vote
    // domain and are handled explicitly.
    if (Next->getType()->isPointerTy() != Cur->getType()->isPointerTy())
      break;
    Cur = Next;
  }
  return Cur;
}

void TypeInference::noteUse(Value *V) {
  const Value *K = skipCopies(V);
  if (const auto *A = dyn_cast<Argument>(K)) {
    if (A->getParent() == &F)
      ensureValue(V);
    return;
  }
  if (const auto *I = dyn_cast<Instruction>(K)) {
    if (I->getParent() && I->getFunction() == &F)
      ensureValue(V);
  }
}

TypeInference::Id TypeInference::ensureValue(const Value *V) {
  auto It = Index.find(V);
  if (It != Index.end())
    return It->second;
  Id N = static_cast<Id>(Nodes.size());
  Index[V] = N;
  Nodes.push_back(Node(V));
  Parent.push_back(N);
  return N;
}

TypeInference::Id TypeInference::find(Id N) const {
  Id R = N;
  while (Parent[R] != R)
    R = Parent[R];
  while (Parent[N] != N) {
    Id Next = Parent[N];
    Parent[N] = R;
    N = Next;
  }
  return R;
}

void TypeInference::unite(const Value *A, const Value *B) {
  Id X = find(ensureValue(A));
  Id Y = find(ensureValue(B));
  if (X == Y)
    return;
  // Union by order: keep the smaller root for determinism.
  if (X > Y)
    std::swap(X, Y);
  Parent[Y] = X;
  Node &Dst = Nodes[X];
  const Node &Src = Nodes[Y];
  Dst.UnsignedVotes += Src.UnsignedVotes;
  Dst.SignedVotes += Src.SignedVotes;
  Dst.PointerVotes += Src.PointerVotes;
  Dst.IntVotes += Src.IntVotes;
  Dst.FloatVotes += Src.FloatVotes;
  if (Src.MaxIntBits > Dst.MaxIntBits)
    Dst.MaxIntBits = Src.MaxIntBits;
  if (Src.MaxFloatBits > Dst.MaxFloatBits)
    Dst.MaxFloatBits = Src.MaxFloatBits;
}

void TypeInference::voteUnsigned(const Value *V) {
  if (!V)
    return;
  noteUse(const_cast<Value *>(V));
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return;
  Nodes[find(It->second)].UnsignedVotes++;
}

void TypeInference::voteSigned(const Value *V) {
  if (!V)
    return;
  noteUse(const_cast<Value *>(V));
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return;
  Nodes[find(It->second)].SignedVotes++;
}

void TypeInference::votePointer(const Value *V) {
  if (!V || !V->getType()->isPointerTy())
    return;
  noteUse(const_cast<Value *>(V));
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return;
  Nodes[find(It->second)].PointerVotes++;
}

void TypeInference::voteIntOf(const Value *V) {
  if (!V)
    return;
  Type *T = V->getType();
  if (T->isIntegerTy())
    voteInt(V, T->getIntegerBitWidth());
  else if (T->isPointerTy())
    votePointer(V);
}

void TypeInference::voteInt(const Value *V, unsigned Bits) {
  if (!V)
    return;
  noteUse(const_cast<Value *>(V));
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return;
  Node &N = Nodes[find(It->second)];
  N.IntVotes++;
  N.MaxIntBits = std::max(N.MaxIntBits, Bits);
}

void TypeInference::voteFloat(const Value *V, unsigned Bits) {
  if (!V)
    return;
  noteUse(const_cast<Value *>(V));
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return;
  Node &N = Nodes[find(It->second)];
  N.FloatVotes++;
  N.MaxFloatBits = std::max(N.MaxFloatBits, Bits);
}

static bool isTransparentCopy(const Instruction *I) {
  switch (I->getOpcode()) {
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
    return I->getOperand(0)->getType()->isPointerTy() ==
           I->getType()->isPointerTy();
  case Instruction::Select:
  case Instruction::PHI:
    return true;
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    return false;
  default:
    return false;
  }
}

void TypeInference::collect() {
  // Seed: every argument, alloca, and instruction result gets a node so
  // votes always have a home.
  for (Argument &A : F.args())
    ensureValue(&A);
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (!I.getType()->isVoidTy())
        ensureValue(&I);
      for (Value *Op : I.operands()) {
        if (isa<Instruction>(Op) || isa<Argument>(Op))
          noteUse(Op);
      }
      // Transparent copies share one web.
      if (isTransparentCopy(&I))
        unite(&I, I.getOperand(0));
      if (const auto *SI = dyn_cast<SelectInst>(&I)) {
        unite(&I, SI->getTrueValue());
        unite(&I, SI->getFalseValue());
      }
      if (const auto *PN = dyn_cast<PHINode>(&I)) {
        for (Value *Inc : PN->incoming_values())
          if (isa<Instruction>(Inc) || isa<Argument>(Inc))
            unite(&I, Inc);
      }
      // Loads alias their pointer for signedness: the loaded value's type
      // is the memory's type, so votes on the load read the slot's type.
      // (Stores vote on the stored value, not the slot.)
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        unite(&I, LI->getPointerOperand());
    }
  }

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // Integer compares vote on both operands; Eq/Ne abstain.
      if (const auto *CI = dyn_cast<ICmpInst>(&I)) {
        if (CI->isUnsigned()) {
          voteUnsigned(CI->getOperand(0));
          voteUnsigned(CI->getOperand(1));
        } else if (CI->isSigned()) {
          voteSigned(CI->getOperand(0));
          voteSigned(CI->getOperand(1));
        }
        voteIntOf(CI->getOperand(0));
        voteIntOf(CI->getOperand(1));
        continue;
      }
      // Unsigned/signed arithmetic votes on the LHS (mirrors the
      // unsigned-arith cast insertion in the reference emitter).
      if (I.getOpcode() == Instruction::UDiv ||
          I.getOpcode() == Instruction::URem ||
          I.getOpcode() == Instruction::LShr) {
        voteUnsigned(I.getOperand(0));
        voteIntOf(&I);
        continue;
      }
      if (I.getOpcode() == Instruction::AShr ||
          I.getOpcode() == Instruction::SDiv ||
          I.getOpcode() == Instruction::SRem) {
        voteSigned(I.getOperand(0));
        voteIntOf(&I);
        continue;
      }
      if (const auto *CI = dyn_cast<CastInst>(&I)) {
        switch (CI->getOpcode()) {
        case Instruction::ZExt:
          voteUnsigned(&I);
          voteIntOf(&I);
          break;
        case Instruction::SExt:
          voteSigned(&I);
          voteIntOf(&I);
          break;
        case Instruction::Trunc:
          // Keeps the source web (already united when transparent enough);
          // still records width evidence on the result.
          voteIntOf(&I);
          break;
        case Instruction::SIToFP:
        case Instruction::FPToSI:
          voteSigned(CI->getOperand(0));
          break;
        case Instruction::UIToFP:
        case Instruction::FPToUI:
          voteUnsigned(CI->getOperand(0));
          break;
        case Instruction::IntToPtr:
          votePointer(&I);
          voteIntOf(CI->getOperand(0));
          break;
        case Instruction::PtrToInt:
          voteIntOf(&I);
          votePointer(CI->getOperand(0));
          break;
        case Instruction::BitCast:
          if (I.getType()->isPointerTy())
            votePointer(&I);
          else if (I.getType()->isIntegerTy())
            voteInt(&I, I.getType()->getIntegerBitWidth());
          else if (I.getType()->isFloatTy())
            voteFloat(&I, I.getType()->getPrimitiveSizeInBits());
          break;
        default:
          break;
        }
        continue;
      }
      if (const auto *LI = dyn_cast<LoadInst>(&I)) {
        votePointer(LI->getPointerOperand());
        Type *ET = LI->getType();
        if (ET->isIntegerTy())
          voteInt(&I, ET->getIntegerBitWidth());
        else if (ET->isFloatTy())
          voteFloat(&I, ET->getPrimitiveSizeInBits());
        else if (ET->isPointerTy())
          votePointer(&I);
        recordFieldAccess(LI->getPointerOperand(), 0, true, LI);
        // Field access through a GEP: attribute the load to the GEP's base
        // at the GEP's constant offset.
        if (const auto *GEP =
                dyn_cast<GetElementPtrInst>(LI->getPointerOperand())) {
          const DataLayout &DL = F.getParent()->getDataLayout();
          APInt Off(64, 0);
          if (GEP->accumulateConstantOffset(DL, Off))
            recordFieldAccess(GEP->getPointerOperand(), Off.getZExtValue(),
                              false, LI);
        }
        continue;
      }
      if (const auto *SI = dyn_cast<StoreInst>(&I)) {
        votePointer(SI->getPointerOperand());
        Type *VT = SI->getValueOperand()->getType();
        if (VT->isIntegerTy())
          voteInt(SI->getValueOperand(), VT->getIntegerBitWidth());
        else if (VT->isFloatTy())
          voteFloat(SI->getValueOperand(), VT->getPrimitiveSizeInBits());
        else if (VT->isPointerTy())
          votePointer(SI->getValueOperand());
        recordFieldAccess(SI->getPointerOperand(), 0, true, SI);
        if (const auto *GEP =
                dyn_cast<GetElementPtrInst>(SI->getPointerOperand())) {
          const DataLayout &DL = F.getParent()->getDataLayout();
          APInt Off(64, 0);
          if (GEP->accumulateConstantOffset(DL, Off))
            recordFieldAccess(GEP->getPointerOperand(), Off.getZExtValue(),
                              false, SI);
        }
        continue;
      }
      if (const auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        votePointer(GEP->getPointerOperand());
        votePointer(&I);
        // A zero-offset GEP aliases the base for all votes; a nonzero GEP
        // still shares the pointer web but must not carry integer votes
        // across (it addresses a different field).
        if (GEP->hasAllZeroIndices())
          unite(&I, GEP->getPointerOperand());
        continue;
      }
      if (isa<AllocaInst>(&I)) {
        votePointer(&I);
        continue;
      }
      if (isa<CallInst>(&I) || isa<InvokeInst>(&I)) {
        const auto *CB = cast<CallBase>(&I);
        // Opaque asm stubs (`revenant.asm.*`, marked `!revenant.opaque` by
        // the lifter) carry no type evidence; voting on their operands
        // would launder integers into pointers and vice versa.
        if (const Function *Callee = CB->getCalledFunction()) {
          if (Callee->getName().starts_with("revenant.asm.") ||
              Callee->getName().starts_with("llvm.dbg."))
            continue;
        }
        // Address-taken operands (byval/byref/nocapture-readonly pointer
        // args, or any pointer passed where the callee may load) vote
        // pointer; integer args vote int.
        for (unsigned A = 0, E = CB->arg_size(); A < E; ++A) {
          Value *Arg = CB->getArgOperand(A);
          if (Arg->getType()->isPointerTy())
            votePointer(Arg);
          else if (Arg->getType()->isIntegerTy())
            voteInt(Arg, Arg->getType()->getIntegerBitWidth());
          else if (Arg->getType()->isFloatTy())
            voteFloat(Arg, Arg->getType()->getPrimitiveSizeInBits());
        }
        continue;
      }
      // Plain integer ALU keeps width evidence.
      if (I.getType()->isIntegerTy() && I.isBinaryOp())
        voteInt(&I, I.getType()->getIntegerBitWidth());
      else if (I.getType()->isFloatTy() && I.isBinaryOp())
        voteFloat(&I, I.getType()->getPrimitiveSizeInBits());
    }
  }
}

void TypeInference::recordFieldAccess(const Value *Ptr, uint64_t BaseOffset,
                                      bool DirectMemOp, const Instruction *I) {
  const Value *Base = skipCopies(Ptr);
  // Walk through a constant GEP to the ultimate base.
  uint64_t GepOff = 0;
  if (const auto *GEP = dyn_cast<GetElementPtrInst>(Base)) {
    const DataLayout &DL = F.getParent()->getDataLayout();
    APInt Off(64, 0);
    if (GEP->accumulateConstantOffset(DL, Off)) {
      GepOff = Off.getZExtValue();
      Base = skipCopies(GEP->getPointerOperand());
    } else {
      // Non-constant GEP: the base is still a pointer, but no field.
      return;
    }
  }
  if (!DirectMemOp)
    return; // GEP alone is not an access; the load/store records it.

  // Access width from the memory operation; the byte offset is the GEP
  // offset plus the caller's base offset.
  uint64_t SizeBits = 0;
  uint64_t OffBits = (GepOff + BaseOffset) * 8;
  const DataLayout &DL = F.getParent()->getDataLayout();
  if (const auto *LI = dyn_cast<LoadInst>(I)) {
    SizeBits = DL.getTypeStoreSizeInBits(LI->getType());
  } else if (const auto *SI = dyn_cast<StoreInst>(I)) {
    SizeBits = DL.getTypeStoreSizeInBits(SI->getValueOperand()->getType());
  } else {
    return; // GEP alone is not an access; wait for its load/store users.
  }
  if (SizeBits == 0)
    return;

  // Find the base's access record (create on first field access).
  ensureValue(Base);
  FieldAccess &FA = Bases[Base];
  FA.Base = Base;
  auto It = FA.Fields.find(OffBits);
  if (It == FA.Fields.end() || It->second.SizeBits < SizeBits) {
    FA.Fields[OffBits] = FieldCell{SizeBits, I->getDebugLoc().get()};
  }
  FA.MaxEndBits = std::max(FA.MaxEndBits, OffBits + SizeBits);
}

bool TypeInference::isUnsigned(const Value *V) const {
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return false;
  const Node &N = Nodes[find(It->second)];
  // Unsigned-sticky join: any unsigned witness wins (mirrors expr_signed).
  if (N.UnsignedVotes > 0)
    return true;
  return false;
}

bool TypeInference::hasSignedWitness(const Value *V) const {
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return false;
  return Nodes[find(It->second)].SignedVotes > 0;
}

bool TypeInference::isPointer(const Value *V) const {
  if (!V->getType()->isPointerTy())
    return false;
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return true; // No votes: trust the IR type.
  const Node &N = Nodes[find(It->second)];
  // inttoptr results and allocas always voted pointer at collect time.
  // A pointer-typed value with integer votes but no pointer votes is
  // likely an integer masquerading as an address (opaque use); still
  // describe it as a pointer since the IR type is authoritative.
  (void)N;
  return true;
}

unsigned TypeInference::intBits(const Value *V, unsigned Default) const {
  auto It = Index.find(skipCopies(V));
  if (It == Index.end())
    return Default;
  unsigned W = Nodes[find(It->second)].MaxIntBits;
  return W ? W : Default;
}

const TypeInference::FieldAccess *
TypeInference::fieldsFor(const Value *Base) const {
  auto It = Bases.find(skipCopies(Base));
  if (It == Bases.end())
    return nullptr;
  return &It->second;
}

} // namespace revenant
