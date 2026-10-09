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
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/Decl.h"

#include <memory>

using namespace llvm;
using namespace revenant::clangx;



TEST(TranslationUnitTest, EmitsPointerReturnFallbackWithPointerType) {
  LLVMContext Context;
  Module M("pointer-return", Context);
  M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  Type *PtrTy = PointerType::get(Context, 0);
  Function *F = Function::Create(FunctionType::get(PtrTy, false),
                                 Function::ExternalLinkage, "pointer_result", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  IRBuilder<> Builder(Entry);
  Builder.CreateRet(ConstantPointerNull::get(cast<PointerType>(PtrTy)));

  ClangFrontendOptions Options;
  Options.Lang = "c";
  auto TUOrErr = createASTContext(M, Options);
  if (!TUOrErr) {
    ADD_FAILURE() << toString(TUOrErr.takeError());
    return;
  }
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
