//===- TypeRecovery.cpp - Recovered types/names as DI metadata -*- C++ -*-===//
//
// Part of the Revenant project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements `revenant-types`: run TypeInference over the function, then
// emit one DILocalVariable per named alloca/argument plus struct types for
// multi-offset pointer bases. All DI work goes through RevenantIR
// (`revenant/IR/Contract.h`); the build fails with #error until that header
// lands so there is exactly one writer of CU/subprogram/variable helpers.
//
// Naming: allocas/arguments keep their IR names with the leading `rv.`
// prefix (if any) stripped; empty names become `v<line>`/`a<index>`.
// Integer type names follow the reference C spelling (`int32_t`,
// `uint64_t`, ...). Structs are named `rv_struct_<func>_<base>` with
// members `field_<offset>`; a single-offset base stays a scalar pointer.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/TypeRecovery/TypeRecovery.h"

#if !__has_include("revenant/IR/Contract.h")
#error "revenant-types needs RevenantIR: wait for RvIRContract to land revenant/IR/Contract.h"
#endif

#include "revenant/IR/Contract.h"

#include "TypeInference.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace revenant {
namespace {

StringRef displayName(StringRef N) {
  // SROA/mem2reg names look like `x.addr`, `p.0`, `rv.tmp3`: keep them, the
  // frontend reads them as-is. Only strip a `rv.` producer prefix.
  if (N.consume_front("rv."))
    return N;
  return N;
}

std::string intTypeName(unsigned Bits, bool Unsigned) {
  // Matches IrTy::c(signed) in the reference: i1 is bool.
  if (Bits == 1)
    return "bool";
  return (Unsigned ? "uint" : "int") + utostr(Bits) + "_t";
}

DIType *intDIType(DIBuilder &DIB, unsigned Bits, bool Unsigned) {
  unsigned Encoding =
      Bits == 1 ? dwarf::DW_ATE_boolean
                : Unsigned ? dwarf::DW_ATE_unsigned : dwarf::DW_ATE_signed;
  return ir::createBasicType(DIB, intTypeName(Bits, Unsigned), Bits,
                             Encoding);
}

DIType *floatDIType(DIBuilder &DIB, unsigned Bits) {
  return ir::createBasicType(DIB, Bits == 32 ? "float" : "double", Bits,
                             dwarf::DW_ATE_float);
}

DIType *diTypeForInteger(DIBuilder &DIB, TypeInference &TI,
                         const Value *V, unsigned DefaultBits) {
  unsigned Bits = TI.intBits(V, DefaultBits);
  if (Bits == 0)
    Bits = DefaultBits;
  // Clamp to real LLVM integer widths the frontend can spell.
  if (Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64) {
    Bits = Bits < 8 ? 8 : Bits < 16 ? 16 : Bits < 32 ? 32 : 64;
  }
  return intDIType(DIB, Bits, TI.isUnsigned(V));
}

// Build the pointee for a pointer base: a struct when two or more distinct
// field offsets were observed, else the scalar access type (or an opaque
// byte when nothing was observed).
DIType *pointeeForBase(DIBuilder &DIB, DIScope *Scope, DIFile *File,
                       Function &F, TypeInference &TI, const Value *Base,
                       const TypeInference::FieldAccess *FA) {
  if (!FA || FA->Fields.empty()) {
    // No field evidence: opaque byte pointee (reference Unknown(8)).
    DIType *Byte = intDIType(DIB, 8, /*Unsigned=*/true);
    return Byte;
  }
  if (FA->Fields.size() == 1) {
    // Single scalar access: pointee is the access type directly.
    auto It = FA->Fields.begin();
    uint64_t SizeBits = It->second.SizeBits;
    // Signedness comes from the loaded/stored value, which may sit behind
    // a GEP: check loads/stores on the base itself and on GEPs rooted at
    // it, plus float-typed loads for the float case.
    bool Unsigned = false;
    bool Float = false;
    unsigned FloatBits = 32;
    SmallVector<const Value *, 4> Ptrs{Base};
    for (const User *U : Base->users()) {
      if (const auto *GEP = dyn_cast<GetElementPtrInst>(U))
        Ptrs.push_back(GEP);
    }
    bool Done = false;
    for (const Value *P : Ptrs) {
      for (const User *U : P->users()) {
        if (const auto *LI = dyn_cast<LoadInst>(U)) {
          Unsigned = TI.isUnsigned(LI);
          if (LI->getType()->isFloatTy()) {
            Float = true;
            FloatBits = LI->getType()->getPrimitiveSizeInBits();
          }
          Done = true;
          break;
        }
        if (const auto *SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() == P) {
            Unsigned = TI.isUnsigned(SI->getValueOperand());
            Done = true;
            break;
          }
        }
      }
      if (Done)
        break;
    }
    if (Float)
      return floatDIType(DIB, FloatBits);
    if (SizeBits == 32 || SizeBits == 64) {
      unsigned Bits = SizeBits <= 32 ? 32 : 64;
      return intDIType(DIB, Bits, Unsigned);
    }
    if (SizeBits == 8 || SizeBits == 16)
      return intDIType(DIB, (unsigned)SizeBits, Unsigned);
    return intDIType(DIB, 8, true);
  }

