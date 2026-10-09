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
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
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
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Error.h"

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
  LO.MicrosoftExt = LO.MSVCCompat = true;
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

/// DWARF arg number -> DILocalVariable, for the function's params.
static DenseMap<unsigned, const llvm::DILocalVariable *>
paramVars(const llvm::DISubprogram *SP, llvm::Function &F) {
  DenseMap<unsigned, const llvm::DILocalVariable *> Out;
  if (!SP)
    return Out;
  for (auto &BB : F)
    for (auto &I : BB) {
      const llvm::DILocalVariable *V = nullptr;
      if (auto *DDI = llvm::dyn_cast<llvm::DbgDeclareInst>(&I))
        V = DDI->getVariable();
      if (V && V->getArg())
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
      for (unsigned I = 0, E = CDA->getNumElements(); I != E; ++I)
        Elems.push_back(c::IntegerLiteral::Create(
            AST, APInt(AST.getIntWidth(ElemTy), CDA->getElementAsInteger(I)),
            ElemTy, L));
      c::Expr *ILE = new (AST) c::InitListExpr(AST, L, Elems, L);
      ILE->setType(VD->getType());
      VD->setInit(ILE);
      VD->setInitStyle(c::VarDecl::CInit);
    }
  }
}


static void buildFunction(llvm::Function &F, FrontendContext &FCtx,
                          TypeBuilder &Types, StmtBuilder &Stmts,
                          ExprBuilder &Exprs) {
  c::ASTContext &AST = FCtx.ast();
  c::FunctionDecl *FD = FCtx.functionDecl(F, Types);
  // Declarations still need ParmVarDecls for Clang's printer, but have no body.

  // Parameters: DI arg names when present, else a1..aN.
  DenseMap<unsigned, const llvm::DILocalVariable *> PV;
  if (llvm::DISubprogram *SP = F.getSubprogram())
    PV = paramVars(SP, F);
  SmallVector<c::ParmVarDecl *, 8> Params;
  unsigned Idx = 1;
  for (llvm::Argument &A : F.args()) {
    c::QualType PT = Types.typeOf(A.getType(), nullptr);
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
  FD->setParams(Params);

  if (F.isDeclaration())
    return;
  // Locals: one VarDecl per alloca, DI names/types when available.
  SmallVector<c::Stmt *, 16> LocalDecls;
  for (llvm::BasicBlock &BB : F)
    for (llvm::Instruction &I : BB)
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
        const llvm::DILocalVariable *V = dbgVarFor(*AI);
        const llvm::DIType *DI = V ? V->getType() : nullptr;
        c::QualType LT = DI ? Types.typeOfDI(DI)
                            : Types.typeOf(AI->getAllocatedType(), nullptr);
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
                         ExprBuilder &Exprs) {
  buildGlobals(M, *T.FCtx, Types);
  for (llvm::Function &F : M)
    buildFunction(F, *T.FCtx, Types, Stmts, Exprs);
}

void printTranslationUnit(c::TranslationUnitDecl *TU, llvm::raw_ostream &OS,
                          ArrayRef<StringRef> Includes) {
  c::PrintingPolicy Policy = TU->getASTContext().getPrintingPolicy();
  for (StringRef Inc : Includes)
    OS << "#include <" << Inc << ">\n";
  if (!Includes.empty())
    OS << "\n";
  for (unsigned Pass = 0; Pass != 3; ++Pass)
    for (c::Decl *D : TU->decls()) {
      if (D->isImplicit())
        continue;
      auto *FD = llvm::dyn_cast<c::FunctionDecl>(D);
      bool IsRecord = llvm::isa<c::RecordDecl>(D);
      bool IsPrototype = FD && !FD->doesThisDeclarationHaveABody();
      if ((Pass == 0 && !IsRecord) || (Pass == 1 && !IsPrototype) ||
          (Pass == 2 && (IsRecord || IsPrototype)))
        continue;
      D->print(OS, Policy, /*Indentation=*/0, /*PrintInstantiation=*/true);
      if (!FD || IsPrototype)
        OS << ";";
      OS << "\n";
    }
}

} // namespace revenant::clangx
