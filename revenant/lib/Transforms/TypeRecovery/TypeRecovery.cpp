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
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"

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

DIType *diTypeForAccess(DIBuilder &DIB, TypeInference &TI,
                        const TypeInference::FieldCell &Cell) {
  const Value *Access = Cell.Access;
  if (!Access)
    return intDIType(DIB, 8, /*Unsigned=*/true);
  Type *Ty = Access->getType();
  if (Ty->isIntegerTy())
    return diTypeForInteger(DIB, TI, Access, Ty->getIntegerBitWidth());
  if (Ty->isFloatTy())
    return floatDIType(DIB, Ty->getPrimitiveSizeInBits());
  if (Ty->isPointerTy())
    return ir::createPointerType(
        DIB, intDIType(DIB, 8, /*Unsigned=*/true));
  unsigned Bits = Cell.SizeBits <= 8    ? 8
                  : Cell.SizeBits <= 16 ? 16
                  : Cell.SizeBits <= 32 ? 32
                                        : 64;
  return intDIType(DIB, Bits, TI.isUnsigned(Access));
}

// Build the pointee for a pointer base: a struct when two or more distinct
// field offsets were observed, else the scalar access type (or an opaque
// byte when nothing was observed).
DIType *pointeeForBase(DIBuilder &DIB, DIScope *Scope, DIFile *File,
                       Function &F, TypeInference &TI, const Value *Base,
                       const TypeInference::FieldAccess *FA) {
  // Module normalization shares one layout across direct callers. Keep its
  // named LLVM struct and DI schema together instead of creating a second,
  // function-local layout from the subset of fields observed here.
  if (Base->hasUseList()) {
    for (const User *U : Base->users())
      if (const auto *G = dyn_cast<GetElementPtrInst>(U))
        if (MDNode *MD = G->getMetadata("revenant.pointee"))
          if (MD->getNumOperands() == 1)
            if (auto *Ty = dyn_cast<DICompositeType>(MD->getOperand(0)))
              return Ty;
  }
  if (!FA) {
    DIType *Byte = intDIType(DIB, 8, /*Unsigned=*/true);
    return Byte;
  }
  if (FA->Fields.empty()) {
    if (FA->HasDynamicAccess)
      return diTypeForAccess(DIB, TI, FA->DynamicAccess);
    return intDIType(DIB, 8, /*Unsigned=*/true);
  }
  if (FA->Fields.size() == 1) {
    // A dynamic scalar access is stronger evidence than a lone constant
    // offset when the base also serves as an array.
    if (FA->HasDynamicAccess)
      return diTypeForAccess(DIB, TI, FA->DynamicAccess);
    return diTypeForAccess(DIB, TI, FA->Fields.begin()->second);
  }

  // Multi-offset: one DICompositeType with a member per offset. The widest
  // access at each offset supplies that member's recovered type.
  SmallVector<DIDerivedType *, 8> Members;
  for (const auto &KV : FA->Fields) {
    uint64_t OffBits = KV.first;
    const TypeInference::FieldCell &Cell = KV.second;
    DIType *FieldTy = diTypeForAccess(DIB, TI, Cell);
    std::string Name = "field_" + utostr(OffBits / 8);
    DIDerivedType *M = DIB.createMemberType(
        Scope, Name, File, 1, Cell.SizeBits, Cell.SizeBits, OffBits,
        DINode::FlagZero, FieldTy);
    Members.push_back(M);
  }

  std::string StructName;
  {
    raw_string_ostream OS(StructName);
    OS << "rv_struct_" << F.getName() << "_";
    if (Base->hasName())
      OS << displayName(Base->getName());
    else if (const auto *A = dyn_cast<Argument>(Base))
      OS << "arg" << A->getArgNo();
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

  // CU plumbing lives in RevenantIR: one shared compile unit per module. The
  // builder starts over the module's existing CU so finalize() has one.
  DICompileUnit *ExistingCU = nullptr;
  if (NamedMDNode *CUs = M->getNamedMetadata("llvm.dbg.cu"))
    if (CUs->getNumOperands() > 0)
      ExistingCU = dyn_cast<DICompileUnit>(CUs->getOperand(0));
  DIBuilder DIB(*M, /*AllowUnresolved=*/true, ExistingCU);
  DICompileUnit &CU = ir::getOrCreateCompileUnit(*M, DIB, "revenant.ll");
  DISubprogram *SP = F.getSubprogram();
  if (!SP)
    SP = &ir::createSubprogramFor(F, DIB, CU);
  DIFile *File = SP->getFile();
  DIScope *Scope = SP;

  bool Changed = false;

  // Keep the subprogram signature aligned with the recovered IR types. The
  // return type occupies slot zero in DWARF's subroutine type array.
  auto signatureType = [&](Type *Ty, const Value *V) -> DIType * {
    if (Ty->isIntegerTy()) {
      // A zero-extension of an ordered comparison yields 0/1, but the
      // comparison predicate—not the zext—carries signedness evidence for
      // the function's declared integer result.
      if (const auto *ZExt = dyn_cast_or_null<ZExtInst>(V))
        if (const auto *Cmp = dyn_cast<ICmpInst>(ZExt->getOperand(0)))
          if (Cmp->isSigned() || Cmp->isUnsigned())
            return intDIType(DIB, TI.intBits(V, Ty->getIntegerBitWidth()),
                             Cmp->isUnsigned());
      if (V)
        return diTypeForInteger(DIB, TI, V, Ty->getIntegerBitWidth());
      return intDIType(DIB, Ty->getIntegerBitWidth(), /*Unsigned=*/false);
    }
    if (Ty->isFloatTy())
      return floatDIType(DIB, Ty->getPrimitiveSizeInBits());
    if (Ty->isPointerTy()) {
      const TypeInference::FieldAccess *FA = V ? TI.fieldsFor(V) : nullptr;
      DIType *Pointee =
          V ? pointeeForBase(DIB, Scope, File, F, TI, V, FA)
            : intDIType(DIB, 8, /*Unsigned=*/true);
      return ir::createPointerType(DIB, Pointee);
    }
    return intDIType(DIB, 8, /*Unsigned=*/true);
  };
  SmallVector<Metadata *, 8> SignatureTypes;
  Type *ReturnTy = F.getReturnType();
  if (ReturnTy->isVoidTy()) {
    SignatureTypes.push_back(nullptr);
  } else {
    const Value *ReturnValue = nullptr;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (const auto *RI = dyn_cast<ReturnInst>(&I))
          if (RI->getReturnValue()) {
            ReturnValue = RI->getReturnValue();
            break;
          }
      }
      if (ReturnValue)
        break;
    }
    SignatureTypes.push_back(signatureType(ReturnTy, ReturnValue));
  }
  for (Argument &A : F.args())
    SignatureTypes.push_back(signatureType(A.getType(), &A));
  DISubroutineType *Signature = ir::createSubroutineType(DIB, SignatureTypes);
  if (SP->getType() != Signature) {
    SP->replaceType(Signature);
    Changed = true;
  }

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
      BasicBlock &Entry = F.getEntryBlock();
      BasicBlock::iterator Pos = Entry.getFirstNonPHIIt();
      const DILocation *DL = nullptr;
      if (Pos != Entry.end())
        DL = Pos->getDebugLoc().get();
      if (!DL)
        DL = DILocation::get(M->getContext(), 1, 1, Scope);
      DIB.insertDbgValueIntrinsic(&A, Var, DIB.createExpression(), DL, Pos);
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
        const DataLayout &DL = M->getDataLayout();
        Metadata *Subscript = DIB.getOrCreateSubrange(
            0, static_cast<int64_t>(cast<ArrayType>(AllocTy)->getNumElements()));
        DINodeArray Subscripts = DIB.getOrCreateArray({Subscript});
        uint64_t SizeBits = DL.getTypeAllocSizeInBits(AllocTy).getFixedValue();
        uint32_t AlignBits = DL.getABITypeAlign(AllocTy).value() * 8;
        Ty = DIB.createArrayType(SizeBits, AlignBits, Elem, Subscripts);
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

  // Pointer bases that are not allocas (inttoptr results, GEP chains) get no
  // retained type. Retaining one per function made every later DIBuilder copy
  // and deduplicate the whole retained list in finalize(), which is quadratic
  // in function count, and nothing reads the list.

  // The verifier wants a !dbg location on every call in a function that has a
  // DISubprogram; lifted calls only carry !revenant.va.
  DILocation *CallLoc = DILocation::get(F.getContext(), 1, 1, SP);
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (isa<CallBase>(I) && !I.getDebugLoc()) {
        I.setDebugLoc(CallLoc);
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
