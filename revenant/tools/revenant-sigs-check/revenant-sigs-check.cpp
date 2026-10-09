#include "revenant/Analysis/Analysis.h"
#include "revenant/Analysis/Signatures.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <optional>
#include <utility>
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


static std::optional<std::pair<unsigned, bool>>
functionTypeParameters(TpiStream &TPI, TpiStream &IPI,
                       codeview::TypeIndex TI, bool IsId) {
  using namespace codeview;
  if (!IsId && TI.getIndex() >= TPI.TypeIndexBegin() &&
      TI.getIndex() < TPI.TypeIndexEnd()) {
    CVType Type = TPI.getType(TI);
    if (Type.kind() == TypeLeafKind::LF_PROCEDURE) {
      ProcedureRecord Record;
      if (Error E = TypeDeserializer::deserializeAs(Type, Record)) {
        consumeError(std::move(E));
        return std::nullopt;
      }
      return std::pair<unsigned, bool>(Record.getParameterCount(), false);
    }
    if (Type.kind() == TypeLeafKind::LF_MFUNCTION) {
      MemberFunctionRecord Record;
      if (Error E = TypeDeserializer::deserializeAs(Type, Record)) {
        consumeError(std::move(E));
        return std::nullopt;
      }
      return std::pair<unsigned, bool>(Record.getParameterCount(), true);
    }
    return std::nullopt;
  }
  if (!IsId || TI.getIndex() < IPI.TypeIndexBegin() ||
      TI.getIndex() >= IPI.TypeIndexEnd())
    return std::nullopt;
  CVType IdType = IPI.getType(TI);
  TypeIndex FunctionType;
  bool IsMember = false;
  if (IdType.kind() == TypeLeafKind::LF_FUNC_ID) {
    FuncIdRecord Record;
    if (Error E = TypeDeserializer::deserializeAs(IdType, Record)) {
      consumeError(std::move(E));
      return std::nullopt;
    }
    FunctionType = Record.getFunctionType();
  } else if (IdType.kind() == TypeLeafKind::LF_MFUNC_ID) {
    MemberFuncIdRecord Record;
    if (Error E = TypeDeserializer::deserializeAs(IdType, Record)) {
      consumeError(std::move(E));
      return std::nullopt;
    }
    FunctionType = Record.getFunctionType();
    IsMember = true;
  } else {
    return std::nullopt;
  }
  auto Params = functionTypeParameters(TPI, IPI, FunctionType, false);
  if (!Params)
    return std::nullopt;
  Params->second = IsMember || Params->second;
  return Params;
}

static std::map<uint32_t, unsigned>
pdbParameterCounts(NativeSession &Session) {
  std::map<uint32_t, unsigned> Counts;
  auto DbiOrErr = Session.getPDBFile().getPDBDbiStream();
  if (!DbiOrErr) {
    consumeError(DbiOrErr.takeError());
    return Counts;
  }
  auto TPIOrErr = Session.getPDBFile().getPDBTpiStream();
  if (!TPIOrErr) {
    consumeError(TPIOrErr.takeError());
    return Counts;
  }
  auto IPIOrErr = Session.getPDBFile().getPDBIpiStream();
  if (!IPIOrErr) {
    consumeError(IPIOrErr.takeError());
    return Counts;
  }
  TpiStream &TPI = *TPIOrErr;
  TpiStream &IPI = *IPIOrErr;
  const DbiModuleList &Modules = DbiOrErr->modules();
  for (uint32_t M = 0; M < Modules.getModuleCount(); ++M) {
    auto StreamOrErr = Session.getModuleDebugStream(M);
    if (!StreamOrErr) {
      consumeError(StreamOrErr.takeError());
      continue;
    }
    bool HadError = false;
    for (const codeview::CVSymbol &Symbol : StreamOrErr->symbols(&HadError)) {
      using codeview::SymbolKind;
      bool IsProcId = Symbol.kind() == SymbolKind::S_GPROC32_ID ||
                      Symbol.kind() == SymbolKind::S_LPROC32_ID ||
                      Symbol.kind() == SymbolKind::S_LPROC32_DPC_ID;
      bool IsProc = IsProcId || Symbol.kind() == SymbolKind::S_GPROC32 ||
                    Symbol.kind() == SymbolKind::S_LPROC32 ||
                    Symbol.kind() == SymbolKind::S_LPROC32_DPC;
      if (!IsProc)
        continue;
      auto ProcOrErr = codeview::SymbolDeserializer::deserializeAs<
          codeview::ProcSym>(Symbol);
      if (!ProcOrErr) {
        consumeError(ProcOrErr.takeError());
        continue;
      }
      const codeview::ProcSym &Proc = *ProcOrErr;
      uint32_t Rva = Session.getRVAFromSectOffset(Proc.Segment, Proc.CodeOffset);
      auto Params =
          functionTypeParameters(TPI, IPI, Proc.FunctionType, IsProcId);
      if (Params)
        Counts[Rva] = Params->first + Params->second;
    }
    (void)HadError;
  }
  return Counts;
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
  auto Hooks = createX86TargetHooks();
  if (!Hooks) {
    errs() << "target hooks setup failed\n";
    return 1;
  }
  Program P = revenant::analyze(PE, *Hooks);
  auto DisOrErr = Disasm::create(*PE.Img);
  if (!DisOrErr) {
    logAllUnhandledErrors(DisOrErr.takeError(), errs(), "decoder setup failed: ");
    return 1;
  }
  Disasm Dis = std::move(*DisOrErr);
  Signatures Sigs = analyzeSignatures(P, Dis);

  std::unique_ptr<IPDBSession> Session;
  if (Error E = NativeSession::createFromPdbPath(PdbPath, Session)) {
    logAllUnhandledErrors(std::move(E), errs(), "PDB load failed: ");
    return 1;
  }
  Session->setLoadAddress(PE.Img->Base.Value);
  auto PdbCounts =
      pdbParameterCounts(static_cast<NativeSession &>(*Session));

  std::error_code EC;
  raw_fd_ostream CSV(CsvPath, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "cannot open " << CsvPath << ": " << EC.message() << '\n';
    return 1;
  }
  CSV << "function_va,param_count,return_kind,pdb_param_count\n";
  uint64_t Compared = 0, Matched = 0, MissingPdb = 0;
  for (const auto &[VA, Sig] : Sigs.Funcs) {
    auto RVA = VA.offsetFrom(PE.Img->Base);
    if (!RVA || *RVA > UINT32_MAX)
      continue;
    auto PdbIt = PdbCounts.find(static_cast<uint32_t>(*RVA));
    unsigned PdbCount =
        PdbIt == PdbCounts.end() ? UINT32_MAX : PdbIt->second;
    const char *ReturnKind = !Sig.HasRet ? "none"
                             : Sig.Ret.Float ? "float"
                                             : "integer";
    CSV << format_hex_no_prefix(VA.Value, 0, true) << ',' << Sig.Params.size()
        << ',' << ReturnKind << ',';
    if (PdbCount == UINT32_MAX)
      CSV << '\n';
    else
      CSV << PdbCount << '\n';
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
