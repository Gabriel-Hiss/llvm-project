//===- rv-structure-tool.cpp - lit runner for RevenantStructure -----------===//
//
// Reads an LLVM IR module, runs RevenantStructurePass on every function,
// and prints the annotated module on stdout:
//
//   rv-structure-tool input.ll -o - | FileCheck input.ll
//
//===----------------------------------------------------------------------===//

#include "revenant/Structure/RevenantStructure.h"

#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input .ll>"),
                                          cl::init("-"));
static cl::opt<std::string> OutputFilename("o", cl::desc("output"),
                                           cl::init("-"));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "revenant structure test runner\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  for (Function &F : *M) {
    if (F.isDeclaration())
      continue;
    DominatorTree DT(F);
    PostDominatorTree PDT;
    PDT.recalculate(F);
    LoopInfo LI(DT);
    revenant::annotateFunction(F, DT, PDT, LI);
  }

  std::error_code EC;
  raw_fd_ostream Out(OutputFilename, EC, sys::fs::OF_None);
  if (EC) {
    errs() << argv[0] << ": " << EC.message() << "\n";
    return 1;
  }
  M->print(Out, nullptr);
  return 0;
}
