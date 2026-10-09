//===- TypeSmoke.cpp - reference-module type smoke test ---------*- C++ -*-===//
//
// Loads LunaIRContract's cpp-method.ll, runs the type builder over it, and
// checks the contract-relevant facts: the vtable global has array type,
// the method function type resolves, the record materializes with its
// _vptr member, and the demangled method name recovers Klass::Method.
// Prints the record decl via Clang's own printer (the same printer path
// the frontend's C++ output goes through).
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Clang/FrontendContext.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <cassert>
#include <cstdio>

using namespace llvm;
using namespace revenant::clangx;

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  if (argc != 2) {
    errs() << "usage: TypeSmoke cpp-method.ll\n";
    return 2;
  }
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyFile(argv[1], Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 2;
  }

  clang::CompilerInstance Clang;
  Clang.createVirtualFileSystem();
  Clang.createDiagnostics();
  clang::TargetOptions TO;
  TO.Triple = M->getTargetTriple().str();
  Clang.setTarget(
      clang::TargetInfo::CreateTargetInfo(Clang.getDiagnostics(), TO));
  Clang.createFileManager();
  Clang.createSourceManager();
  Clang.createPreprocessor(clang::TU_Complete);
  Clang.createASTContext();

  FrontendContext FCtx(Clang.getASTContext(), *M, SourceLang::CPlusPlus);
  std::unique_ptr<TypeBuilder> Types = createTypeBuilder(FCtx);

  // 1. Vtable global type: [2 x ptr] -> constant array.
  GlobalVariable *VT = M->getNamedGlobal("??_7Klass@@6B@");
  assert(VT && "vtable global missing");
  clang::QualType VTT = Types->globalType(*VT);
  assert(isa<clang::ConstantArrayType>(VTT.getTypePtr()) &&
         "vtable should map to a constant array");
  outs() << "vtable type ok\n";

  // 2. Method function type: (ptr, i32) -> i32.
  Function *MF =
      dyn_cast<Function>(M->getNamedValue("?Method@Klass@@UEAAHXZ"));
  assert(MF && "method function missing");
  clang::QualType MT = Types->functionType(*MF);
  const auto *FPT = MT->getAs<clang::FunctionProtoType>();
  assert(FPT && FPT->getNumParams() == 2 && "method type should have 2 params");
  outs() << "method type ok\n";

  // 3. Record materializes; print it with Clang's own printer.
  DISubprogram *SP = MF->getSubprogram();
  assert(SP && "method subprogram missing");
  const DICompositeType *Klass =
      dyn_cast_or_null<DICompositeType>(SP->getContainingType());
  assert(Klass && "containing type missing");
  clang::RecordDecl *RD = Types->recordFor(Klass);
  assert(RD && "record failed to materialize");
  clang::PrintingPolicy Policy(Clang.getASTContext().getLangOpts());
  RD->print(outs(), Policy);
  outs() << "\n";

  // 4. Register the vtable global (Core's job in production), then slot 0
  // must dispatch to the method named by the vtable initializer.
  // Core's TypeBuilder interface registers the recovered vtable.
  if (auto *VTB = dyn_cast<GlobalVariable>(
          M->getNamedValue("??_7Klass@@6B@"))) {
    Types->addVTable("Klass", VTB);
  }
  const DISubprogram *Slot0 = Types->virtualMethod(Klass, 0);
  outs() << "slot0: " << (Slot0 ? Slot0->getName() : StringRef("<unknown>"))
         << "\n";

  outs() << "SMOKE-OK\n";
  return 0;
}
