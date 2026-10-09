//===- ClangFrontend.h - contract .ll -> C/C++ text -------------*- C++ -*-===//
//
// Host for the Clang frontend: builds a clang::ASTContext for the module's
// target triple and `revenant.lang`, declares globals/imports/functions,
// builds bodies through StmtBuilder+ExprBuilder with types through
// TypeBuilder, and prints the translation unit with Clang's Decl printer.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_FRONTEND_H
#define REVENANT_CLANG_FRONTEND_H

#include "clang/AST/ASTContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "revenant/Clang/FrontendContext.h"
#include <memory>

namespace clang {
class DiagnosticsEngine;
class DiagnosticOptions;
class FileManager;
class LangOptions;
class SelectorTable;
class SourceManager;
class TargetInfo;
class TranslationUnitDecl;
namespace Builtin {
class Context;
}
} // namespace clang

namespace revenant::clangx {

class ExprBuilder;
class FrontendContext;
class StmtBuilder;
class TypeBuilder;

/// Options for one decompile run; the tool fills this once.
struct ClangFrontendOptions {
  /// Output language override; empty = use the `revenant.lang` module flag.
  /// Accepts "c" or "cpp"/"c++".
  llvm::StringRef Lang;
  /// Emit `goto`-based bodies even when region annotations are present.
  bool ForceGoto = false;
};

/// Owns the Clang infrastructure for one module. `createASTContext` makes
/// it; `fillTranslationUnit` populates declarations and bodies. All clang
/// nodes stay valid while this object lives.
class TranslationUnit {
public:
  ~TranslationUnit();

  clang::ASTContext &ast() { return *AST; }
  clang::TranslationUnitDecl *tu() { return TU; }
  FrontendContext &frontend() { return *FCtx; }
  // Members destroy in reverse declaration order: AST is declared last so
  // it dies before the Idents/Builtins/Selectors/Sources it points into.
  std::unique_ptr<clang::DiagnosticsEngine> Diags;
  std::unique_ptr<clang::DiagnosticOptions> DiagOpts;
  std::unique_ptr<clang::FileManager> Files;
  std::unique_ptr<clang::SourceManager> Sources;
  std::unique_ptr<clang::LangOptions> LangOpts;
  std::unique_ptr<clang::Builtin::Context> Builtins;
  std::unique_ptr<clang::SelectorTable> Selectors;
  std::unique_ptr<clang::IdentifierTable> Idents;
  std::unique_ptr<clang::TargetInfo> Target;
  std::unique_ptr<clang::ASTContext> AST;
  clang::TranslationUnitDecl *TU = nullptr;
  std::unique_ptr<FrontendContext> FCtx;
};

/// Create the empty shell: LangOptions/Target/ASTContext/FrontendContext
/// for `M`'s triple and language. No declarations yet; the caller then
/// constructs its builders against `T->frontend()` and calls
/// fillTranslationUnit.
llvm::Expected<std::unique_ptr<TranslationUnit>>
createASTContext(llvm::Module &M, const ClangFrontendOptions &Opts);

/// Populate `T`: globals (incl. `@str_*` string constants and `@g_*` data),
/// external FunctionDecls for imports, FunctionDecls with ParmVarDecls and
/// local VarDecls for every `define`, bodies via `Stmts`/`Exprs`, types via
/// `Types`.
void fillTranslationUnit(TranslationUnit &T, llvm::Module &M,
                         TypeBuilder &Types, StmtBuilder &Stmts,
                         ExprBuilder &Exprs);

/// Print the translation unit as C/C++ source using Clang's own printer.
/// `Includes` is emitted verbatim as `#include` lines first.
void printTranslationUnit(clang::TranslationUnitDecl *TU,
                          llvm::raw_ostream &OS,
                          llvm::ArrayRef<llvm::StringRef> Includes);

/// Read `revenant.lang` off `M`, honoring an explicit override.
SourceLang langFor(const llvm::Module &M, const ClangFrontendOptions &Opts);

} // namespace revenant::clangx

#endif // REVENANT_CLANG_FRONTEND_H
