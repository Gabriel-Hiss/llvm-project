//===- TranslationUnit.cpp - IR module -> clang::ASTContext -----*- C++ -*-===//
//
// Host: stands up a bare clang::ASTContext for the module's triple and
// `revenant.lang`, declares globals/imports/functions with DI-backed names
// and types, attaches bodies via StmtBuilder/ExprBuilder, prints the TU.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/ClangFrontend.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/FrontendContext.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Clang/WideIntModel.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Error.h"
#include <unordered_set>


using llvm::APInt;
using llvm::ArrayRef;
using llvm::DenseMap;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;

namespace revenant::clangx {

namespace c = clang;

TranslationUnit::~TranslationUnit() = default;
SourceLang langFor(const llvm::Module &M, const ClangFrontendOptions &Opts) {
  if (!Opts.Lang.empty())
    return (Opts.Lang == "cpp" || Opts.Lang == "c++" || Opts.Lang == "cxx")
               ? SourceLang::CPlusPlus
               : SourceLang::C;
  return moduleLang(M);
}

llvm::Expected<std::unique_ptr<TranslationUnit>>
createASTContext(llvm::Module &M, const ClangFrontendOptions &Opts) {
  SourceLang Lang = langFor(M, Opts);
  auto T = std::make_unique<TranslationUnit>();
  T->LangOpts = std::make_unique<c::LangOptions>();
  c::LangOptions &LO = *T->LangOpts;
  LO.CPlusPlus = Lang == SourceLang::CPlusPlus;
  LO.CPlusPlus11 = LO.CPlusPlus14 = LO.CPlusPlus17 = LO.CPlusPlus;
  LO.Bool = true;
  // The emitted text must also compile with GCC/Clang, not just MSVC.
  LO.MicrosoftExt = LO.MSVCCompat = false;
  LO.DeclSpecKeyword = true;
  LO.WChar = true;

  llvm::IntrusiveRefCntPtr<c::DiagnosticIDs> DiagIDs(new c::DiagnosticIDs);
  T->DiagOpts = std::make_unique<c::DiagnosticOptions>();
  T->Diags = std::make_unique<c::DiagnosticsEngine>(
      DiagIDs, *T->DiagOpts, new c::IgnoringDiagConsumer());
  c::FileSystemOptions FSO;
  T->Files = std::make_unique<c::FileManager>(FSO, nullptr);
  T->Sources = std::make_unique<c::SourceManager>(*T->Diags, *T->Files,
                                                  /*UserFilesAreVolatile=*/
                                                  false);

  auto TargetOpts = std::make_shared<c::TargetOptions>();
  std::string TripleStr = M.getTargetTriple().str();
  if (TripleStr.empty())
    TripleStr = "x86_64-pc-windows-msvc";
  TargetOpts->Triple = TripleStr;
  T->Target.reset(c::TargetInfo::CreateTargetInfo(*T->Diags, *TargetOpts));

  T->Idents = std::make_unique<c::IdentifierTable>(LO);
  T->Selectors = std::make_unique<c::SelectorTable>();
  T->Builtins = std::make_unique<c::Builtin::Context>();
  T->AST = std::make_unique<c::ASTContext>(
      LO, *T->Sources, *T->Idents, *T->Selectors, *T->Builtins, c::TU_Complete);
  if (T->Target)
    T->AST->InitBuiltinTypes(*T->Target);
  T->Builtins->initializeBuiltins(*T->Idents, LO);
  T->TU = T->AST->getTranslationUnitDecl();
  T->FCtx = std::make_unique<FrontendContext>(*T->AST, M, Lang);
  return std::move(T);
}

/// A DILocalVariable bound to `AI` via debug declare records, if any.
static const llvm::DILocalVariable *dbgVarFor(llvm::AllocaInst &AI) {
  for (auto *DVR : llvm::findDVRDeclares(&AI))
    if (auto *V = DVR->getVariable())
      return V;
  return nullptr;
}


/// Use a recovered array DI type only when its extent and element storage
/// shape agree with the LLVM alloca. A wrong element stride changes every
/// GEP-based access, even when total byte size happens to match.
static bool arrayDIShapeMatches(c::ASTContext &AST,
                                const llvm::DataLayout &DL,
                                llvm::Type *IRTy, c::QualType DIQT) {
  auto *IRArray = llvm::dyn_cast<llvm::ArrayType>(IRTy);
  auto *DIArray =
      llvm::dyn_cast<c::ConstantArrayType>(DIQT.getTypePtr());
  if (!IRArray || !DIArray ||
      DIArray->getSize().getZExtValue() != IRArray->getNumElements())
    return false;

  llvm::Type *IRElem = IRArray->getElementType();
  c::QualType DIElem = DIArray->getElementType();
  bool SameCategory =
      (IRElem->isIntegerTy() && DIElem->isIntegerType()) ||
      (IRElem->isFloatingPointTy() && DIElem->isFloatingType()) ||
      (IRElem->isPointerTy() && DIElem->isPointerType()) ||
      (IRElem->isStructTy() && DIElem->isRecordType()) ||
      (IRElem->isArrayTy() && DIElem->isArrayType()) ||
      (IRElem->isVectorTy() && DIElem->isVectorType());
  if (!SameCategory)
    return false;

  llvm::TypeSize ElemBits = DL.getTypeStoreSizeInBits(IRElem);
  llvm::TypeSize ArrayBits = DL.getTypeAllocSizeInBits(IRArray);
  return !ElemBits.isScalable() && !ArrayBits.isScalable() &&
         AST.getTypeSize(DIElem) == ElemBits.getFixedValue() &&
         AST.getTypeSize(DIArray) == ArrayBits.getFixedValue();
}

/// DWARF arg number -> DILocalVariable, for the function's params.
static DenseMap<unsigned, const llvm::DILocalVariable *>
paramVars(const llvm::DISubprogram *SP, llvm::Function &F) {
  DenseMap<unsigned, const llvm::DILocalVariable *> Out;
  if (!SP)
    return Out;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *DVI = llvm::dyn_cast<llvm::DbgVariableIntrinsic>(&I)) {
        const llvm::DILocalVariable *V = DVI->getVariable();
        if (V && V->isParameter() && V->getArg())
          Out[V->getArg()] = V;
      }
  return Out;
}

