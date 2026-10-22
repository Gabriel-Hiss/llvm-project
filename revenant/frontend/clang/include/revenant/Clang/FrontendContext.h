//===- FrontendContext.h - shared Clang frontend state ----------*- C++ -*-===//
//
// Part of the Revenant project; shared state for the IR -> Clang AST frontend.
//
// FrontendContext owns the clang::ASTContext-facing state every builder
// (TypeBuilder, ExprBuilder, StmtBuilder) needs: the context itself, the
// source module, the value -> declaration bindings produced while the
// translation unit is built, and identifier sanitization. Create one per
// decompiled module, build declarations with the Core host, then hand the
// context to the builders.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_FRONTENDCONTEXT_H
#define REVENANT_CLANG_FRONTENDCONTEXT_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "revenant/Clang/TypeBuilder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Module.h"
#include <string>

namespace revenant::clangx {

/// Detected producer language, from the `revenant.lang` module flag.
enum class SourceLang { C, CPlusPlus };

/// Per-module state shared by the Clang frontend builders.
///
/// Decl binding model: the Core host creates one clang declaration per
/// llvm::Function / llvm::GlobalVariable, one ParmVarDecl per Argument, and
/// one VarDecl per AllocaInst (after the contract's SROA). Builders resolve
/// an llvm::Value to its clang::ValueDecl through `lookupDecl`. ExprBuilder
/// may additionally bind temporaries for instruction results it cannot emit
/// inline; those go through `bindTemp`/`lookupDecl` as well.
class FrontendContext {
public:
  FrontendContext(clang::ASTContext &AST, llvm::Module &M, SourceLang Lang)
      : AST(AST), M(M), Lang(Lang) {}

  clang::ASTContext &ast() const { return AST; }
  llvm::Module &module() const { return M; }
  SourceLang lang() const { return Lang; }
  bool isCPlusPlus() const { return Lang == SourceLang::CPlusPlus; }

  /// IdentifierInfo for `Name`, sanitized for the target language: non
  /// [A-Za-z0-9_$] characters are rewritten and language keywords get a
  /// trailing underscore. Results are cached; the returned pointer is stable.
  clang::IdentifierInfo *ident(llvm::StringRef Name);

  /// The declaration for a module-level or function-local llvm::Value
  /// (Function, GlobalVariable, Argument, AllocaInst, or an instruction bound
  /// with bindTemp). nullptr if nothing was bound yet.
  clang::ValueDecl *lookupDecl(const llvm::Value *V) const {
    auto It = ValueDecls.find(V);
    return It == ValueDecls.end() ? nullptr : It->second;
  }

  /// Bind an llvm::Value to its clang declaration. Called by Core for
  /// functions, globals, params and allocas; by ExprBuilder for temporaries
  /// it introduces for instruction results.
  void bindDecl(const llvm::Value *V, clang::ValueDecl *D) {
    ValueDecls[V] = D;
  }

  /// Bind a temporary VarDecl ExprBuilder introduced for instruction `V`.
  /// The temporary must live in the current function's scope.
  void bindTemp(llvm::Value *V, clang::VarDecl *D) { bindDecl(V, D); }

  /// Request a system header for the emitted translation unit; the builder
  /// asks for one when it lowers an IR form to a C library call (e.g.
  /// `llvm.memset` -> `memset`, which needs `<string.h>`). Duplicate
  /// requests are ignored and the first request wins.
  void requireInclude(llvm::StringRef Header);

  /// Headers requested through `requireInclude`, in request order.
  llvm::ArrayRef<std::string> requestedIncludes() const {
    return RequestedIncludes;
  }


  /// Function declaration for `F`, creating a declaration-only FunctionDecl
  /// on first use (used for imports and for callees referenced before their
  /// definition is emitted). Function types come from the TypeBuilder.
  clang::FunctionDecl *functionDecl(llvm::Function &F, TypeBuilder &Types);

  /// Global VarDecl for `GV`, created on first use.
  clang::VarDecl *globalDecl(llvm::GlobalVariable &GV, TypeBuilder &Types);

private:
  clang::ASTContext &AST;
  llvm::Module &M;
  SourceLang Lang;
  llvm::DenseMap<const llvm::Value *, clang::ValueDecl *> ValueDecls;
  llvm::StringMap<clang::IdentifierInfo *> Sanitized;
  llvm::SmallVector<std::string, 4> RequestedIncludes;
};

/// Read the `revenant.lang` module flag; defaults to C when absent or
/// unknown.
SourceLang moduleLang(const llvm::Module &M);

/// True when `Name` is usable as a C/C++ identifier without sanitizing.
bool isPlainIdentifier(llvm::StringRef Name);

/// Rewrite `Name` into a valid identifier (keyword-safe when `CPlusPlus`).
std::string sanitizeIdentifier(llvm::StringRef Name, bool CPlusPlus);

} // namespace revenant::clangx

#endif // REVENANT_CLANG_FRONTENDCONTEXT_H
