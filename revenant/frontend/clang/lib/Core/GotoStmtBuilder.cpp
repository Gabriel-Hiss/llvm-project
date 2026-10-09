//===- GotoStmtBuilder.cpp - label+goto reference StmtBuilder ---*- C++ -*-===//
//
// Reference StmtBuilder: one label per block, instructions via ExprBuilder,
// terminators as goto/cond-goto/switch-goto. Always valid C for any CFG.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/FrontendContext.h"
#include "revenant/Clang/TypeBuilder.h"
#include "clang/AST/Expr.h"
#include "llvm/IR/Instructions.h"

using llvm::APInt;
using llvm::DenseMap;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;

namespace revenant::clangx {

namespace c = clang;

c::CompoundStmt *GotoStmtBuilder::buildBody(llvm::Function &F,
                                            c::FunctionDecl *FD,
                                            ExprBuilder &Exprs,
                                            ExprScope &Scope) {
  c::ASTContext &AST = Ctx.ast();
  Exprs.beginFunction(F, FD, Scope);

  SmallVector<c::Stmt *, 32> Stmts;
  DenseMap<const llvm::BasicBlock *, c::LabelDecl *> Labels;
  auto labelFor = [&](const llvm::BasicBlock &BB) -> c::LabelDecl * {
    auto It = Labels.find(&BB);
    if (It != Labels.end())
      return It->second;
    std::string Name = BB.getName().str();
    if (Name.empty())
      Name = (Twine("bb_") + Twine(Labels.size())).str();
    c::LabelDecl *LD = c::LabelDecl::Create(AST, FD, c::SourceLocation(),
                                            Ctx.ident(Name));
    Labels[&BB] = LD;
    return LD;
  };
  auto gotoTo = [&](llvm::BasicBlock *BB) -> c::GotoStmt * {
    return new (AST)
        c::GotoStmt(labelFor(*BB), c::SourceLocation(), c::SourceLocation());
  };

  SmallVector<c::Stmt *, 8> BlockStmts;
  for (llvm::BasicBlock &BB : F) {
    BlockStmts.clear();
    for (llvm::Instruction &I : BB) {
      if (I.isTerminator())
        continue;
      if (c::Stmt *S = Exprs.instr(I, Scope))
        BlockStmts.push_back(S);
    }

    c::Stmt *Last = nullptr;
    llvm::Instruction *T = BB.getTerminator();
    if (auto *BI = llvm::dyn_cast<llvm::BranchInst>(T)) {
      if (BI->isUnconditional())
        Last = gotoTo(BI->getSuccessor(0));
      else
        Last = c::IfStmt::Create(
            AST, c::SourceLocation(), c::IfStatementKind::Ordinary, nullptr,
            nullptr, Exprs.cond(BI->getCondition(), Scope),
            c::SourceLocation(), c::SourceLocation(),
            gotoTo(BI->getSuccessor(0)), c::SourceLocation(),
            gotoTo(BI->getSuccessor(1)));
    } else if (auto *SI = llvm::dyn_cast<llvm::SwitchInst>(T)) {
      c::Expr *Cond = Exprs.cond(SI->getCondition(), Scope);
      c::SwitchStmt *SW =
          c::SwitchStmt::Create(AST, nullptr, nullptr, Cond,
                                c::SourceLocation(), c::SourceLocation());
      SmallVector<c::Stmt *, 8> Cases;
      for (auto &Case : SI->cases()) {
        c::QualType CT = Cond ? Cond->getType() : AST.IntTy;
        llvm::APInt CaseValue =
            Case.getCaseValue()->getValue().sextOrTrunc(AST.getIntWidth(CT));
        c::Expr *LH = c::IntegerLiteral::Create(
            AST, CaseValue, CT, c::SourceLocation());
        c::CaseStmt *CS = c::CaseStmt::Create(
            AST, LH, nullptr, c::SourceLocation(), c::SourceLocation(),
            c::SourceLocation());
        CS->setSubStmt(gotoTo(Case.getCaseSuccessor()));
        Cases.push_back(CS);
      }
      if (llvm::BasicBlock *Def = SI->getDefaultDest())
        Cases.push_back(new (AST) c::DefaultStmt(
            c::SourceLocation(), c::SourceLocation(), gotoTo(Def)));
      SW->setBody(c::CompoundStmt::Create(AST, Cases, c::FPOptionsOverride(),
                                          c::SourceLocation(),
                                          c::SourceLocation()));
      Last = SW;
    } else if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(T)) {
      Last = c::ReturnStmt::Create(AST, c::SourceLocation(),
                                   Exprs.retVal(RI->getReturnValue(), Scope),
                                   nullptr);
    } else {
      // unreachable, indirectbr, callbr, invoke, catch*: empty statement; a
      // structured builder or ExprBuilder can do better.
      Last = new (AST) c::NullStmt(c::SourceLocation(), false);
    }
    BlockStmts.push_back(Last);

    // Wrap the block's statements in a compound after the label so the
    // label labels a statement (labels on declarations are illegal in C).
    c::CompoundStmt *Inner =
        c::CompoundStmt::Create(AST, BlockStmts, c::FPOptionsOverride(),
                                c::SourceLocation(), c::SourceLocation());
    Stmts.push_back(new (AST) c::LabelStmt(c::SourceLocation(),
                                           labelFor(BB), Inner));
  }

  return c::CompoundStmt::Create(AST, Stmts, c::FPOptionsOverride(),
                                 c::SourceLocation(), c::SourceLocation());
}

} // namespace revenant::clangx
