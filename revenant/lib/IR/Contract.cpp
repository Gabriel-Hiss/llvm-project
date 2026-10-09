//===- revenant/lib/IR/Contract.cpp - IR contract read/write --------------===//
//
// See docs/ir-contract.md for the contract this implements.
//
//===----------------------------------------------------------------------===//

#include "revenant/IR/Contract.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace revenant::ir;

namespace {

std::string hexVA(uint64_t VA) { return utohexstr(VA, /*LowerCase=*/true); }

/// Parse "0x<hex>" or bare "<hex>" into VA.
std::optional<uint64_t> parseVA(StringRef S) {
  StringRef Digits = S.starts_with("0x") ? S.drop_front(2) : S;
  if (Digits.empty())
    return std::nullopt;
  uint64_t VA = 0;
  if (Digits.getAsInteger(16, VA))
    return std::nullopt;
  return VA;
}

/// A `!{i64 VA}` node -> VA.
std::optional<uint64_t> extractVA(const MDNode *N) {
  if (!N || N->getNumOperands() != 1)
    return std::nullopt;
  auto *CI = mdconst::dyn_extract<ConstantInt>(N->getOperand(0));
  if (!CI || !CI->getType()->isIntegerTy(64))
    return std::nullopt;
  return CI->getZExtValue();
}

MDNode *vaNode(LLVMContext &C, uint64_t VA) {
  Metadata *Op =
      ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(C), VA));
  return MDNode::get(C, Op);
}

const char *kindName(RegionKind K) {
  switch (K) {
  case RegionKind::If:      return "if";
  case RegionKind::SC:      return "sc";
  case RegionKind::While:   return "while";
  case RegionKind::DoWhile: return "dowhile";
  case RegionKind::For:     return "for";
  case RegionKind::Loop:    return "loop";
  case RegionKind::Switch:  return "switch";
  case RegionKind::Goto:    return "goto";
  }
  llvm_unreachable("bad RegionKind");
}

std::optional<RegionKind> parseKind(StringRef S) {
  if (S == "if")      return RegionKind::If;
  if (S == "sc")      return RegionKind::SC;
  if (S == "while")   return RegionKind::While;
  if (S == "dowhile") return RegionKind::DoWhile;
  if (S == "for")     return RegionKind::For;
  if (S == "loop")    return RegionKind::Loop;
  if (S == "switch")  return RegionKind::Switch;
  if (S == "goto")    return RegionKind::Goto;
  return std::nullopt;
}

unsigned operandCount(RegionKind K) {
  return isLoopRegionKind(K) ? 3 : 1;
}

/// Parse one descriptor node `!{!"kind", i32 ...}`.
std::optional<RegionAnnotation> parseDesc(const MDNode *N) {
  if (!N)
    return std::nullopt;
  auto *S = dyn_cast_or_null<MDString>(N->getOperand(0));
  if (!S)
    return std::nullopt;
  auto Kind = parseKind(S->getString());
  if (!Kind)
    return std::nullopt;
  RegionAnnotation A;
  A.Kind = *Kind;
  if (N->getNumOperands() != 1 + operandCount(*Kind))
    return std::nullopt;
  int32_t *Out[] = {&A.A, &A.B, &A.C};
  for (unsigned I = 0; I < operandCount(*Kind); ++I) {
    auto *CI = mdconst::dyn_extract<ConstantInt>(N->getOperand(I + 1));
    if (!CI || !CI->getType()->isIntegerTy(32))
      return std::nullopt;
    *Out[I] = static_cast<int32_t>(CI->getSExtValue());
  }
  return A;
}

MDNode *descNode(LLVMContext &C, RegionKind Kind, ArrayRef<int32_t> Indices) {
  SmallVector<Metadata *, 4> Ops;
  Ops.push_back(MDString::get(C, kindName(Kind)));
  Type *I32 = Type::getInt32Ty(C);
  for (int32_t I : Indices)
    Ops.push_back(ConstantAsMetadata::get(ConstantInt::get(I32, I)));
  return MDNode::get(C, Ops);
}

bool isDbgIntrinsicCall(const Instruction &I) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return false;
  const Function *Callee = CB->getCalledFunction();
  return Callee && Callee->getIntrinsicID() != Intrinsic::not_intrinsic &&
         Callee->getName().starts_with("llvm.dbg.");
}

