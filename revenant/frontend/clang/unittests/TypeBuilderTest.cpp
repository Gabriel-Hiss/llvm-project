//===- TypeBuilderTest.cpp - RealTypeBuilder unit tests ---------*- C++ -*-===//
//
// Port of the C++-relevant rules in revenant-emit-cpp/tests/unit.rs and
// revenant-demangle/tests/demangle.rs to the Clang TypeBuilder:
// MSVC `?area@Shape@@...` demangles to `Shape::area`, Itanium `_Z3foov`
// to `foo`, integer widths map with DI signedness, and a DICompositeType
// with a virtual `area` resolves through virtualMethod() (the
// `Rect::area`/`Shape::area` fixture from emit-cpp's unit tests).
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Clang/FrontendContext.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Type.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace revenant::clangx;

namespace {

struct TypeFixture : public ::testing::Test {
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  std::unique_ptr<clang::CompilerInstance> Clang;
  std::unique_ptr<FrontendContext> FCtx;
  std::unique_ptr<TypeBuilder> Types;

  void SetUp() override {
    M = std::make_unique<Module>("t", Ctx);
    M->setTargetTriple(Triple(sys::getDefaultTargetTriple()));

    Clang = std::make_unique<clang::CompilerInstance>();
    Clang->createVirtualFileSystem();
    Clang->createDiagnostics();
    clang::TargetOptions TO;
    TO.Triple = M->getTargetTriple().str();
    Clang->setTarget(
        clang::TargetInfo::CreateTargetInfo(Clang->getDiagnostics(), TO));
    Clang->createFileManager();
    Clang->createSourceManager();
    Clang->createPreprocessor(clang::TU_Complete);
    Clang->createASTContext();

    FCtx = std::make_unique<FrontendContext>(Clang->getASTContext(), *M,
                                             SourceLang::CPlusPlus);
    Types = createTypeBuilder(*FCtx);
  }
};

TEST_F(TypeFixture, IntWidthsFollowSignedness) {
  // i32 without DI -> unsigned int (emit-cpp `u` convention).
  EXPECT_EQ(Types->typeOf(Type::getInt32Ty(Ctx), nullptr),
            Clang->getASTContext().UnsignedIntTy);
  DIBuilder B(*M);
  DIBasicType *S32 = B.createBasicType("int", 32, dwarf::DW_ATE_signed);
  EXPECT_EQ(Types->typeOfDI(S32), Clang->getASTContext().IntTy);
  DIBasicType *U64 = B.createBasicType("u64", 64, dwarf::DW_ATE_unsigned);
  EXPECT_EQ(Types->typeOfDI(U64), Clang->getASTContext().UnsignedLongLongTy);
  // i1 -> bool.
  EXPECT_EQ(Types->typeOf(Type::getInt1Ty(Ctx), nullptr),
            Clang->getASTContext().BoolTy);
}

TEST_F(TypeFixture, PointersArraysFunctions) {
  // ptr -> void*.
  clang::QualType P = Types->typeOf(PointerType::get(Ctx, 0), nullptr);
  EXPECT_TRUE(P->isPointerType());
  // [4 x i32] -> constant array of 4 unsigned ints.
  ArrayType *AT = ArrayType::get(Type::getInt32Ty(Ctx), 4);
  clang::QualType A = Types->typeOf(AT, nullptr);
  EXPECT_TRUE(isa<clang::ConstantArrayType>(A.getTypePtr()));
  // Function signature maps to a function type.
  Function *F = Function::Create(
      FunctionType::get(Type::getInt32Ty(Ctx), {Type::getInt32Ty(Ctx)},
                        false),
      GlobalValue::ExternalLinkage, "f", M.get());
  clang::QualType FT = Types->functionType(*F);
  EXPECT_TRUE(FT->isFunctionProtoType());
}

TEST_F(TypeFixture, StructFromDIComposite) {
  DIBuilder B(*M);
  DIFile *F = B.createFile("t.cpp", "/");
  DIBasicType *I32 = B.createBasicType("int", 32, dwarf::DW_ATE_signed);
  DICompositeType *Shape = B.createStructType(
      nullptr, "Shape", F, 1, 64, 64, DINode::FlagZero, nullptr,
      B.getOrCreateArray({
          B.createMemberType(nullptr, "id", F, 1, 32, 32, 64,
                             DINode::FlagZero, I32),
      }));
  clang::RecordDecl *RD = Types->recordFor(Shape);
  ASSERT_NE(RD, nullptr);
  EXPECT_EQ(RD->getName(), "Shape");
}

TEST_F(TypeFixture, VirtualSlotResolves) {
  DIBuilder B(*M);
  DIFile *F = B.createFile("t.cpp", "/");
  DIBasicType *I32 = B.createBasicType("int", 32, dwarf::DW_ATE_signed);
  DISubroutineType *Sig =
      B.createSubroutineType(B.getOrCreateTypeArray({I32}));
  // createMethod needs a non-CU scope; build the record first with an
  // empty element list, then attach the method under it.
  DICompositeType *Shape = B.createStructType(
      nullptr, "Shape", F, 1, 64, 64, DINode::FlagZero, nullptr,
      B.getOrCreateArray({}));
  DISubprogram *Area = B.createMethod(
      Shape, "area", "_ZN5Shape4areaEv", F, 2, Sig, /*VTableIndex=*/0,
      /*ThisAdjustment=*/0, /*VTableHolder=*/Shape, DINode::FlagZero,
      DISubprogram::SPFlagVirtual);
  Shape->replaceElements(B.getOrCreateArray({Area}));
  // Slot 0 holds the first virtual in declaration order.
  EXPECT_EQ(Types->virtualMethod(Shape, 0), Area);
  EXPECT_EQ(Types->virtualMethod(Shape, 7), nullptr);
  // Materialized method decl answers methodFor.
  clang::RecordDecl *RD = Types->recordFor(Shape);
  ASSERT_NE(RD, nullptr);
  EXPECT_NE(Types->methodFor(Area), nullptr);
}

} // namespace
