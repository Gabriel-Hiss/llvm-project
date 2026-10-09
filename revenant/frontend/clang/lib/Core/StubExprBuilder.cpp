//===- StubExprBuilder.cpp - minimal ExprBuilder ----------------*- C++ -*-===//
//
// Fallback ExprBuilder Core installs until lib/Expr lands. It materializes
// every producing instruction as a typed temporary `v<N>` initialized from
// a trivial lowering (constants stay constants, direct calls stay calls,
// the rest become 0), so GotoStmtBuilder emits complete, compilable bodies.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/FrontendContext.h"
#include "revenant/Clang/TypeBuilder.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"

using llvm::APInt;
using llvm::SmallVector;
using llvm::Twine;

namespace revenant::clangx {

namespace c = clang;

class StubExprBuilder final : public ExprBuilder {
public:
  StubExprBuilder(FrontendContext &Ctx, TypeBuilder &Types)
      : Ctx(Ctx), Types(Types) {}

  void beginFunction(llvm::Function &F, c::FunctionDecl *FD,
                     ExprScope &) override {
    this->FD = FD;
    Counter = 0;
  }

  c::Expr *value(llvm::Value *V, ExprScope &Scope) override {
    c::ASTContext &AST = Ctx.ast();
    c::SourceLocation L;
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
      c::QualType T = Types.typeOf(CI->getType(), nullptr);
      APInt Value = CI->getValue().sextOrTrunc(AST.getIntWidth(T));
      return c::IntegerLiteral::Create(AST, Value, T, L);
    }
    if (auto *CF = llvm::dyn_cast<llvm::ConstantFP>(V)) {
      c::QualType T = Types.typeOf(CF->getType(), nullptr);
      return c::FloatingLiteral::Create(AST, CF->getValueAPF(), true, T, L);
    }
    if (llvm::isa<llvm::ConstantPointerNull>(V))
      return c::IntegerLiteral::Create(AST, APInt(64, 0),
                                       AST.UnsignedLongLongTy, L);
    if (auto *D = Ctx.lookupDecl(V))
      return declRef(D);
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V))
      return declRef(tempFor(I));
    return c::IntegerLiteral::Create(AST, APInt(64, 0),
                                     AST.UnsignedLongLongTy, L);
  }

  c::Stmt *instr(llvm::Instruction &I, ExprScope &Scope) override {
    c::ASTContext &AST = Ctx.ast();
    c::SourceLocation L;
    if (llvm::isa<llvm::AllocaInst>(I) || llvm::isa<llvm::DbgInfoIntrinsic>(I))
      return nullptr;
    bool ProducesValue = !I.getType()->isVoidTy();

    c::Expr *RHS = nullptr;
    if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
      llvm::Function *Callee = CI->getCalledFunction();
      if (Callee) {
        SmallVector<c::Expr *, 8> Args;
        for (auto &A : CI->args())
          Args.push_back(value(A.get(), Scope));
        c::FunctionDecl *CD = Ctx.functionDecl(*Callee, Types);
        c::Expr *Fn = declRef(CD);
        c::QualType RT =
            CD->getType()->castAs<c::FunctionType>()->getReturnType();
        RHS = c::CallExpr::Create(AST, Fn, Args, RT, c::VK_PRValue, L,
                                  c::FPOptionsOverride());
      }
    }
    if (!RHS) {
      if (!ProducesValue)
        return new (AST) c::NullStmt(L, false);
      c::QualType T = Types.typeOf(I.getType(), nullptr);
      if (T.isNull())
        T = AST.UnsignedLongLongTy;
      RHS = c::IntegerLiteral::Create(AST, APInt(AST.getIntWidth(T), 0), T, L);
    }

    if (!ProducesValue)
      return RHS;

    c::VarDecl *Tmp = tempFor(&I);
    Tmp->setInit(RHS);
    Tmp->setInitStyle(c::VarDecl::CInit);
    return new (AST) c::DeclStmt(c::DeclGroupRef(Tmp), L, L);
  }

  c::Expr *cond(llvm::Value *V, ExprScope &Scope) override {
    return value(V, Scope);
  }

  c::Expr *retVal(llvm::Value *V, ExprScope &Scope) override {
    if (!V)
      return nullptr;
    return value(V, Scope);
  }

  c::Expr *inverted(llvm::Value *Cond, ExprScope &Scope) override {
    c::ASTContext &AST = Ctx.ast();
    if (auto *ICI = llvm::dyn_cast<llvm::ICmpInst>(Cond)) {
      c::Expr *L = value(ICI->getOperand(0), Scope);
      c::Expr *R = value(ICI->getOperand(1), Scope);
      c::BinaryOperatorKind Op;
      switch (ICI->getInversePredicate()) {
      case llvm::CmpInst::ICMP_EQ: Op = c::BO_EQ; break;
      case llvm::CmpInst::ICMP_NE: Op = c::BO_NE; break;
      case llvm::CmpInst::ICMP_UGT:
      case llvm::CmpInst::ICMP_SGT: Op = c::BO_GT; break;
      case llvm::CmpInst::ICMP_UGE:
      case llvm::CmpInst::ICMP_SGE: Op = c::BO_GE; break;
      case llvm::CmpInst::ICMP_ULT:
      case llvm::CmpInst::ICMP_SLT: Op = c::BO_LT; break;
      case llvm::CmpInst::ICMP_ULE:
      case llvm::CmpInst::ICMP_SLE: Op = c::BO_LE; break;
      default: Op = c::BO_NE; break;
      }
      return c::BinaryOperator::Create(AST, L, R, Op, AST.IntTy,
                                       c::VK_PRValue, c::OK_Ordinary,
                                       c::SourceLocation(),
                                       c::FPOptionsOverride());
    }
    c::Expr *E = value(Cond, Scope);
    return c::UnaryOperator::Create(AST, E, c::UO_LNot, AST.IntTy,
                                    c::VK_PRValue, c::OK_Ordinary,
                                    c::SourceLocation(), false,
                                    c::FPOptionsOverride());
  }

private:
  c::Expr *declRef(c::ValueDecl *D) {
    c::QualType T = D->getType();
    c::ExprValueKind VK =
        llvm::isa<c::FunctionDecl>(D) ? c::VK_PRValue : c::VK_LValue;
    return c::DeclRefExpr::Create(Ctx.ast(),
                                  c::NestedNameSpecifierLoc(),
                                  c::SourceLocation(), D, false,
                                  c::SourceLocation(), T, VK);
  }

  c::VarDecl *tempFor(llvm::Instruction *I) {
    if (auto *D = llvm::dyn_cast_or_null<c::VarDecl>(Ctx.lookupDecl(I)))
      return D;
    c::QualType T = Types.typeOf(I->getType(), nullptr);
    if (T.isNull())
      T = Ctx.ast().UnsignedLongLongTy;
    c::VarDecl *VD = c::VarDecl::Create(
        Ctx.ast(), FD, c::SourceLocation(), c::SourceLocation(),
        Ctx.ident(("v" + Twine(Counter++)).str()), T, nullptr, c::SC_None);
    Ctx.bindTemp(I, VD);
    return VD;
  }

  FrontendContext &Ctx;
  TypeBuilder &Types;
  c::FunctionDecl *FD = nullptr;
  unsigned Counter = 0;
};

#ifndef REVENANT_FRONTEND_EXTERNAL_EXPR
std::unique_ptr<ExprBuilder> createExprBuilder(FrontendContext &Ctx,
                                               TypeBuilder &Types) {
  return std::make_unique<StubExprBuilder>(Ctx, Types);
}
#endif

} // namespace revenant::clangx