  // Multi-offset: one DICompositeType with a member per offset. Each
  // member's signedness comes from the values accessed through a GEP
  // rooted at this base (loads/stores behind a constant GEP vote on
  // their own value web at collection time).
  SmallVector<DIDerivedType *, 8> Members;
  for (const auto &KV : FA->Fields) {
    uint64_t OffBits = KV.first;
    uint64_t SizeBits = KV.second.SizeBits;
    unsigned Bits = SizeBits <= 8    ? 8
                    : SizeBits <= 16 ? 16
                    : SizeBits <= 32 ? 32
                                     : 64;
    bool Unsigned = false;
    for (const User *U : Base->users()) {
      const auto *GEP = dyn_cast<GetElementPtrInst>(U);
      if (!GEP)
        continue;
      for (const User *GU : GEP->users()) {
        if (const auto *LI = dyn_cast<LoadInst>(GU)) {
          Unsigned = TI.isUnsigned(LI);
          break;
        }
        if (const auto *SI = dyn_cast<StoreInst>(GU)) {
          if (SI->getPointerOperand() == GEP) {
            Unsigned = TI.isUnsigned(SI->getValueOperand());
            break;
          }
        }
      }
      break;
    }
    DIType *FieldTy = intDIType(DIB, Bits, Unsigned);
    std::string Name = "field_" + utostr(OffBits / 8);
    DIDerivedType *M = DIB.createMemberType(
        Scope, Name, File, 1, Bits, Bits, OffBits, DINode::FlagZero, FieldTy);
    Members.push_back(M);
  }
  std::string StructName;
  {
    raw_string_ostream OS(StructName);
    OS << "rv_struct_" << F.getName() << "_";
    if (Base->hasName())
      OS << displayName(Base->getName());
    else
      OS << "anon";
  }
  uint64_t SizeBits = FA->MaxEndBits;
  SmallVector<Metadata *, 8> Elems;
  for (DIDerivedType *M : Members)
    Elems.push_back(M);
  return ir::createStructType(DIB, Scope, StructName, SizeBits, Elems);
}

} // namespace