static StringRef vtableClassName(StringRef Name) {
  if (!Name.starts_with("??_7"))
    return {};
  return Name.drop_front(4).split("@@").first;
}

static void buildGlobals(llvm::Module &M, FrontendContext &FCtx,
                         TypeBuilder &Types) {
  c::ASTContext &AST = FCtx.ast();
  for (llvm::GlobalVariable &GV : M.globals()) {
    StringRef Name = GV.getName();
    StringRef Class = vtableClassName(Name);
    if (!Class.empty())
      Types.addVTable(Class, &GV);
    c::VarDecl *VD = FCtx.globalDecl(GV, Types);
    if (GV.isDeclaration())
      continue;
    llvm::Constant *Init = GV.getInitializer();
    c::SourceLocation L;
    auto *CDS = llvm::dyn_cast_or_null<llvm::ConstantDataSequential>(Init);
    if (!CDS)
      continue;
    // C allows a string literal that exactly fills the array; C++ always
    // counts the terminator, so there only a NUL-terminated array prints as a
    // literal (without its last byte) and the rest as a byte list.
    if (CDS->isString() && (!FCtx.isCPlusPlus() || CDS->isCString())) {
      StringRef S = CDS->getRawDataValues();
      c::QualType ArrTy = AST.getConstantArrayType(
          AST.CharTy, APInt(64, S.size()), nullptr,
          c::ArraySizeModifier::Normal, 0);
      if (FCtx.isCPlusPlus())
        S = S.drop_back();
      VD->setType(ArrTy);
      VD->setInit(c::StringLiteral::Create(
          AST, S, c::StringLiteralKind::Ordinary, false, ArrTy, L));
      VD->setInitStyle(c::VarDecl::CInit);
    } else if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataArray>(CDS)) {
      SmallVector<c::Expr *, 64> Elems;
      c::QualType ElemTy = Types.typeOf(CDA->getElementType(), nullptr);
      // A literal typed narrower than `int` prints with the Ui8/Ui16 numeric
      // literal suffix, which gcc and clang-cl do not understand
      // ("operator\"\"Ui8\"" not found). Spell the same value with the
      // promoted literal type; the InitListExpr and the global keep ElemTy.
      c::QualType LitTy = ElemTy;
      if (LitTy->isIntegerType() && AST.getIntWidth(LitTy) < 32)
        LitTy = AST.IntTy;
      for (unsigned I = 0, E = CDA->getNumElements(); I != E; ++I)
        Elems.push_back(c::IntegerLiteral::Create(
            AST, APInt(AST.getIntWidth(LitTy), CDA->getElementAsInteger(I)),
            LitTy, L));
      c::Expr *ILE = new (AST) c::InitListExpr(AST, L, Elems, L);
      ILE->setType(VD->getType());
      VD->setInit(ILE);
      VD->setInitStyle(c::VarDecl::CInit);
    }
  }
}


