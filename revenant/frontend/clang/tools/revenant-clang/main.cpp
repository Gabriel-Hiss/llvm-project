//===- main.cpp - revenant-clang: contract .ll -> C/C++ ---------*- C++ -*-===//
//
// Usage: revenant-clang in.ll [-o out.c|out.cpp] [--lang=c|cpp] [--goto]
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/ClangFrontend.h"
#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Triple.h"
using namespace llvm;
using namespace revenant::clangx;

static cl::opt<std::string> Input(cl::Positional, cl::desc("<input.ll>"),
                                  cl::Required);
static cl::opt<std::string> Output("o", cl::desc("output file (default stdout)"),
                                   cl::value_desc("file"));
static cl::opt<std::string> Lang("lang", cl::desc("output language: c|cpp"),
                                 cl::value_desc("lang"));
static cl::opt<bool> ForceGoto("goto",
                               cl::desc("emit goto-based bodies even when "
                                        "region annotations exist"));
static cl::list<std::string>
    Includes("include", cl::desc("extra #include <h> lines"),
             cl::value_desc("header"), cl::CommaSeparated);

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv,
                              "revenant-clang: Revenant IR -> C/C++\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseIRFile(Input, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  ClangFrontendOptions Opts;
  Opts.Lang = Lang;
  Opts.ForceGoto = ForceGoto;

  auto TOrErr = createASTContext(*M, Opts);
  if (!TOrErr) {
    errs() << "revenant-clang: " << toString(TOrErr.takeError()) << "\n";
    return 1;
  }
  std::unique_ptr<TranslationUnit> T = std::move(*TOrErr);

  auto Types = createTypeBuilder(T->frontend());
  auto Stmts = createStmtBuilder(T->frontend(), *Types);
  auto Exprs = createExprBuilder(T->frontend(), *Types);
  fillTranslationUnit(*T, *M, *Types, *Stmts, *Exprs);

  SmallVector<StringRef, 8> Inc;
  for (auto &S : Includes)
    Inc.push_back(S);
  if (Lang == "c" || (Lang.empty() && !T->frontend().isCPlusPlus())) {
    // The stub path prints fixed-width ints; give output the typedefs.
    for (StringRef S : {"stdint.h", "stddef.h", "stdbool.h"})
      if (llvm::find(Inc, S) == Inc.end())
        Inc.insert(Inc.begin(), S);
  }
  // __fastfail/__debugbreak are spelled as the MSVC intrinsics and live in
  // <intrin.h>; on the MSVC ABI family that header is the only declaration
  // that links (a plain prototype gives LNK2019).
  if (llvm::Triple(M->getTargetTriple()).isKnownWindowsMSVCEnvironment() &&
      llvm::find(Inc, StringRef("intrin.h")) == Inc.end())
    Inc.push_back("intrin.h");

  if (Output.empty()) {
    printTranslationUnit(T->tu(), outs(), Inc);
    return 0;
  }
  std::error_code EC;
  llvm::ToolOutputFile Out(Output, EC, llvm::sys::fs::OF_None);
  if (EC) {
    errs() << "revenant-clang: " << Output << ": " << EC.message() << "\n";
    return 1;
  }
  printTranslationUnit(T->tu(), Out.os(), Inc);
  Out.keep();
  return 0;
}
