#include "revenant/Analysis/Signatures.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "llvm/DebugInfo/PDB/IPDBEnumChildren.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/PDBSymbolData.h"
#include "llvm/DebugInfo/PDB/PDBSymbolFunc.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::pdb;
using namespace revenant;

static cl::opt<std::string> Binary(cl::Positional, cl::Required,
                                   cl::desc("input PE binary"));
static cl::opt<std::string> PdbPath(cl::Positional, cl::Required,
                                    cl::desc("matching PDB file"));
static cl::opt<std::string> CsvPath(cl::Positional, cl::Required,
                                    cl::desc("recovered signatures CSV"));

static bool buildPdataProgram(const PeFile &PE, const Disasm &Dis,
                              Program &P) {
  P.Img = PE.Img;
  P.Externs = std::make_shared<std::map<Va, Extern>>();
  P.Names = std::make_shared<std::map<Va, std::string>>();
  for (const RuntimeFunction &RF : PE.Pdata) {
    if (RF.ChainedTo || RF.End <= RF.Begin)
      continue;
    auto StartRva = RF.Begin.offsetFrom(PE.Img->Base);
    auto EndRva = RF.End.offsetFrom(PE.Img->Base);
    if (!StartRva || !EndRva || *StartRva > UINT32_MAX ||
        *EndRva > UINT32_MAX)
      continue;

    FuncRecord F;
    F.Start = RF.Begin;
    F.End = RF.End;
    F.HasPdata = true;
    F.Chunks.push_back({RF.Begin, RF.End});
    for (Va IP = RF.Begin; IP < RF.End;) {
      auto Ins = Dis.decodeOne(IP);
      if (!Ins || Ins->Size == 0)
        break;
      auto Rva = IP.offsetFrom(PE.Img->Base);
      if (!Rva || *Rva > UINT32_MAX)
        break;
      F.InstrRvas.push_back(static_cast<uint32_t>(*Rva));
      IP = Ins->nextVa();
    }
    if (F.InstrRvas.empty())
      continue;

    BlockRec B;
    B.Start = static_cast<uint32_t>(*StartRva);
    B.End = static_cast<uint32_t>(*EndRva);
    B.InstrsBegin = 0;
    B.InstrsEnd = F.InstrRvas.size();
    B.EndKind = BlockEnd::Return;
    F.Blocks.push_back(B);
    P.Functions.emplace(F.Start, std::move(F));
  }
  return true;
}

static unsigned pdbParameterCount(IPDBSession &Session, uint32_t Rva) {
  std::unique_ptr<PDBSymbol> Symbol =
      Session.findSymbolByRVA(Rva, PDB_SymType::Function);
  auto *Func = dyn_cast_or_null<PDBSymbolFunc>(Symbol.get());
  if (!Func)
    return UINT32_MAX;
  auto Args = Func->getArguments();
  return Args ? Args->getChildCount() : 0;
}

int main(int Argc, char **Argv) {
  cl::ParseCommandLineOptions(Argc, Argv,
                              "Compare inferred signatures with PDB truth.\n");
  auto PEOrErr = PeFile::load(Binary);
  if (!PEOrErr) {
    logAllUnhandledErrors(PEOrErr.takeError(), errs(), "PE load failed: ");
    return 1;
  }
  PeFile PE = std::move(*PEOrErr);
  auto DisOrErr = Disasm::create(*PE.Img);
  if (!DisOrErr) {
    logAllUnhandledErrors(DisOrErr.takeError(), errs(), "decoder setup failed: ");
    return 1;
  }
  Disasm Dis = std::move(*DisOrErr);
  Program P;
  buildPdataProgram(PE, Dis, P);
  Signatures Sigs = analyzeSignatures(P, Dis);

  std::unique_ptr<IPDBSession> Session;
  if (Error E = NativeSession::createFromPdbPath(PdbPath, Session)) {
    logAllUnhandledErrors(std::move(E), errs(), "PDB load failed: ");
    return 1;
  }
  Session->setLoadAddress(PE.Img->Base.Value);

  std::error_code EC;
  raw_fd_ostream CSV(CsvPath, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "cannot open " << CsvPath << ": " << EC.message() << '\n';
    return 1;
  }
  CSV << "function_va,param_count,return_kind\n";
  uint64_t Compared = 0, Matched = 0, MissingPdb = 0;
  for (const auto &[VA, Sig] : Sigs.Funcs) {
    auto RVA = VA.offsetFrom(PE.Img->Base);
    if (!RVA || *RVA > UINT32_MAX)
      continue;
    unsigned PdbCount = pdbParameterCount(*Session, static_cast<uint32_t>(*RVA));
    const char *ReturnKind = !Sig.HasRet ? "none"
                             : Sig.Ret.Float ? "float"
                                             : "integer";
    CSV << format_hex_no_prefix(VA.Value, 0, true) << ',' << Sig.Params.size()
        << ',' << ReturnKind << '\n';
    if (PdbCount == UINT32_MAX) {
      ++MissingPdb;
      continue;
    }
    ++Compared;
    Matched += PdbCount == Sig.Params.size();
  }
  outs() << "PDB parameter-count agreement: " << Matched << '/' << Compared
         << " (" << MissingPdb << " functions without a matching PDB function)\n"
         << "CSV: " << CsvPath << '\n';
  return 0;
}