namespace {
/// Prototype shape for a CRT/SDK function name, spelled with the one-letter
/// types `crtPrim` maps to Clang types.
struct CrtShape {
  llvm::StringRef Name;
  char Ret;
  llvm::StringRef Params;
};

/// One entry per CRT/SDK function name the headers this translation unit
/// already includes declare (MSVC `<stdint.h>` reaches vcruntime.h and
/// corecrt.h). `V` void, `I` int, `U` unsigned long long. Each shape is
/// checked against those headers: declaring the lifted guess instead
/// redefines the header's function and the recompile dies on C2371/C2556.
const CrtShape CrtShapes[] = {
    {"_invalid_parameter_noinfo", 'V', ""},
    {"__security_init_cookie", 'V', ""},
    {"_purecall", 'I', ""},
    {"__security_check_cookie", 'V', "U"},
    {"__report_gsfailure", 'V', ""},
    {"_amsg_exit", 'V', "I"},
};


llvm::Type *crtPrim(llvm::LLVMContext &Ctx, char Kind) {
  switch (Kind) {
  case 'V':
    return llvm::Type::getVoidTy(Ctx);
  case 'I':
    return llvm::Type::getInt32Ty(Ctx);
  case 'U':
    return llvm::Type::getInt64Ty(Ctx);
  }
  return llvm::Type::getVoidTy(Ctx);
}
} // namespace

std::optional<CrtPrototype> crtPrototypeFor(llvm::StringRef Name,
                                            llvm::LLVMContext &Ctx) {
  for (const CrtShape &Shape : CrtShapes) {
    if (Name != Shape.Name)
      continue;
    CrtPrototype Proto;
    Proto.Ret = crtPrim(Ctx, Shape.Ret);
    for (char Kind : Shape.Params)
      Proto.Params.push_back(crtPrim(Ctx, Kind));
    return Proto;
  }
  return std::nullopt;
}

/// Declaration for a library function whose name a CRT/SDK header already
/// declares, carrying the header's prototype instead of the lifted guess.
/// Nullptr when no override applies and the caller uses the lifted type.
static c::FunctionDecl *headerPrototypeDeclFor(llvm::Function &F,
                                               FrontendContext &FCtx,
                                               TypeBuilder &Types) {
  if (!F.hasFnAttribute("revenant.lib"))
    return nullptr;
  std::optional<CrtPrototype> Proto = crtPrototypeFor(F.getName(),
                                                      F.getContext());
  if (!Proto || Proto->Params.size() != F.arg_size())
    return nullptr;
  c::ASTContext &AST = FCtx.ast();
  SmallVector<c::QualType, 4> ParamTypes;
  for (llvm::Type *T : Proto->Params)
    ParamTypes.push_back(Types.typeOf(T, nullptr));
  c::QualType FT = AST.getFunctionType(Types.typeOf(Proto->Ret, nullptr),
                                       ParamTypes,
                                       c::FunctionProtoType::ExtProtoInfo());
  auto *FD = c::FunctionDecl::Create(
      AST, AST.getTranslationUnitDecl(), c::SourceLocation(),
      c::SourceLocation(), c::DeclarationName(FCtx.ident(F.getName())), FT,
      nullptr, c::SC_Extern);
  AST.getTranslationUnitDecl()->addDecl(FD);
  FCtx.bindDecl(&F, FD);
  return FD;
}

