#include "revenant/Analysis/Analysis.h"
#include "revenant/Analysis/LibMatch.h"
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
static cl::opt<std::string> LibraryDBPath(
    "libdb", cl::desc("optional static library match database"));


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
      if (Params) {
        unsigned N = Params->first + Params->second;
        auto Prev = Counts.find(Rva);
        if (Prev != Counts.end() && Prev->second != N)
          errs() << "folded twins at rva " << format_hex_no_prefix(Rva, 8)
                 << ": pdb counts " << Prev->second << " vs " << N << '\n';
        Counts[Rva] = N;
      }
    }
    (void)HadError;
  }
  return Counts;
}

/// Shape tag for mismatch triage: how the function body looks.
static const char *shapeTag(const Function &F) {
  if (F.Instrs.empty())
    return "empty";
  if (F.Instrs.size() == 1) {
    const Instr &I = F.Instrs[0];
    if (I.Flow == FlowKind::Branch)
      return "jmp1";
    if (I.Flow == FlowKind::IndirectBranch)
      return "ijmp1";
    if (I.Flow == FlowKind::Return)
      return "ret1";
    return "other1";
  }
  if (F.Instrs.size() == 2 && F.Instrs[1].Flow == FlowKind::Return &&
      F.Instrs[0].Flow == FlowKind::None)
    return "movret";
  const Instr &Last = F.Instrs.back();
  if ((Last.Flow == FlowKind::Branch ||
       Last.Flow == FlowKind::IndirectBranch) &&
      !(Last.HasTarget && F.contains(Last.Target)))
    return "tailjmp";
  return "body";
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
  if (!LibraryDBPath.empty()) {
    auto DB = LibraryDatabase::load(LibraryDBPath);
    if (!DB) {
      logAllUnhandledErrors(DB.takeError(), errs(),
                           "library database load failed: ");
      return 1;
    }
    std::vector<LibraryMatch> Matches = matchLibraryFunctions(P, *DB);
    applyLibraryMatches(P, Sigs, Matches, *DB);
  }

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
  CSV << "function_va,param_count,return_kind,pdb_param_count,callers,min_prefix,max_prefix,shape,ninstrs\n";
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
    unsigned NCallers = 0, MinPrefix = UINT32_MAX, MaxPrefix = 0;
    for (const auto &[Site, N] : Sigs.CallsiteArgs) {
      auto CallIns = Dis.decodeOne(Site);
      if (!CallIns || !CallIns->HasTarget || CallIns->Target != VA)
        continue;
      ++NCallers;
      MinPrefix = std::min(MinPrefix, N);
      MaxPrefix = std::max(MaxPrefix, N);
    }
    std::string Shape = "?";
    size_t NInstrs = 0;
    if (auto F = revenant::function(P, VA)) {
      Shape = shapeTag(*F);
      NInstrs = F->Instrs.size();
    }
    CSV << format_hex_no_prefix(VA.Value, 0, true) << ',' << Sig.Params.size()
        << ',' << ReturnKind << ',';
    if (PdbCount == UINT32_MAX)
      CSV << ',';
    else
      CSV << PdbCount << ',';
    CSV << NCallers << ',';
    if (NCallers == 0)
      CSV << ',';
    else
      CSV << MinPrefix << ',';
    if (NCallers == 0)
      CSV << ',';
    else
      CSV << MaxPrefix << ',';
    CSV << Shape << ',' << NInstrs << '\n';
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
