#include "revenant/Clang/ClangFrontend.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"

#include "gtest/gtest.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/Decl.h"

#include <memory>

using namespace llvm;
using namespace revenant::clangx;

namespace {
class EnumSwitchTypeBuilder final : public TypeBuilder {
public:
  EnumSwitchTypeBuilder(FrontendContext &Ctx, clang::QualType EnumTy)
      : Fallback(Ctx), EnumTy(EnumTy) {}

  clang::QualType typeOf(llvm::Type *Ty, const llvm::DIType *DI) override {
    return Ty->isIntegerTy(32) ? EnumTy : Fallback.typeOf(Ty, DI);
  }
  clang::QualType typeOfDI(const llvm::DIType *DI) override {
    return Fallback.typeOfDI(DI);
  }
  clang::QualType functionType(llvm::Function &F) override {
    return Fallback.functionType(F);
  }
  clang::QualType globalType(llvm::GlobalVariable &GV) override {
    return Fallback.globalType(GV);
  }
  clang::RecordDecl *recordFor(const llvm::DICompositeType *DI) override {
    return Fallback.recordFor(DI);
  }
  const llvm::DISubprogram *
  virtualMethod(const llvm::DICompositeType *DI, unsigned Slot) override {
    return Fallback.virtualMethod(DI, Slot);
  }
  clang::CXXMethodDecl *methodFor(const llvm::DISubprogram *SP) override {
    return Fallback.methodFor(SP);
  }

private:
  NullTypeBuilder Fallback;
  clang::QualType EnumTy;
};
} // namespace

TEST(TranslationUnitTest, EmitsEnumSwitchCaseLiteralsAsIntegerTypes) {
  LLVMContext Context;
  Module M("enum-switch", Context);
  M.setTargetTriple("x86_64-pc-windows-msvc");
  Type *I32 = Type::getInt32Ty(Context);
  Function *F = Function::Create(FunctionType::get(I32, {I32}, false),
                                 Function::ExternalLinkage, "enum_switch", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  BasicBlock *Case = BasicBlock::Create(Context, "case", F);
  BasicBlock *Default = BasicBlock::Create(Context, "default", F);
  IRBuilder<> Builder(Entry);
  SwitchInst *Switch = Builder.CreateSwitch(F->getArg(0), Default, 1);
  Switch->addCase(ConstantInt::get(I32, 1), Case);
  Builder.SetInsertPoint(Case);
  Builder.CreateRet(ConstantInt::get(I32, 2));
  Builder.SetInsertPoint(Default);
  Builder.CreateRet(ConstantInt::get(I32, 0));

  ClangFrontendOptions Options;
  Options.Lang = "c";
  auto TUOrErr = createASTContext(M, Options);
  ASSERT_TRUE(TUOrErr) << toString(TUOrErr.takeError());
  std::unique_ptr<TranslationUnit> TU = std::move(*TUOrErr);
  auto *Enum = clang::EnumDecl::Create(
      TU->ast(), TU->tu(), clang::SourceLocation(), clang::SourceLocation(),
      TU->frontend().ident("Mode"), nullptr, false, false, true);
  Enum->startDefinition();
  Enum->completeDefinition(TU->ast().IntTy, TU->ast().IntTy, 32, 32);
  EnumSwitchTypeBuilder Types(TU->frontend(), TU->ast().getEnumType(Enum));
  std::unique_ptr<StmtBuilder> Stmts = createStmtBuilder(TU->frontend(), Types);
  std::unique_ptr<ExprBuilder> Exprs = createExprBuilder(TU->frontend(), Types);
  fillTranslationUnit(*TU, M, Types, *Stmts, *Exprs);

  SmallString<256> Text;
  raw_svector_ostream OS(Text);
  printTranslationUnit(TU->tu(), OS, {});
  EXPECT_NE(StringRef(OS.str()).find("case 1"), StringRef::npos);
}

TEST(TranslationUnitTest, EmitsPointerReturnFallbackWithPointerType) {
  LLVMContext Context;
  Module M("pointer-return", Context);
  M.setTargetTriple("x86_64-pc-windows-msvc");
  Type *PtrTy = PointerType::get(Context, 0);
  Function *F = Function::Create(FunctionType::get(PtrTy, false),
                                 Function::ExternalLinkage, "pointer_result", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  IRBuilder<> Builder(Entry);
  Builder.CreateRet(ConstantPointerNull::get(cast<PointerType>(PtrTy)));

  ClangFrontendOptions Options;
  Options.Lang = "c";
  auto TUOrErr = createASTContext(M, Options);
  ASSERT_TRUE(TUOrErr) << toString(TUOrErr.takeError());
  std::unique_ptr<TranslationUnit> TU = std::move(*TUOrErr);
  std::unique_ptr<TypeBuilder> Types = createTypeBuilder(TU->frontend());
  std::unique_ptr<StmtBuilder> Stmts = createStmtBuilder(TU->frontend(), *Types);
  std::unique_ptr<ExprBuilder> Exprs = createExprBuilder(TU->frontend(), *Types);
  fillTranslationUnit(*TU, M, *Types, *Stmts, *Exprs);

  SmallString<256> Text;
  raw_svector_ostream OS(Text);
  printTranslationUnit(TU->tu(), OS, {});
  EXPECT_NE(StringRef(OS.str()).find("return"), StringRef::npos);
}