/// The FunctionDecl for `F` plus its ParmVarDecls: the declaration half of
/// `buildFunction`, which every caller needs (bodies included).
static c::FunctionDecl *declareFunction(llvm::Function &F,
                                        FrontendContext &FCtx,
                                        TypeBuilder &Types) {
  c::ASTContext &AST = FCtx.ast();
  c::FunctionDecl *FD = headerPrototypeDeclFor(F, FCtx, Types);
  if (!FD) {
    FD = FCtx.functionDecl(F, Types);
    // A library body reads from the real CRT/SDK at link time, so its
    // declaration says so instead of leaving the linker to guess.
    if (F.hasFnAttribute("revenant.lib") && FD->getStorageClass() == c::SC_None)
      FD->setStorageClass(c::SC_Extern);
  }

  // Parameters: DI arg names when present, else a1..aN.
  DenseMap<unsigned, const llvm::DILocalVariable *> PV;
  if (llvm::DISubprogram *SP = F.getSubprogram())
    PV = paramVars(SP, F);
  SmallVector<c::ParmVarDecl *, 8> Params;
  unsigned Idx = 1;
  const c::FunctionProtoType *Prototype =
      FD->getType()->getAs<c::FunctionProtoType>();
  for (llvm::Argument &A : F.args()) {
    c::QualType PT =
        Prototype && Idx <= Prototype->getNumParams()
            ? Prototype->getParamType(Idx - 1)
            : Types.typeOf(A.getType(), nullptr);
    c::IdentifierInfo *II = nullptr;
    if (auto *V = PV.lookup(Idx))
      II = FCtx.ident(V->getName());
    if (!II)
      II = FCtx.ident(("a" + Twine(Idx)).str());
    c::ParmVarDecl *PD = c::ParmVarDecl::Create(AST, FD, c::SourceLocation(),
                                                c::SourceLocation(), II, PT,
                                                nullptr, c::SC_None, nullptr);
    FCtx.bindDecl(&A, PD);
    Params.push_back(PD);
    ++Idx;
  }
  // The declared prototype can carry more parameters than the lifted IR
  // (functionDecl normalizes main/wmain to (int, char**), and a libdb
  // prototype may name parameters the lifted signature dropped). Pad with
  // prototype-typed ParmVarDecls so setParams' count always matches.
  if (Prototype) {
    for (unsigned I = Params.size(); I < Prototype->getNumParams(); ++I) {
      c::QualType PT = Prototype->getParamType(I);
      c::IdentifierInfo *II = FCtx.ident(("a" + Twine(I + 1)).str());
      c::ParmVarDecl *PD = c::ParmVarDecl::Create(
          AST, FD, c::SourceLocation(), c::SourceLocation(), II, PT, nullptr,
          c::SC_None, nullptr);
      Params.push_back(PD);
    }
  }
  FD->setParams(Params);
  return FD;
}

static void buildFunction(llvm::Function &F, FrontendContext &FCtx,
                          TypeBuilder &Types, StmtBuilder &Stmts,
                          ExprBuilder &Exprs) {
  c::ASTContext &AST = FCtx.ast();
  c::FunctionDecl *FD = declareFunction(F, FCtx, Types);

  if (F.isDeclaration())
    return;
  // Locals: one VarDecl per alloca, DI names/types when available.
  SmallVector<c::Stmt *, 16> LocalDecls;
  for (llvm::BasicBlock &BB : F)
    for (llvm::Instruction &I : BB)
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
        const llvm::DILocalVariable *V = dbgVarFor(*AI);
        const llvm::DIType *DI = V ? V->getType() : nullptr;
        c::QualType LT = Types.typeOf(AI->getAllocatedType(), DI);
        if (AI->getAllocatedType()->isArrayTy() &&
            !arrayDIShapeMatches(AST, F.getParent()->getDataLayout(),
                                 AI->getAllocatedType(), LT))
          LT = Types.typeOf(AI->getAllocatedType(), nullptr);
        c::IdentifierInfo *II =
            FCtx.ident(V ? V->getName()
                         : (AI->getName().empty()
                                ? Twine("v_").concat(Twine((uintptr_t)AI)).str()
                                : AI->getName().str()));
        c::VarDecl *VD = c::VarDecl::Create(AST, FD, c::SourceLocation(),
                                            c::SourceLocation(), II, LT,
                                            nullptr, c::SC_None);
        FCtx.bindDecl(AI, VD);
        LocalDecls.push_back(new (AST) c::DeclStmt(
            c::DeclGroupRef(VD), c::SourceLocation(), c::SourceLocation()));
      }

  ExprScope Scope;
  c::CompoundStmt *Body = Stmts.buildBody(F, FD, Exprs, Scope);
  SmallVector<llvm::Instruction *, 16> Temps(Scope.PendingTemps.begin(),
                                             Scope.PendingTemps.end());
  for (llvm::Instruction *I : Temps) {
    auto *VD = llvm::dyn_cast_or_null<c::VarDecl>(FCtx.lookupDecl(I));
    if (!VD)
      continue;
    LocalDecls.push_back(new (AST) c::DeclStmt(
        c::DeclGroupRef(VD), c::SourceLocation(), c::SourceLocation()));
  }
  if (!LocalDecls.empty()) {
    SmallVector<c::Stmt *, 32> WithLocals(LocalDecls.begin(), LocalDecls.end());
    WithLocals.push_back(Body);
    Body = c::CompoundStmt::Create(AST, WithLocals, c::FPOptionsOverride(),
                                   c::SourceLocation(), c::SourceLocation());
  }

  c::QualType RT = FD->getType()->castAs<c::FunctionType>()->getReturnType();
  if (!RT->isVoidType()) {
    SmallVector<c::Stmt *, 2> Tail;
    Tail.push_back(Body);
    llvm::Constant *Zero = llvm::Constant::getNullValue(F.getReturnType());
    Tail.push_back(c::ReturnStmt::Create(
        AST, c::SourceLocation(), Exprs.retVal(Zero, Scope), nullptr));
    Body = c::CompoundStmt::Create(AST, Tail, c::FPOptionsOverride(),
                                   c::SourceLocation(), c::SourceLocation());
  }
  FD->setBody(Body);
}

