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

// A recovered main keeps its lifted arity: the hosted spelling must not add
// parameters the definition lacks (FunctionDecl::setParams asserts on that).
TEST(TranslationUnitTest, MainKeepsLiftedArity) {
  for (unsigned Arity : {0u, 1u, 2u}) {
    LLVMContext Context;
    Module M("main-arity", Context);
    M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
    Type *I32 = Type::getInt32Ty(Context);
    SmallVector<Type *, 2> Params(Arity, I32);
    if (Arity == 2)
      Params[1] = PointerType::get(Context, 0);
    Function *F = Function::Create(FunctionType::get(I32, Params, false),
                                   Function::ExternalLinkage, "main", M);
    IRBuilder<> Builder(BasicBlock::Create(Context, "entry", F));
    Builder.CreateRet(Arity ? static_cast<Value *>(F->getArg(0))
                            : ConstantInt::get(I32, 7));

    ClangFrontendOptions Options;
    Options.Lang = "c";
    auto TUOrErr = createASTContext(M, Options);
    if (!TUOrErr) {
      ADD_FAILURE() << toString(TUOrErr.takeError());
      return;
    }
    std::unique_ptr<TranslationUnit> TU = std::move(*TUOrErr);
    std::unique_ptr<TypeBuilder> Types = createTypeBuilder(TU->frontend());
    std::unique_ptr<StmtBuilder> Stmts =
        createStmtBuilder(TU->frontend(), *Types);
    std::unique_ptr<ExprBuilder> Exprs =
        createExprBuilder(TU->frontend(), *Types);
    fillTranslationUnit(*TU, M, *Types, *Stmts, *Exprs);

    SmallString<256> Text;
    raw_svector_ostream OS(Text);
    printTranslationUnit(TU->tu(), OS, {});
    StringRef Out = OS.str();
    size_t At = Out.find("int main(");
    ASSERT_NE(At, StringRef::npos) << Out;
    StringRef Sig = Out.substr(At + 9);
    Sig = Sig.take_until([](char C) { return C == ')'; });
    unsigned Printed =
        (Sig.empty() || Sig == "void") ? 0 : 1 + Sig.count(',');
    EXPECT_EQ(Printed, Arity) << Out;
  }
}

