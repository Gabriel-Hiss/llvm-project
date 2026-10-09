//===- StmtBuilder.h - LLVM function/region -> clang::Stmt ------*- C++ -*-===//
//
// StmtBuilder (owner: Swe2ClangStmt, lib/Stmt/) turns one contract function
// into its body CompoundStmt. It walks the llvm::Function CFG (blocks named
// `bb_<VA hex>`), consumes the structuring annotations on header
// terminators (`!revenant.region !{!"if",follow}` /
// `!{!"while"|"dowhile"|"for",header,latch,exit}` / `!{!"switch",follow}`),
// and emits CompoundStmt/IfStmt/WhileStmt/DoStmt/ForStmt/SwitchStmt. Blocks
// no annotation covers fall back to `LabelStmt` + `GotoStmt` on the block
// names. Non-terminator instructions and branch conditions go through
// ExprBuilder; ExprBuilder is not responsible for control flow.
//
// Core ships GotoStmtBuilder (lib/Core/) as the reference implementation:
// labels + gotos, always produces a compilable body for any CFG, and is the
// correct behavior when annotations are absent. The lib/Stmt builder must
// stay strictly better-or-equal than it: it may emit `goto` for anything it
// cannot structure.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_STMTBUILDER_H
#define REVENANT_CLANG_STMTBUILDER_H

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "llvm/IR/Function.h"
#include <memory>

namespace revenant::clangx {

class ExprBuilder;
struct ExprScope;
class FrontendContext;
class TypeBuilder;

class StmtBuilder {
public:
  virtual ~StmtBuilder() = default;

  /// Build `FD`'s body for `F` (CompoundStmt, never null). The ExprBuilder
  /// and `Scope` are supplied by the caller; `Scope` starts empty and is
  /// passed through every ExprBuilder call. All VarDecls ExprBuilder needs
  /// must be declared inside the returned body (or bound to `FD` params).
  virtual clang::CompoundStmt *buildBody(llvm::Function &F,
                                         clang::FunctionDecl *FD,
                                         ExprBuilder &Exprs,
                                         ExprScope &Scope) = 0;
};

/// Reference StmtBuilder in lib/Core/: emits a label per basic block and
/// one `goto`/cond-goto per terminator. Does not consume region
/// annotations; correct for every well-formed CFG including irreducible
/// ones. Serves as the body while lib/Stmt lands, and its per-terminator
/// emission is the fallback the structured builder should reuse.
class GotoStmtBuilder final : public StmtBuilder {
public:
  GotoStmtBuilder(FrontendContext &Ctx, TypeBuilder &Types)
      : Ctx(Ctx), Types(Types) {}

  clang::CompoundStmt *buildBody(llvm::Function &F, clang::FunctionDecl *FD,
                                 ExprBuilder &Exprs,
                                 ExprScope &Scope) override;

private:
  FrontendContext &Ctx;
  TypeBuilder &Types;
};

/// Construct the structured StmtBuilder (defined in lib/Stmt/).
std::unique_ptr<StmtBuilder> createStmtBuilder(FrontendContext &Ctx,
                                               TypeBuilder &Types);

} // namespace revenant::clangx

#endif // REVENANT_CLANG_STMTBUILDER_H
