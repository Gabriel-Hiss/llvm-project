//===- ExprContractHarness.cpp - drive ExprBuilder over .ll ----*- C++ -*-===//
//
// Test-only driver (not installed): parses one contract `.ll` module,
// builds a skeletal FunctionDecl per `define` (params + alloca VarDecls,
// like Core will), runs ExprBuilder over every non-terminator instruction
// in order, and prints the resulting statements with Clang's printer.
// Used to shake ExprBuilder against the RvIRContract reference modules
// before revenant-clang lands.
//
// Usage: RevenantExprContractHarness <file.ll> [c|cpp]
//
//===----------------------------------------------------------------------===//

#include "../ExprBuilderImpl.h"

#include "revenant/Clang/FrontendContext.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/ModuleLoader.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
// Same minimal types as the unit tests.
class TestTypes : public revenant::clangx::TypeBuilder {
public:
  explicit TestTypes(revenant::clangx::FrontendContext *C) : Ctx(C) {}
  clang::QualType typeOf(llvm::Type *Ty, const llvm::DIType *DI) override {
    clang::ASTContext &A = Ctx->ast();
    if (Ty->isVoidTy())
      return A.VoidTy;
    if (Ty->isFloatTy())
      return A.FloatTy;
    if (Ty->isDoubleTy())
      return A.DoubleTy;
    if (Ty->isIntegerTy(1))
      return Ctx->isCPlusPlus() ? A.BoolTy : A.UnsignedCharTy;
    if (Ty->isIntegerTy()) {
      bool Signed = DI ? isSignedDI(DI) : true;
      switch (Ty->getIntegerBitWidth()) {
      case 8:
        return Signed ? A.SignedCharTy : A.UnsignedCharTy;
      case 16:
        return Signed ? A.ShortTy : A.UnsignedShortTy;
      case 32:
        return Signed ? A.IntTy : A.UnsignedIntTy;
      default:
        return Signed ? A.LongLongTy : A.UnsignedLongLongTy;
      }
    }
    if (Ty->isPointerTy())
      return A.getPointerType(A.VoidTy);
    return A.UnsignedLongLongTy;
  }
  clang::QualType typeOfDI(const llvm::DIType *DI) override {
    return typeOf(llvm::Type::getInt64Ty(Ctx->module().getContext()), DI);
  }
  clang::QualType functionType(llvm::Function &F) override {
    clang::ASTContext &A = Ctx->ast();
    SmallVector<clang::QualType, 4> Ps;
    for (llvm::Argument &Ag : F.args())
      Ps.push_back(typeOf(Ag.getType(), nullptr));
    clang::FunctionProtoType::ExtProtoInfo EPI;
    return A.getFunctionType(typeOf(F.getReturnType(), nullptr), Ps, EPI);
  }
  clang::QualType globalType(llvm::GlobalVariable &GV) override {
    return typeOf(GV.getValueType(), nullptr);
  }
  clang::RecordDecl *recordFor(const llvm::DICompositeType *) override {
    return nullptr;
  }
  const llvm::DISubprogram *
  virtualMethod(const llvm::DICompositeType *, unsigned) override {
    return nullptr;
  }
  clang::CXXMethodDecl *methodFor(const llvm::DISubprogram *) override {
    return nullptr;
  }

private:
  revenant::clangx::FrontendContext *Ctx;
  static bool isSignedDI(const llvm::DIType *DI) {
    if (auto *BT = llvm::dyn_cast<llvm::DIBasicType>(DI))
      return BT->getEncoding() == llvm::dwarf::DW_ATE_signed ||
             BT->getEncoding() == llvm::dwarf::DW_ATE_signed_char;
    return true;
  }
};
} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  if (argc < 2) {
    errs() << "usage: RevenantExprContractHarness <file.ll> [c|cpp]\n";
    return 2;
  }
  bool IsCXX = argc > 2 && std::string(argv[2]) == "cpp";

  LLVMContext LLCtx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyFile(argv[1], Err, LLCtx);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Clang scaffolding (same shape as the unit tests).
  auto DiagIDs = new clang::DiagnosticIDs();
  clang::DiagnosticOptions DiagOpts;
  auto Diags = new clang::DiagnosticsEngine(
      llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(DiagIDs), DiagOpts);
  auto TOpts = std::make_shared<clang::TargetOptions>();
  TOpts->Triple = M->getTargetTriple().str();
  clang::LangOptions LangOpts;
  LangOpts.CPlusPlus = IsCXX;
  clang::TargetInfo *Target = clang::TargetInfo::CreateTargetInfo(*Diags, *TOpts);
  clang::FileSystemOptions FSOpts;
  auto Files = new clang::FileManager(FSOpts);
  auto SM = new clang::SourceManager(*Diags, *Files);
  auto Idents = std::make_unique<clang::IdentifierTable>(LangOpts);
  auto Sels = std::make_unique<clang::SelectorTable>();
  auto Builtins = std::make_unique<clang::Builtin::Context>();
  clang::HeaderSearchOptions HSOpts;
  auto Headers = std::make_shared<clang::HeaderSearch>(HSOpts, *SM, *Diags,
                                                       LangOpts, Target);
  auto ACtx = std::make_unique<clang::ASTContext>(
      LangOpts, *SM, *Idents, *Sels, *Builtins, clang::TU_Complete);
  ACtx->InitBuiltinTypes(*Target);
  auto FCtx = std::make_unique<revenant::clangx::FrontendContext>(
      *ACtx, *M,
      IsCXX ? revenant::clangx::SourceLang::CPlusPlus
            : revenant::clangx::SourceLang::C);
  TestTypes Types(FCtx.get());
  auto Exprs = revenant::clangx::createExprBuilder(*FCtx, Types);

  // Module globals bound like Core binds them (string constants keep
  // their @str_* name; the builder turns them into literals on use).
  for (llvm::GlobalVariable &GV : M->globals()) {
    clang::QualType T = Types.globalType(GV);
    clang::VarDecl *VD = clang::VarDecl::Create(
        *ACtx, ACtx->getTranslationUnitDecl(), clang::SourceLocation(),
        clang::SourceLocation(), FCtx->ident(GV.getName()), T,
        ACtx->getTrivialTypeSourceInfo(T), clang::SC_None);
    FCtx->bindDecl(&GV, VD);
  }
  clang::PrintingPolicy Policy(LangOpts);
  unsigned Errors = 0;
  for (llvm::Function &F : *M) {
    if (F.isDeclaration())
      continue;
    // Skeletal FunctionDecl: params bound like Core binds them.
    SmallVector<clang::QualType, 8> PTs;
    for (llvm::Argument &A : F.args())
      PTs.push_back(Types.typeOf(A.getType(), nullptr));
    clang::FunctionProtoType::ExtProtoInfo EPI;
    clang::QualType FQT = ACtx->getFunctionType(
        Types.typeOf(F.getReturnType(), nullptr), PTs, EPI);
    clang::FunctionDecl *FD = clang::FunctionDecl::Create(
        *ACtx, ACtx->getTranslationUnitDecl(), clang::SourceLocation(),
        clang::SourceLocation(), FCtx->ident(F.getName()), FQT,
        ACtx->getTrivialTypeSourceInfo(FQT), clang::SC_None);
    SmallVector<clang::ParmVarDecl *, 8> Ps;
    unsigned Idx = 0;
    for (llvm::Argument &A : F.args()) {
      clang::ParmVarDecl *P = clang::ParmVarDecl::Create(
          *ACtx, FD, clang::SourceLocation(), clang::SourceLocation(),
          FCtx->ident(A.getName().empty()
                          ? ("a" + std::to_string(Idx + 1))
                          : A.getName()),
          PTs[Idx], ACtx->getTrivialTypeSourceInfo(PTs[Idx]), clang::SC_None,
          nullptr);
      Ps.push_back(P);
      FCtx->bindDecl(&A, P);
      ++Idx;
    }
    FD->setParams(Ps);
    // Alloca VarDecls, like Core creates after SROA.
    for (llvm::BasicBlock &BB : F)
      for (llvm::Instruction &I : BB)
        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
          clang::QualType T =
              Types.typeOf(AI->getAllocatedType(), nullptr);
          clang::VarDecl *VD = clang::VarDecl::Create(
              *ACtx, FD, clang::SourceLocation(), clang::SourceLocation(),
              FCtx->ident(AI->getName()), T,
              ACtx->getTrivialTypeSourceInfo(T), clang::SC_None);
          FCtx->bindDecl(AI, VD);
        }

    revenant::clangx::ExprScope Scope;
    Exprs->beginFunction(F, FD, Scope);
    outs() << "// ---- " << F.getName() << "\n";
    for (llvm::BasicBlock &BB : F) {
      outs() << "// block " << BB.getName() << "\n";
      for (llvm::Instruction &I : BB) {
        if (I.isTerminator()) {
          if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
            clang::Expr *R =
                Exprs->retVal(RI->getReturnValue(), Scope);
            outs() << "return";
            if (R) {
              outs() << " ";
              R->printPretty(outs(), nullptr, Policy);
            }
            outs() << ";\n";
          } else {
            outs() << "// terminator: " << I.getOpcodeName() << "\n";
          }
          continue;
        }
        clang::Stmt *S = Exprs->instr(I, Scope);
        if (!S) {
          // Folded into a use, or a temp StmtBuilder will emit.
          if (FCtx->lookupDecl(&I))
            outs() << "// temp v for %" << I.getName() << "\n";
          continue;
        }
        if (auto *E = llvm::dyn_cast<clang::Expr>(S)) {
          E->printPretty(outs(), nullptr, Policy);
          outs() << ";\n";
        } else {
          S->printPretty(outs(), nullptr, Policy);
          outs() << "\n";
        }
      }
    }
    // Print pending temporaries' bound decls for visibility.
    for (llvm::Instruction *T : Scope.PendingTemps) {
      if (auto *VD =
              llvm::dyn_cast_or_null<clang::VarDecl>(FCtx->lookupDecl(T))) {
        outs() << "// pending temp: ";
        VD->print(outs(), Policy);
        outs() << "\n";
      } else {
        outs() << "// UNBOUND temp for %" << T->getName() << " (BUG)\n";
        ++Errors;
      }
    }
  }
  if (Errors) {
    errs() << "FAILED: " << Errors << " unbound temporaries\n";
    return 1;
  }
  outs() << "// OK\n";
  return 0;
}