TEST(TranslationUnitTest, DeclaresCalleesBeforeWholeUnitDefinitions) {
  LLVMContext Context;
  Module M("forward-call", Context);
  M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  Type *I32 = Type::getInt32Ty(Context);
  FunctionType *FnTy = FunctionType::get(I32, false);
  Function *Caller = Function::Create(FnTy, Function::ExternalLinkage,
                                      "caller", M);
  Function *Callee = Function::Create(FnTy, Function::ExternalLinkage,
                                      "callee", M);
  BasicBlock *CallerEntry = BasicBlock::Create(Context, "entry", Caller);
  IRBuilder<> Builder(CallerEntry);
  Builder.CreateRet(Builder.CreateCall(Callee));
  BasicBlock *CalleeEntry = BasicBlock::Create(Context, "entry", Callee);
  Builder.SetInsertPoint(CalleeEntry);
  Builder.CreateRet(ConstantInt::get(I32, 7));

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
  StringRef Source = OS.str();
  size_t CallerProto = Source.find("caller(");
  size_t CalleeProto = Source.find("callee(");
  ASSERT_NE(CallerProto, StringRef::npos);
  ASSERT_NE(CalleeProto, StringRef::npos);
  size_t CallerDefinition = Source.find("caller(", CallerProto + 1);
  size_t CalleeDefinition = Source.find("callee(", CalleeProto + 1);
  ASSERT_NE(CallerDefinition, StringRef::npos);
  ASSERT_NE(CalleeDefinition, StringRef::npos);
  EXPECT_LT(CalleeProto, CallerDefinition);
  EXPECT_EQ(Source.count("caller("), 2u);
  EXPECT_EQ(Source.count("callee("), 3u);
}
TEST(TranslationUnitTest, CastsStringArgumentsToOpaquePointerParameters) {
  LLVMContext Context;
  Module M("opaque-string-argument", Context);
  M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  Type *PtrTy = PointerType::get(Context, 0);
  Type *VoidTy = Type::getVoidTy(Context);
  Function *Callee = Function::Create(
      FunctionType::get(VoidTy, {PtrTy}, false), Function::ExternalLinkage,
      "opaque_string", M);
  auto *Text = new GlobalVariable(
      M, ArrayType::get(Type::getInt8Ty(Context), 2), true,
      GlobalValue::PrivateLinkage,
      ConstantDataArray::getString(Context, "x", true), "str_opaque");
  Function *Caller = Function::Create(
      FunctionType::get(VoidTy, false), Function::ExternalLinkage, "caller",
      M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", Caller);
  IRBuilder<> Builder(Entry);
  Builder.CreateCall(Callee, {Text});
  Builder.CreateRetVoid();

  ClangFrontendOptions Options;
  Options.Lang = "cpp";
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

  SmallString<256> Source;
  raw_svector_ostream OS(Source);
  printTranslationUnit(TU->tu(), OS, {});
  EXPECT_NE(StringRef(OS.str()).find("(void *)\"x\""), StringRef::npos);
}

TEST(TranslationUnitTest, EmitsInfinityWithoutNonportableInfToken) {
  LLVMContext Context;
  Module M("infinity", Context);
  M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  Function *F = Function::Create(
      FunctionType::get(Type::getDoubleTy(Context), false),
      Function::ExternalLinkage, "infinity_value", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", F);
  IRBuilder<> Builder(Entry);
  Builder.CreateRet(ConstantFP::getInfinity(Type::getDoubleTy(Context)));

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
  EXPECT_EQ(StringRef(OS.str()).find("Inf"), StringRef::npos);
}

TEST(TranslationUnitTest, ArrayElementLiteralsAvoidNarrowTypeSuffixes) {
  LLVMContext Context;
  Module M("byte-table", Context);
  M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  // The gcc/clang targets hold their tables as narrow arrays; a literal
  // carrying the element type prints as `229Ui8`, which no target compiler
  // understands ("operator\"Ui8\" not found").
  Constant *Elems =
      ConstantDataArray::get(Context, ArrayRef<uint8_t>{229, 1, 0, 255});
  new GlobalVariable(M, ArrayType::get(Type::getInt8Ty(Context), 4), true,
                     GlobalValue::ExternalLinkage, Elems, "table");

  ClangFrontendOptions Options;
  // The element list path is what the C++ emitter uses: C mode spells a
  // fully narrow array as a string literal instead.
  Options.Lang = "cpp";
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
  StringRef Src = OS.str();
  EXPECT_EQ(Src.find("Ui8"), StringRef::npos);
  EXPECT_NE(Src.find("229"), StringRef::npos);
}

TEST(TranslationUnitTest, IndirectCallThroughDataSlotCastsCallee) {
  LLVMContext Context;
  Module M("slot-call", Context);
  M.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  Type *I64 = Type::getInt64Ty(Context);
  // The lifter keeps a code address as a data object when it cannot prove
  // the callee (`ops[which & 1](a, b)` in the corpus reaches its indirect
  // call this way), so the call names a slot, not a function.
  ArrayType *SlotTy = ArrayType::get(Type::getInt8Ty(Context), 16);
  GlobalVariable *Slot = new GlobalVariable(M, SlotTy, false,
                                            GlobalValue::ExternalLinkage,
                                            nullptr, "f_slot");
  Function *Caller =
      Function::Create(FunctionType::get(I64, {I64, I64, I64}, false),
                       Function::ExternalLinkage, "caller", M);
  BasicBlock *Entry = BasicBlock::Create(Context, "entry", Caller);
  IRBuilder<> Builder(Entry);
  Value *A = Caller->getArg(0), *B = Caller->getArg(1),
        *C = Caller->getArg(2);
  Builder.CreateRet(Builder.CreateCall(
      FunctionType::get(I64, {I64, I64, I64}, false), Slot, {A, B, C}));

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
  StringRef Src = OS.str();
  // `(*data)(args)` and `data(args)` are both rejected by every target
  // compiler: the callee must be spelled through the call's own function
  // type.
  EXPECT_EQ(Src.find("(*f_slot)"), StringRef::npos);
  EXPECT_NE(Src.find("(*("), StringRef::npos);
  EXPECT_EQ(Src.find("return (long long)f_slot("), StringRef::npos);
}
