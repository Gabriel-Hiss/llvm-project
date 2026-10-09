//===- ExprBuilder.h - LLVM IR instruction -> clang::Expr -------*- C++ -*-===//
//
// ExprBuilder (owner: MuseClangExpr, lib/Expr/) lifts a single contract
// function's instructions to Clang expressions/statements. StmtBuilder owns
// the CFG walk and calls these entry points; ExprBuilder owns value
// resolution (constants, DeclRefExpr for bound decls/temporaries) and the
// side table of instructions that must be materialized as temporaries.
//
// Virtual dispatch: when a call's callee is `load (gep (load this), slot)`
// over a record TypeBuilder knows, ExprBuilder should prefer
// TypeBuilder::virtualMethod()/methodFor() and emit a direct method call on
// `this` (CXXMemberCallExpr path) instead of a raw function-pointer call.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_EXPRBUILDER_H
#define REVENANT_CLANG_EXPRBUILDER_H

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

namespace revenant::clangx {

class FrontendContext;
class TypeBuilder;

/// Per-function state ExprBuilder accumulates while building. Created by
/// StmtBuilder::buildBody, passed back through the ExprBuilder methods.
struct ExprScope {
  /// Instruction results that need local `v<N>` variables. Core emits their
  /// declarations in the function prologue after the body has been built.
  llvm::SmallVector<llvm::Instruction *, 16> PendingTemps;
  /// Expressions computed for materialized results. instr() emits the
  /// initializer assignment at the defining instruction when applicable.
  llvm::DenseMap<llvm::Instruction *, clang::Expr *> PendingInitializers;
};

class ExprBuilder {
public:
  virtual ~ExprBuilder() = default;

  /// Per-function hook: called once before any instruction is built.
  /// Implementations may scan the function (collect dbg info, decide which
  /// instructions need temporaries, cache the `this` pointer's record DI).
  virtual void beginFunction(llvm::Function &F, clang::FunctionDecl *FD,
                             ExprScope &Scope) = 0;

  /// Build the expression an llvm::Value denotes: ConstantInt/FP ->
  /// literal, GlobalVariable -> DeclRefExpr of its VarDecl, Argument ->
  /// ParmVarDecl ref, AllocaInst -> its VarDecl ref, Instruction -> inline
  /// expression or bound temporary ref. Never null; unresolved values
  /// produce a `uintptr_t` constant of their best-known value.
  virtual clang::Expr *value(llvm::Value *V, ExprScope &Scope) = 0;

  /// One non-terminator instruction -> a statement for the enclosing
  /// block (typically a BinaryOperator assignment into the result's
  /// temporary, a CallExpr statement, a store lowering, or a DeclStmt).
  /// Terminators are never passed here; StmtBuilder owns control flow.
  /// May return nullptr for instructions fully folded into their uses.
  virtual clang::Stmt *instr(llvm::Instruction &I, ExprScope &Scope) = 0;

  /// Condition expression for a BranchInst/SwitchInst terminator, or for a
  /// loop/if annotation on a terminator. `Cond` is the IR condition value.
  virtual clang::Expr *cond(llvm::Value *Cond, ExprScope &Scope) = 0;

  /// Return value expression for a ReturnInst operand (nullptr operand ->
  /// nullptr out).
  virtual clang::Expr *retVal(llvm::Value *V, ExprScope &Scope) = 0;

  /// Loop-canonicalization hooks for StmtBuilder's `for` regions. All have
  /// defaults so implementations may ignore them: then `for` regions print
  /// as `while` + tail step, which is still correct.

  /// True when `I` must not be emitted as a body statement because it was
  /// folded into a for-header init/step (or is otherwise consumed by the
  /// structured statement being built). Default: never suppress.
  virtual bool suppress(llvm::Instruction &I, ExprScope &Scope) {
    return false;
  }

  /// Canonical `for` init: the preheader tail assignment feeding the loop's
  /// induction variable, or nullptr when there is none.
  virtual clang::Stmt *forInit(llvm::BasicBlock &Preheader,
                               llvm::BasicBlock &Header, ExprScope &Scope) {
    return nullptr;
  }

  /// Canonical `for` step: the latch tail update expression for the
  /// induction variable, or nullptr when there is none.
  virtual clang::Expr *forStep(llvm::BasicBlock &Latch, ExprScope &Scope) {
    return nullptr;
  }

  /// Canonical negation of a condition: predicate flip on icmp, else `!x`.
  virtual clang::Expr *inverted(llvm::Value *Cond, ExprScope &Scope) = 0;
};

/// Construct the real ExprBuilder implementation (defined in lib/Expr/).
/// Until that library links, Core uses the declaration below only through
/// this factory so the interface is exercised end to end.
std::unique_ptr<ExprBuilder> createExprBuilder(FrontendContext &Ctx,
                                               TypeBuilder &Types);

} // namespace revenant::clangx

#endif // REVENANT_CLANG_EXPRBUILDER_H
