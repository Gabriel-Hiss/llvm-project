//===- revenant-ir-verify.cpp - check .ll against the IR contract ---------===//
//
// Parses a textual IR module, runs the stock LLVM verifier, then every rule
// from docs/ir-contract.md. Exit 0 clean, 1 contract violations, 2
// parse/IO error.
//
//===----------------------------------------------------------------------===//

#include "revenant/IR/Contract.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace revenant::ir;

static cl::opt<std::string> InputFile(cl::Positional, cl::Required,
                                      cl::desc("<input .ll>"));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv,
                              "revenant-ir-verify: check a .ll against the "
                              "frontend IR contract (docs/ir-contract.md)\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 2;
  }

  unsigned Violations = 0;
  auto Report = [&](StringRef Msg) {
    errs() << InputFile << ": error: " << Msg << "\n";
  };

  if (verifyModule(*M, &errs()))
    ++Violations;

  Violations += verifyModuleContract(*M, [&](ContractSeverity Sev,
                                             StringRef Msg) {
    switch (Sev) {
    case ContractSeverity::Error:
      Report(Msg);
      break;
    case ContractSeverity::Warning:
      errs() << InputFile << ": warning: " << Msg << "\n";
      break;
    }
  });

  if (Violations)
    return 1;
  return 0;
}
