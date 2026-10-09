#include "revenant/Analysis/Signatures.h"
#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
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

static void buildPdataProgram(const PeFile &PE, const Disasm &Dis,
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
    std::vector<Instr> Instructions;
    std::map<uint64_t, size_t> Indices;
    for (Va IP = RF.Begin; IP < RF.End;) {
      auto Ins = Dis.decodeOne(IP);
      if (!Ins || Ins->Size == 0)
        break;
      Indices.emplace(IP.Value, Instructions.size());
      Instructions.push_back(*Ins);
      auto Rva = IP.offsetFrom(PE.Img->Base);
      if (!Rva || *Rva > UINT32_MAX)
        break;
      F.InstrRvas.push_back(static_cast<uint32_t>(*Rva));
      IP = Ins->nextVa();
    }
    if (Instructions.empty())
      continue;

    std::set<size_t> Boundaries{0};
    for (size_t I = 0; I < Instructions.size(); ++I) {
      const Instr &Ins = Instructions[I];
      if (Ins.Flow != FlowKind::Branch && Ins.Flow != FlowKind::CondBranch &&
          Ins.Flow != FlowKind::IndirectBranch && Ins.Flow != FlowKind::Return &&
          Ins.Flow != FlowKind::Trap)
        continue;
      if (I + 1 < Instructions.size())
        Boundaries.insert(I + 1);
      if (Ins.HasTarget && Ins.Target >= RF.Begin && Ins.Target < RF.End) {
        auto Target = Indices.find(Ins.Target.Value);
        if (Target != Indices.end())
          Boundaries.insert(Target->second);
      }
    }
    SmallVector<size_t, 16> Starts(Boundaries.begin(), Boundaries.end());
    for (size_t BI = 0; BI < Starts.size(); ++BI) {
      size_t Begin = Starts[BI];
      size_t End = BI + 1 < Starts.size() ? Starts[BI + 1]
                                           : Instructions.size();
      if (Begin >= End)
        continue;
      const Instr &Last = Instructions[End - 1];
      BlockRec B;
      B.Start = static_cast<uint32_t>(
          Instructions[Begin].Addr - PE.Img->Base);
      Va BlockEndVa = Last.nextVa();
      if (BlockEndVa > RF.End)
        BlockEndVa = RF.End;
      B.End = static_cast<uint32_t>(BlockEndVa - PE.Img->Base);
      B.InstrsBegin = Begin;
      B.InstrsEnd = End;
      B.EdgesBegin = F.Edges.size();

      auto AddEdge = [&](Va Target, EdgeTag Kind) {
        auto TargetRva = Target.offsetFrom(PE.Img->Base);
        if (!TargetRva || *TargetRva > UINT32_MAX)
          return;
        F.Edges.push_back({static_cast<uint32_t>(*TargetRva), Kind});
      };
      auto AddFallthrough = [&]() {
        if (End < Instructions.size())
          AddEdge(Instructions[End].Addr, EdgeTag::Fallthrough);
      };
      if (Last.Flow == FlowKind::Return) {
        B.EndKind = BlockEnd::Return;
      } else if (Last.Flow == FlowKind::Trap) {
        B.EndKind = BlockEnd::Noreturn;
      } else if (Last.Flow == FlowKind::IndirectBranch) {
        B.EndKind = BlockEnd::UnresolvedIndirect;
        F.UnresolvedIndirects.push_back(Last.Addr);
      } else if (Last.Flow == FlowKind::CondBranch) {
        B.EndKind = BlockEnd::Branch;
        if (Last.HasTarget && Last.Target >= RF.Begin && Last.Target < RF.End)
          AddEdge(Last.Target, EdgeTag::CondTaken);
        else if (Last.HasTarget)
          AddEdge(Last.Target, EdgeTag::TailCall);
        AddFallthrough();
      } else if (Last.Flow == FlowKind::Branch) {
        if (Last.HasTarget && Last.Target >= RF.Begin && Last.Target < RF.End) {
          B.EndKind = BlockEnd::Branch;
          AddEdge(Last.Target, EdgeTag::Jump);
        } else {
          B.EndKind = BlockEnd::TailCall;
          if (Last.HasTarget)
            AddEdge(Last.Target, EdgeTag::TailCall);
        }
      } else if (End < Instructions.size()) {
        B.EndKind = BlockEnd::Fallthrough;
        AddFallthrough();
      } else {
        B.EndKind = BlockEnd::EndOfFunction;
      }
      B.EdgesEnd = F.Edges.size();
      F.Blocks.push_back(B);
    }
    P.Functions.emplace(F.Start, std::move(F));
  }
}

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