std::string printableInst(const Instruction &I) {
  std::string S;
  raw_string_ostream OS(S);
  I.print(OS);
  return S;
}

} // namespace

//===----------------------------------------------------------------------===//
// Module-level
//===----------------------------------------------------------------------===//

StringRef revenant::ir::langName(Lang L) {
  switch (L) {
  case Lang::C:    return "c";
  case Lang::CPP:  return "cpp";
  case Lang::Rust: return "rust";
  }
  llvm_unreachable("bad Lang");
}

void revenant::ir::setModuleLang(Module &M, Lang L) {
  M.setModuleFlag(Module::Override, ModuleFlagLang,
                  MDString::get(M.getContext(), langName(L)));
}

std::optional<Lang> revenant::ir::getModuleLang(const Module &M) {
  Metadata *V = M.getModuleFlag(ModuleFlagLang);
  auto *S = dyn_cast_or_null<MDString>(V);
  if (!S)
    return std::nullopt;
  StringRef T = S->getString();
  if (T == "c")    return Lang::C;
  if (T == "cpp")  return Lang::CPP;
  if (T == "rust") return Lang::Rust;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Functions and blocks
//===----------------------------------------------------------------------===//

void revenant::ir::setFunctionVA(Function &F, uint64_t VA) {
  F.addFnAttr(FnAttrVA, "0x" + hexVA(VA));
}

std::optional<uint64_t> revenant::ir::getFunctionVA(const Function &F) {
  if (!F.hasFnAttribute(FnAttrVA))
    return std::nullopt;
  return parseVA(F.getFnAttribute(FnAttrVA).getValueAsString());
}

void revenant::ir::nameBlock(BasicBlock &BB, uint64_t VA) {
  BB.setName("bb_" + hexVA(VA));
}

void revenant::ir::nameBlock(BasicBlock &BB) {
  const Function *F = BB.getParent();
  unsigned Idx = 0;
  for (const BasicBlock &B : *F) {
    if (&B == &BB)
      break;
    ++Idx;
  }
  BB.setName("bb_x" + utostr(Idx));
}

std::optional<uint64_t> revenant::ir::getBlockVA(const BasicBlock &BB) {
  StringRef N = BB.getName();
  if (!N.starts_with("bb_") || N.starts_with("bb_x"))
    return std::nullopt;
  return parseVA(N.drop_front(3));
}

//===----------------------------------------------------------------------===//
// Instruction provenance
//===----------------------------------------------------------------------===//

void revenant::ir::setInstructionVA(Instruction &I, uint64_t VA) {
  I.setMetadata(MDKindVA, vaNode(I.getContext(), VA));
}

std::optional<uint64_t> revenant::ir::getInstructionVA(const Instruction &I) {
  return extractVA(I.getMetadata(MDKindVA));
}

void revenant::ir::copyInstructionVA(const Instruction &Src, Instruction &Dst) {
  Dst.setMetadata(MDKindVA, Src.getMetadata(MDKindVA));
}

void revenant::ir::setGlobalVA(GlobalVariable &GV, uint64_t VA) {
  GV.setMetadata(MDKindVA, vaNode(GV.getContext(), VA));
}

std::optional<uint64_t> revenant::ir::getGlobalVA(const GlobalVariable &GV) {
  return extractVA(GV.getMetadata(MDKindVA));
}

//===----------------------------------------------------------------------===//
// Region annotations
//===----------------------------------------------------------------------===//

bool revenant::ir::isLoopRegionKind(RegionKind K) {
  return K == RegionKind::While || K == RegionKind::DoWhile ||
         K == RegionKind::For || K == RegionKind::Loop;
}

StringRef revenant::ir::regionKindName(RegionKind K) { return kindName(K); }

void revenant::ir::setRegionAnnotation(Instruction &Term, RegionKind Kind,
                                       ArrayRef<int32_t> Indices) {
  assert(Indices.size() == operandCount(Kind));
  Term.setMetadata(MDKindRegion,
                   descNode(Term.getContext(), Kind, Indices));
}

void revenant::ir::addRegionAnnotation(Instruction &Term, RegionKind Kind,
                                       ArrayRef<int32_t> Indices) {
  assert(Indices.size() == operandCount(Kind));
  LLVMContext &C = Term.getContext();
  MDNode *New = descNode(C, Kind, Indices);
  MDNode *Old = Term.getMetadata(MDKindRegion);
  if (!Old) {
    Term.setMetadata(MDKindRegion, New);
    return;
  }
  // Fold to list form, keeping existing descriptors in order.
  SmallVector<Metadata *, 4> Ops;
  if (Old->getNumOperands() > 0 && isa<MDNode>(Old->getOperand(0))) {
    for (const MDOperand &Op : Old->operands())
      Ops.push_back(Op.get());
  } else {
    Ops.push_back(Old);
  }
  Ops.push_back(New);
  Term.setMetadata(MDKindRegion, MDNode::get(C, Ops));
}

std::optional<RegionAnnotation>
revenant::ir::getRegionAnnotation(const Instruction &I) {
  MDNode *N = I.getMetadata(MDKindRegion);
  if (!N)
    return std::nullopt;
  if (N->getNumOperands() > 0 && isa<MDNode>(N->getOperand(0)))
    return parseDesc(cast<MDNode>(N->getOperand(0)));
  return parseDesc(N);
}

SmallVector<RegionAnnotation, 2>
revenant::ir::getRegionAnnotations(const Instruction &I) {
  SmallVector<RegionAnnotation, 2> Out;
  MDNode *N = I.getMetadata(MDKindRegion);
  if (!N)
    return Out;
  if (N->getNumOperands() > 0 && isa<MDNode>(N->getOperand(0))) {
    for (const MDOperand &Op : N->operands())
      if (auto A = parseDesc(cast_or_null<MDNode>(Op.get())))
        Out.push_back(*A);
    return Out;
  }
  if (auto A = parseDesc(N))
    Out.push_back(*A);
  return Out;
}

void revenant::ir::clearRegionAnnotation(Instruction &I) {
  I.setMetadata(MDKindRegion, nullptr);
}

void revenant::ir::remapRegionIndices(Function &F,
                                      function_ref<int32_t(int32_t)> Map) {
  LLVMContext &C = F.getContext();
  for (BasicBlock &BB : F) {
    Instruction *T = BB.getTerminator();
    if (!T->getMetadata(MDKindRegion))
      continue;
    SmallVector<RegionAnnotation, 2> Anns = getRegionAnnotations(*T);
    SmallVector<Metadata *, 4> Ops;
    for (const RegionAnnotation &A : Anns) {
      int32_t Vals[] = {A.A, A.B, A.C};
      SmallVector<int32_t, 3> Idx;
      for (unsigned K = 0; K < operandCount(A.Kind); ++K)
        Idx.push_back(Vals[K] == NoBlockIndex ? NoBlockIndex : Map(Vals[K]));
      Ops.push_back(descNode(C, A.Kind, Idx));
    }
    if (Ops.size() == 1)
      T->setMetadata(MDKindRegion, cast<MDNode>(Ops[0]));
    else
      T->setMetadata(MDKindRegion, MDNode::get(C, Ops));
  }
}

//===----------------------------------------------------------------------===//
// Opaque asm calls
//===----------------------------------------------------------------------===//

bool revenant::ir::isOpaqueAsmCallee(const Function &F) {
  return F.getName().starts_with(OpaqueAsmPrefix);
}

bool revenant::ir::isOpaqueAsmCall(const Instruction &I) {
  return I.getMetadata(MDKindOpaque) != nullptr;
}

Function *revenant::ir::getOrCreateOpaqueAsmFn(Module &M, StringRef Mnemonic,
                                             Type *RetTy,
                                             ArrayRef<Type *> ArgTys) {
  FunctionType *FT = FunctionType::get(RetTy, ArgTys, /*isVarArg=*/false);
  return cast<Function>(
      M.getOrInsertFunction((OpaqueAsmPrefix + Mnemonic.lower()).str(), FT)
          .getCallee());
}

CallInst *revenant::ir::makeOpaqueAsmCall(Module &M, StringRef Mnemonic,
                                        Type *RetTy, ArrayRef<Value *> Args,
                                        Instruction *InsertBefore,
                                        std::optional<uint64_t> VA) {
  SmallVector<Type *, 8> ArgTys;
  for (Value *A : Args)
    ArgTys.push_back(A->getType());
  Function *F = getOrCreateOpaqueAsmFn(M, Mnemonic, RetTy, ArgTys);
  CallInst *Call = CallInst::Create(F, Args, "", InsertBefore->getIterator());
  Call->setMetadata(MDKindOpaque, MDNode::get(M.getContext(), {}));
  if (VA)
    setInstructionVA(*Call, *VA);
  return Call;
}

//===----------------------------------------------------------------------===//
// Debug info
//===----------------------------------------------------------------------===//

static dwarf::SourceLanguage dwLangFor(const Module &M) {
  switch (getModuleLang(M).value_or(Lang::C)) {
  case Lang::C:    return dwarf::DW_LANG_C;
  case Lang::CPP:  return dwarf::DW_LANG_C_plus_plus;
  case Lang::Rust: return dwarf::DW_LANG_Rust;
  }
  llvm_unreachable("bad Lang");
}

DICompileUnit &revenant::ir::getOrCreateCompileUnit(Module &M, DIBuilder &DIB,
                                                    StringRef SourceFileName) {
  if (NamedMDNode *CUs = M.getNamedMetadata("llvm.dbg.cu"))
    if (CUs->getNumOperands() > 0)
      if (auto *CU = dyn_cast<DICompileUnit>(CUs->getOperand(0)))
        return *CU;

  DIFile *F = DIB.createFile(
      SourceFileName.empty() ? StringRef("f_recovered") : SourceFileName,
      "revenant");
  return *DIB.createCompileUnit(dwLangFor(M), F, "revenant",
                                /*isOptimized=*/true, /*Flags=*/"",
                                /*RV=*/0, /*SplitName=*/"",
                                DICompileUnit::FullDebug);
}

DISubprogram &revenant::ir::createSubprogramFor(Function &F, DIBuilder &DIB,
                                              DICompileUnit &CU) {
  DIFile *File = CU.getFile();
  DISubroutineType *Sig = createSubroutineType(DIB, {});
  DISubprogram *SP =
      DIB.createFunction(File, F.getName(), F.getName(), File, /*LineNo=*/1,
                         Sig, /*ScopeLine=*/1, DINode::FlagZero,
                         DISubprogram::SPFlagDefinition |
                             DISubprogram::SPFlagOptimized);
  F.setSubprogram(SP);
  return *SP;
}

DILocalVariable *revenant::ir::createLocalVariable(DIBuilder &DIB,
                                                 DIScope *Scope,
                                                 StringRef Name, DIType *Ty,
                                                 unsigned ArgNo) {
  DIFile *File = Scope ? Scope->getFile() : nullptr;
  if (ArgNo > 0)
    return DIB.createParameterVariable(Scope, Name, ArgNo, File, /*LineNo=*/1,
                                       Ty);
  return DIB.createAutoVariable(Scope, Name, File, /*LineNo=*/1, Ty);
}

void revenant::ir::attachLocalVariable(AllocaInst &Alloca,
                                       DILocalVariable *Var, DIBuilder &DIB,
                                       DIExpression *Expr) {
  LLVMContext &C = Alloca.getContext();
  if (!Expr)
    Expr = DIExpression::get(C, {});
  auto *Scope = dyn_cast_or_null<DILocalScope>(Var->getScope());
  const DILocation *DL = DILocation::get(C, Var->getLine(), /*Col=*/0, Scope);
  DIB.insertDeclare(&Alloca, Var, Expr, DL, std::next(Alloca.getIterator()));
}

DILocalVariable *revenant::ir::declareLocal(Function &F, AllocaInst &Alloca,
                                          StringRef Name, DIType *Ty,
                                          DIBuilder &DIB,
                                          DICompileUnit &CU) {
  DIScope *Scope = F.getSubprogram();
  if (!Scope)
    Scope = &createSubprogramFor(F, DIB, CU);
  DILocalVariable *Var = createLocalVariable(DIB, Scope, Name, Ty);
  attachLocalVariable(Alloca, Var, DIB);
  return Var;
}

DIBasicType *revenant::ir::createBasicType(DIBuilder &DIB, StringRef Name,
                                           uint64_t SizeBits,
                                           unsigned DWARFEncoding) {
  return DIB.createBasicType(Name, SizeBits, DWARFEncoding);
}

DIDerivedType *revenant::ir::createPointerType(DIBuilder &DIB,
                                             DIType *Pointee) {
  return DIB.createPointerType(Pointee, /*SizeInBits=*/64);
}

DICompositeType *revenant::ir::createStructType(DIBuilder &DIB, DIScope *Scope,
                                              StringRef Name,
                                              uint64_t SizeBits,
                                              ArrayRef<Metadata *> Members,
                                              unsigned DWARFTag) {
  DIFile *File = Scope ? Scope->getFile() : nullptr;
  DINodeArray Elements = DIB.getOrCreateArray(Members);
  if (DWARFTag == dwarf::DW_TAG_union_type)
    return DIB.createUnionType(Scope, Name, File, /*LineNo=*/1, SizeBits,
                               /*AlignInBits=*/0, DINode::FlagZero, Elements);
  return DIB.createStructType(Scope, Name, File, /*LineNo=*/1, SizeBits,
                              /*AlignInBits=*/0, DINode::FlagZero,
                              /*DerivedFrom=*/nullptr, Elements);
}

DICompositeType *revenant::ir::createEnumType(
    DIBuilder &DIB, DIScope *Scope, StringRef Name, uint64_t SizeBits,
    ArrayRef<Metadata *> Enumerators) {
  DIFile *File = Scope ? Scope->getFile() : nullptr;
  DINodeArray Elements = DIB.getOrCreateArray(Enumerators);
  return DIB.createEnumerationType(Scope, Name, File, /*LineNo=*/1, SizeBits,
                                   /*AlignInBits=*/0, Elements,
                                   /*UnderlyingType=*/nullptr);
}

DIEnumerator *revenant::ir::createEnumerator(DIBuilder &DIB, StringRef Name,
                                             int64_t Value, bool IsUnsigned) {
  return DIB.createEnumerator(Name, Value, IsUnsigned);
}

DISubroutineType *
revenant::ir::createSubroutineType(DIBuilder &DIB,
                                   ArrayRef<Metadata *> Params) {
  return DIB.createSubroutineType(DIB.getOrCreateTypeArray(Params));
}

void revenant::ir::attachGlobalVariable(GlobalVariable &GV, DIBuilder &DIB,
                                        DICompileUnit &CU, StringRef Name,
                                        DIType *Ty) {
  auto *DGV = DIB.createGlobalVariableExpression(
      &CU, Name, /*LinkageName=*/GV.getName(), CU.getFile(), /*LineNo=*/1, Ty,
      /*IsLocalToUnit=*/false);
  GV.addDebugInfo(DGV);
}

//===----------------------------------------------------------------------===//
// Verification
//===----------------------------------------------------------------------===//

namespace {

struct Checker {
  const Module &M;
  ContractDiagFn Diag;
  unsigned Errors = 0;

  Checker(const Module &M, ContractDiagFn Diag) : M(M), Diag(Diag) {}

  void err(StringRef Msg) {
    ++Errors;
    Diag(ContractSeverity::Error, Msg);
  }
  template <typename... Args> void errf(const char *Fmt, Args &&...A) {
    err(formatv(Fmt, std::forward<Args>(A)...).str());
  }

  void run() {
    checkModule();
    for (const Function &F : M.functions())
      checkFunction(F);
    for (const GlobalVariable &GV : M.globals())
      checkGlobal(GV);
  }

  //---- module -----------------------------------------------------------

  void checkModule() {
    bool LangSeen = false;
    SmallVector<Module::ModuleFlagEntry, 8> Entries;
    M.getModuleFlagsMetadata(Entries);
    for (const Module::ModuleFlagEntry &E : Entries) {
      StringRef Key = E.Key->getString();
      if (Key.starts_with("revenant.")) {
        if (Key == ModuleFlagLang) {
          LangSeen = true;
          if (E.Behavior != Module::Override)
            err("revenant.lang module flag must have Override behavior");
          if (!getModuleLang(M))
            err("revenant.lang must be one of \"c\"|\"cpp\"|\"rust\"");
        } else {
          errf("unknown revenant.* module flag \"{0}\"", Key);
        }
      }
      if (Key == "Debug Info Version") {
        auto *CI = mdconst::dyn_extract_or_null<ConstantInt>(E.Val);
        if (!CI || CI->getZExtValue() != 3)
          err("Debug Info Version module flag must be i32 3");
      }
    }
    if (!LangSeen)
      err("module is missing required flag revenant.lang");

    if (M.getTargetTriple().empty())
      err("module is missing 'target triple'");
    if (M.getDataLayoutStr().empty())
      err("module is missing 'target datalayout'");

    bool AnyDI = false;
    for (const Function &F : M.functions())
      AnyDI = AnyDI || F.getSubprogram();
    NamedMDNode *CUs = M.getNamedMetadata("llvm.dbg.cu");
    if (AnyDI || (CUs && CUs->getNumOperands())) {
      if (!CUs || CUs->getNumOperands() == 0) {
        err("module has debug info but no llvm.dbg.cu compile unit");
      } else {
        for (MDNode *Op : CUs->operands())
          if (!isa_and_nonnull<DICompileUnit>(Op))
            err("llvm.dbg.cu operand is not a DICompileUnit");
      }
    }
  }

  //---- functions ---------------------------------------------------------

  void checkFunction(const Function &F) {
    StringRef Name = F.getName();
    bool IsAsmStub = Name.starts_with(OpaqueAsmPrefix);

    if (IsAsmStub) {
      if (!F.isDeclaration())
        errf("opaque asm stub \"{0}\" must be a declaration", Name);
      if (F.hasFnAttribute(FnAttrVA))
        errf("opaque asm stub \"{0}\" must not carry revenant.va", Name);
      return;
    }
    if (Name.starts_with("revenant.")) {
      errf("function name \"{0}\" uses the reserved revenant. prefix", Name);
      return;
    }

    if (F.isDeclaration()) {
      // Imports: revenant.va is optional but must be well formed if present.
      if (F.hasFnAttribute(FnAttrVA) && !getFunctionVA(F))
        errf("import \"{0}\" has malformed revenant.va (want \"0x<hex>\")",
             Name);
      return;
    }

    if (!F.hasFnAttribute(FnAttrVA)) {
      errf("function \"{0}\" is missing required attribute revenant.va",
           Name);
    } else if (!getFunctionVA(F)) {
      errf("function \"{0}\" has malformed revenant.va (want \"0x<hex>\")",
           Name);
    } else if (Name.starts_with("sub_")) {
      if (auto NameVA = parseVA(Name.drop_front(4)))
        if (*NameVA != *getFunctionVA(F))
          errf("function \"{0}\" name VA does not match revenant.va 0x{1:x}",
               Name, *getFunctionVA(F));
    }

    checkBlocks(F);
    checkDebugInfo(F);
    checkMetadataKinds(F);
  }

  //---- blocks, instructions, regions --------------------------------------

  void checkBlocks(const Function &F) {
    DenseMap<const BasicBlock *, unsigned> Index;
    unsigned Idx = 0;
    for (const BasicBlock &BB : F)
      Index[&BB] = Idx++;

    // A successor dominated by its block means a back edge, so that
    // successor is a loop header that needs a loop-kind descriptor.
    DominatorTree DT(const_cast<Function &>(F));
    SmallPtrSet<const BasicBlock *, 8> LoopHeaders;
    for (const BasicBlock &BB : F)
      for (const BasicBlock *Succ : successors(BB.getTerminator()))
        if (Succ == &BB || DT.dominates(Succ, &BB))
          LoopHeaders.insert(Succ);

    StringSet<> Seen;
    for (const BasicBlock &BB : F) {
      if (!BB.hasName()) {
        errf("unnamed basic block #{0} in \"{1}\"", Index.lookup(&BB),
             F.getName());
      } else {
        checkBlockName(F, BB, Index.lookup(&BB), Seen);
      }

      for (const Instruction &I : BB) {
        if (!isDbgIntrinsicCall(I) && !getInstructionVA(I))
          errf("instruction missing !revenant.va in \"{0}\" block \"{1}\": "
               "{2}", F.getName(), BB.getName(), printableInst(I));
        checkOpaque(I);
        if (I.getMetadata(MDKindRegion) && !I.isTerminator())
          errf("!revenant.region on non-terminator in \"{0}\": {1}",
               F.getName(), printableInst(I));
      }

      checkRegions(F, BB, Index);
    }

    for (const BasicBlock *Head : LoopHeaders) {
      bool HasLoopDesc = false;
      for (const RegionAnnotation &A :
           getRegionAnnotations(*Head->getTerminator()))
        HasLoopDesc = HasLoopDesc || isLoopRegionKind(A.Kind);
      if (!HasLoopDesc)
        errf("loop header \"{0}\" in \"{1}\" lacks a loop-kind "
             "!revenant.region descriptor", Head->getName(), F.getName());
    }
  }

  void checkBlockName(const Function &F, const BasicBlock &BB, unsigned Idx,
                      StringSet<> &Seen) {
    StringRef N = BB.getName();
    bool WellFormed;
    if (N.starts_with("bb_x")) {
      StringRef D = N.drop_front(4);
      WellFormed = !D.empty() && all_of(D, [](char c) { return isDigit(c); });
    } else if (N.starts_with("bb_")) {
      StringRef D = N.drop_front(3);
      WellFormed = !D.empty() &&
                   all_of(D, [](char c) { return isHexDigit(c); }) &&
                   (D.size() == 1 || D.front() != '0');
    } else {
      WellFormed = false;
    }
    if (!WellFormed) {
      errf("block name \"{0}\" in \"{1}\" is not bb_<hex>/bb_x<index>", N,
           F.getName());
      return;
    }
    if (!Seen.insert(N).second)
      errf("duplicate block name \"{0}\" in \"{1}\"", N, F.getName());
    (void)Idx;
  }

  void checkOpaque(const Instruction &I) {
    const auto *CB = dyn_cast<CallBase>(&I);
    bool Marked = I.getMetadata(MDKindOpaque) != nullptr;
    const Function *Callee = CB ? CB->getCalledFunction() : nullptr;
    bool ToAsm = Callee && Callee->getName().starts_with(OpaqueAsmPrefix);
    if (Marked && !ToAsm)
      errf("!revenant.opaque on a non revenant.asm.* call in \"{0}\": {1}",
           I.getFunction()->getName(), printableInst(I));
    if (ToAsm && !Marked)
      errf("call to \"{0}\" in \"{1}\" is missing !revenant.opaque",
           Callee->getName(), I.getFunction()->getName());
  }

  void checkRegions(const Function &F, const BasicBlock &BB,
                    const DenseMap<const BasicBlock *, unsigned> &Index) {
    const Instruction *T = BB.getTerminator();
    MDNode *N = T->getMetadata(MDKindRegion);
    if (!N)
      return;
    unsigned NBlocks = Index.size();

    SmallVector<const MDNode *, 2> Descs;
    if (N->getNumOperands() > 0 && isa<MDNode>(N->getOperand(0))) {
      for (const MDOperand &Op : N->operands()) {
        if (const MDNode *D = dyn_cast_or_null<MDNode>(Op.get()))
          Descs.push_back(D);
        else
          errf("!revenant.region list element on \"{0}\" in \"{1}\" is not a "
               "node", BB.getName(), F.getName());
      }
    } else {
      Descs.push_back(N);
    }

    for (const MDNode *D : Descs) {
      auto *S = D->getNumOperands()
                    ? dyn_cast<MDString>(D->getOperand(0))
                    : nullptr;
      if (!S) {
        errf("malformed !revenant.region on \"{0}\" in \"{1}\"", BB.getName(),
             F.getName());
        continue;
      }
      auto Kind = parseKind(S->getString());
      if (!Kind) {
        errf("unknown !revenant.region kind \"{0}\" on \"{1}\" in \"{2}\"",
             S->getString(), BB.getName(), F.getName());
        continue;
      }
      if (D->getNumOperands() != 1 + operandCount(*Kind)) {
        errf("!revenant.region \"{0}\" on \"{1}\" in \"{2}\" wants {3} "
             "operand(s)", S->getString(), BB.getName(), F.getName(),
             operandCount(*Kind));
        continue;
      }
      int32_t Ops[3] = {NoBlockIndex, NoBlockIndex, NoBlockIndex};
      bool BadOps = false;
      for (unsigned I = 0; I < operandCount(*Kind); ++I) {
        auto *CI = mdconst::dyn_extract<ConstantInt>(D->getOperand(I + 1));
        if (!CI || !CI->getType()->isIntegerTy(32)) {
          errf("!revenant.region \"{0}\" operand {1} on \"{2}\" in \"{3}\" "
               "is not i32", S->getString(), I, BB.getName(), F.getName());
          BadOps = true;
          break;
        }
        Ops[I] = static_cast<int32_t>(CI->getSExtValue());
        if (Ops[I] < NoBlockIndex || Ops[I] >= (int32_t)NBlocks) {
          errf("!revenant.region \"{0}\" operand {1} = {2} on \"{3}\" in "
               "\"{4}\" is out of range ({5} blocks)", S->getString(), I,
               Ops[I], BB.getName(), F.getName(), NBlocks);
          BadOps = true;
          break;
        }
      }
      if (BadOps)
        continue;

      switch (*Kind) {
      case RegionKind::If:
      case RegionKind::SC:
        if (!isa<BranchInst>(T) || !cast<BranchInst>(T)->isConditional())
          errf("!revenant.region \"{0}\" on non-conditional terminator in "
               "\"{1}\" block \"{2}\"", S->getString(), F.getName(),
               BB.getName());
        break;
      case RegionKind::Switch:
        if (!isa<SwitchInst>(T))
          errf("!revenant.region \"switch\" on non-switch terminator in "
               "\"{0}\" block \"{1}\"", F.getName(), BB.getName());
        break;
      case RegionKind::While:
      case RegionKind::DoWhile:
      case RegionKind::For:
      case RegionKind::Loop:
        if (Ops[0] != (int32_t)Index.lookup(&BB))
          errf("!revenant.region \"{0}\" header operand {1} != own block "
               "index {2} in \"{3}\"", S->getString(), Ops[0],
               Index.lookup(&BB), F.getName());
        break;
      case RegionKind::Goto:
        break;
      }
    }
  }

  //---- globals --------------------------------------------------------------

  void checkGlobal(const GlobalVariable &GV) {
    StringRef Name = GV.getName();
    if (Name.starts_with("revenant."))
      errf("global name \"{0}\" uses the reserved revenant. prefix", Name);
    if (Name.starts_with("str_")) {
      const Constant *C = GV.hasInitializer() ? GV.getInitializer() : nullptr;
      auto *AT = dyn_cast_or_null<ArrayType>(C ? C->getType() : nullptr);
      if (!GV.isConstant() || !AT || !AT->getElementType()->isIntegerTy(8))
        errf("global \"{0}\" uses the str_ prefix but is not a constant "
             "[N x i8]", Name);
    }
  }

  //---- metadata kinds ---------------------------------------------------------

  void checkMetadataKinds(const Function &F) {
    LLVMContext &C = M.getContext();
    SmallVector<StringRef, 8> Names;
    C.getMDKindNames(Names);
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        SmallVector<std::pair<unsigned, MDNode *>, 8> MDs;
        I.getAllMetadata(MDs);
        for (auto &[K, Node] : MDs) {
          if (K < Names.size() && Names[K].starts_with("revenant.") &&
              !isKnownContractKindName(Names[K]))
            errf("unknown revenant.* metadata kind \"{0}\" in \"{1}\"",
                 Names[K], F.getName());
        }
      }
    }
  }

  static bool isKnownContractKindName(StringRef N) {
    return N == MDKindVA || N == MDKindRegion || N == MDKindOpaque;
  }

  //---- debug info -------------------------------------------------------------

  void checkDebugInfo(const Function &F) {
    bool HasDeclares = false;
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB)
        for (const DbgRecord &R : I.getDbgRecordRange())
          if (auto *DVR = dyn_cast<DbgVariableRecord>(&R))
            HasDeclares = HasDeclares || DVR->isDbgDeclare();
    if (HasDeclares) {
      if (!F.getSubprogram())
        errf("function \"{0}\" has dbg.declare locals but no DISubprogram",
             F.getName());
      if (!M.getNamedMetadata("llvm.dbg.cu"))
        errf("function \"{0}\" has debug info but module lacks llvm.dbg.cu",
             F.getName());
    }
  }
};

} // namespace

unsigned revenant::ir::verifyModuleContract(const Module &M,
                                            ContractDiagFn Diag) {
  Checker C(M, Diag);
  C.run();
  return C.Errors;
}