void fillTranslationUnit(TranslationUnit &T, llvm::Module &M,
                         TypeBuilder &Types, StmtBuilder &Stmts,
                         ExprBuilder &Exprs, bool UserBodiesOnly) {
  buildGlobals(M, *T.FCtx, Types);
  for (llvm::Function &F : M) {
    // A library body is code the real CRT/SDK ships: emitting ours as well
    // redefines the header's function (MSVC C2371/C2556) and duplicates the
    // definition at link. The declaration stays so calls still resolve, and
    // its prototype follows the header when one declares the name.
    if (UserBodiesOnly && F.hasFnAttribute("revenant.lib"))
      declareFunction(F, *T.FCtx, Types);
    else
      buildFunction(F, *T.FCtx, Types, Stmts, Exprs);
  }
}


static c::FunctionDecl *prototypeFor(c::FunctionDecl *Definition,
                                     c::ASTContext &AST) {
  auto *Prototype = c::FunctionDecl::Create(
      AST, AST.getTranslationUnitDecl(), c::SourceLocation(),
      c::SourceLocation(), Definition->getDeclName(), Definition->getType(),
      Definition->getTypeSourceInfo(), Definition->getStorageClass(),
      Definition->UsesFPIntrin(), Definition->isInlineSpecified());
  SmallVector<c::ParmVarDecl *, 8> Parameters;
  for (c::ParmVarDecl *Parameter : Definition->parameters()) {
    Parameters.push_back(c::ParmVarDecl::Create(
        AST, Prototype, c::SourceLocation(), c::SourceLocation(),
        Parameter->getIdentifier(), Parameter->getType(), nullptr,
        c::SC_None, nullptr));
  }
  Prototype->setParams(Parameters);
  return Prototype;
}

void printTranslationUnit(c::TranslationUnitDecl *TU, llvm::raw_ostream &OS,
                          ArrayRef<StringRef> Includes) {
  c::PrintingPolicy Policy = TU->getASTContext().getPrintingPolicy();
  for (StringRef Inc : Includes)
    OS << "#include <" << Inc << ">\n";
  if (!Includes.empty())
    OS << "\n";
  emitWideIntPreamble(OS, TU->getASTContext());
  std::unordered_set<std::string> PrintedRecords;
  for (unsigned Pass = 0; Pass != 3; ++Pass) {
    for (c::Decl *D : TU->decls()) {
      if (D->isImplicit())
        continue;
      auto *FD = llvm::dyn_cast<c::FunctionDecl>(D);
      auto *Record = llvm::dyn_cast<c::RecordDecl>(D);
      bool IsRecord = Record != nullptr;
      bool IsNamespace = llvm::isa<c::NamespaceDecl>(D);
      bool IsRecordContainer = IsRecord || IsNamespace;
      bool IsPrototype = FD && !FD->doesThisDeclarationHaveABody();
      bool HasBody = FD && FD->doesThisDeclarationHaveABody();
      if ((Pass == 0 && !IsRecordContainer) || (Pass == 1 && !FD) ||
          (Pass == 2 && (IsRecordContainer || IsPrototype)))
        continue;
      if (Pass == 0 && Record && Record->isCompleteDefinition() &&
          !Record->getName().empty()) {
        std::string Key =
            (Record->isUnion() ? "union " : "struct ") +
            Record->getNameAsString();
        if (Key == "struct rv_u128")
          continue; // defined in the MSVC helper block above
        if (!PrintedRecords.insert(std::move(Key)).second)
          continue;
      }
      if (Pass == 1 && HasBody) {
        prototypeFor(FD, TU->getASTContext())
            ->print(OS, Policy, /*Indentation=*/0, /*PrintInstantiation=*/true);
        OS << ";\n";
        continue;
      }
      D->print(OS, Policy, /*Indentation=*/0, /*PrintInstantiation=*/true);
      if (!FD || IsPrototype || Pass == 1)
        OS << ";";
      OS << "\n";
    }
  }
}

} // namespace revenant::clangx