PreservedAnalyses TypeRecoveryPass::run(Function &F,
                                        FunctionAnalysisManager &AM) {
  (void)AM;
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  Module *M = F.getParent();
  if (!M)
    return PreservedAnalyses::all();

  TypeInference TI(F);
  TI.collect();

  // CU plumbing lives in RevenantIR: one shared compile unit per module.
  DIBuilder DIB(*M);
  DICompileUnit &CU = ir::getOrCreateCompileUnit(*M, DIB, "revenant.ll");
  DISubprogram *SP = F.getSubprogram();
  if (!SP)
    SP = &ir::createSubprogramFor(F, DIB, CU);
  DIFile *File = SP->getFile();
  DIScope *Scope = SP;

  bool Changed = false;

  // Arguments: recovered names + inferred types as parameter variables.
  unsigned ArgNo = 1;
  for (Argument &A : F.args()) {
    std::string Name = A.hasName() ? displayName(A.getName()).str()
                                   : ("a" + utostr(A.getArgNo()));
    DIType *Ty = nullptr;
    Type *AT = A.getType();
    if (AT->isPointerTy()) {
      const TypeInference::FieldAccess *FA = TI.fieldsFor(&A);
      DIType *Pointee = pointeeForBase(DIB, Scope, File, F, TI, &A, FA);
      Ty = ir::createPointerType(DIB, Pointee);
    } else if (AT->isIntegerTy()) {
      Ty = diTypeForInteger(DIB, TI, &A, AT->getIntegerBitWidth());
    } else if (AT->isFloatTy()) {
      Ty = floatDIType(DIB, AT->getPrimitiveSizeInBits());
    } else {
      continue;
    }
    DILocalVariable *Var =
        ir::createLocalVariable(DIB, Scope, Name, Ty, ArgNo);
    // Attach to the alloca that spills this argument when one exists;
    // otherwise the declare lands on the argument itself via dbg.value.
    bool Attached = false;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          // Heuristic: `name.addr` spills `name`.
          if (AI->hasName() &&
              displayName(AI->getName()) == Name + ".addr") {
            ir::declareLocal(F, *AI, Name, Ty, DIB, CU);
            Attached = true;
            Changed = true;
            break;
          }
        }
      }
      if (Attached)
        break;
    }
    if (!Attached) {
      // No spill slot for this argument: create the parameter variable so
      // the name survives, and point it at the argument with a dbg.value
      // at the entry block.
      const DILocation *DL = nullptr;
      if (Instruction *Pos = F.getEntryBlock().getFirstNonPHI())
        DL = Pos->getDebugLoc().get();
      if (!DL)
        DL = DILocation::get(M->getContext(), 1, 1, Scope);
      DIB.insertDbgValueIntrinsic(&A, Var, DIB.createExpression(), DL,
                                  &F.getEntryBlock());
      Changed = true;
    }
    ++ArgNo;
  }

  // Alloca locals: one auto variable each with the recovered type. Argument
  // spill slots (`name.addr`) are skipped: the parameter entry above already
  // describes the slot, so a second variable would double-describe it.
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;
      if (AI->hasName()) {
        StringRef N = displayName(AI->getName());
        if (N.consume_back(".addr")) {
          bool IsArg = false;
          for (const Argument &A : F.args()) {
            if (A.hasName() && displayName(A.getName()) == N) {
              IsArg = true;
              break;
            }
          }
          if (IsArg)
            continue;
        }
      }
      std::string Name = AI->hasName() ? displayName(AI->getName()).str()
                                       : ("v" + utostr(BB.getNumber()));
      Type *AllocTy = AI->getAllocatedType();
      DIType *Ty = nullptr;
      if (AllocTy->isIntegerTy()) {
        Ty = diTypeForInteger(DIB, TI, AI, AllocTy->getIntegerBitWidth());
      } else if (AllocTy->isFloatTy()) {
        Ty = floatDIType(DIB, AllocTy->getPrimitiveSizeInBits());
      } else if (AllocTy->isPointerTy()) {
        const TypeInference::FieldAccess *FA = TI.fieldsFor(AI);
        DIType *Pointee = pointeeForBase(DIB, Scope, File, F, TI, AI, FA);
        Ty = ir::createPointerType(DIB, Pointee);
      } else if (AllocTy->isArrayTy()) {
        // Fixed-size buffer: describe element type, keep the count in the
        // name comment via the variable (DI has no direct alloca array
        // spelling through the thin wrappers; element pointer suffices).
        Type *ET = AllocTy->getArrayElementType();
        DIType *Elem = nullptr;
        if (ET->isIntegerTy())
          Elem = intDIType(DIB, ET->getIntegerBitWidth(),
                           TI.isUnsigned(AI));
        else if (ET->isFloatTy())
          Elem = floatDIType(DIB, ET->getPrimitiveSizeInBits());
        else if (ET->isPointerTy())
          Elem = ir::createPointerType(DIB, intDIType(DIB, 8, true));
        else
          Elem = intDIType(DIB, 8, true);
        Ty = ir::createPointerType(DIB, Elem);
      } else if (AllocTy->isStructTy()) {
        // Source-level struct alloca (rare in lifted code): describe each
        // element access through field inference when present, else keep
        // the struct shape with byte members.
        const TypeInference::FieldAccess *FA = TI.fieldsFor(AI);
        DIType *Pointee = pointeeForBase(DIB, Scope, File, F, TI, AI, FA);
        Ty = ir::createPointerType(DIB, Pointee);
      } else {
        continue;
      }
      ir::declareLocal(F, *AI, Name, Ty, DIB, CU);
      Changed = true;
    }
  }

  // Pointer bases that are not allocas (inttoptr results, GEP chains):
  // describe their struct shape once via a retained type so the frontend
  // can name field accesses. No variable to attach; the type is reachable
  // through the alloca/argument that roots the base.
  for (const auto &KV : TI.bases()) {
    const Value *Base = KV.first;
    if (isa<AllocaInst>(Base) || isa<Argument>(Base))
      continue; // Already described above.
    if (KV.second.Fields.size() < 2)
      continue;
    DIType *Pointee =
        pointeeForBase(DIB, Scope, File, F, TI, Base, &KV.second);
    DIType *Ptr = ir::createPointerType(DIB, Pointee);
    DIB.retainType(Ptr);
    Changed = true;
  }

  if (!Changed)
    return PreservedAnalyses::all();
  // Only debug metadata changed; code, CFG, and all revenant.* metadata
  // (`!revenant.region`, `!revenant.va`, `!revenant.opaque`) are untouched.
  DIB.finalize();
  return PreservedAnalyses::none();
}

} // namespace revenant
