//===-- llvm-revenant-opcount.cpp - Static opcode census for PE binaries --===//
//
// Usage: llvm-revenant-opcount <out-dir> <pe>...
//
// For each PE32+ input, enumerates functions via the exception directory
// (.pdata), linear-disassembles every function range with the MCDisassembler
// for the triple routed through TargetRegistry, and writes
// <out-dir>/<file-name>.opcount.csv (opcode,count rows) plus one row per input
// in <out-dir>/summary.csv.
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Win64EH.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::object;

namespace {

struct SectionData {
  uint64_t RVA;
  uint64_t EndRVA;
  ArrayRef<uint8_t> Contents;
};

struct Range {
  uint32_t Begin;
  uint32_t End;
};

} // end anonymous namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    errs() << "usage: llvm-revenant-opcount <out-dir> <pe>...\n";
    return 1;
  }

  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllDisassemblers();

  StringRef OutDir(argv[1]);
  if (std::error_code EC = sys::fs::create_directories(OutDir)) {
    errs() << "cannot create output directory: " << EC.message() << "\n";
    return 1;
  }

  SmallString<256> SummaryPath(OutDir);
  sys::path::append(SummaryPath, "summary.csv");
  std::error_code EC;
  raw_fd_ostream Summary(SummaryPath, EC, sys::fs::OF_Append);
  if (EC) {
    errs() << "cannot open " << SummaryPath << ": " << EC.message() << "\n";
    return 1;
  }
  uint64_t SummarySize = 0;
  if (!sys::fs::file_size(SummaryPath, SummarySize) && SummarySize == 0)
    Summary << "binary,path,functions,overlapping_dropped,bytes,instructions,"
               "decode_failures\n";

  StringMap<bool> HostFeatures = sys::getHostCPUFeatures();
  std::string HostFeatureStr;
  {
    raw_string_ostream FS(HostFeatureStr);
    StringRef Sep;
    for (const auto &KV : HostFeatures)
      FS << Sep << (KV.second ? "+" : "-") << KV.first(), Sep = ",";
  }
  std::string HostCPU = sys::getHostCPUName().str();
  Triple HostTriple(sys::getProcessTriple());

  for (int I = 2; I < argc; ++I) {
    StringRef Path(argv[I]);
    StringRef Name = sys::path::filename(Path);

    auto ObjOrErr = ObjectFile::createObjectFile(Path);
    if (!ObjOrErr) {
      errs() << Name << ": " << toString(ObjOrErr.takeError()) << "\n";
      continue;
    }
    ObjectFile &Obj = *ObjOrErr->getBinary();

    auto *COFFObj = dyn_cast<COFFObjectFile>(&Obj);
    if (!COFFObj) {
      errs() << Name << ": not a COFF PE file, skipping\n";
      continue;
    }
    if (!COFFObj->is64()) {
      errs() << Name << ": not PE32+, skipping\n";
      continue;
    }

    const data_directory *ExceptDir =
        COFFObj->getDataDirectory(COFF::EXCEPTION_TABLE);
    if (!ExceptDir || !ExceptDir->RelativeVirtualAddress) {
      errs() << Name << ": no exception directory (.pdata), skipping\n";
      continue;
    }

    Triple TT = Obj.makeTriple();
    std::string LookupErr;
    const Target *TheTarget = TargetRegistry::lookupTarget(TT, LookupErr);
    if (!TheTarget) {
      errs() << Name << ": " << LookupErr << " (" << TT.str() << "), skipping\n";
      continue;
    }

    MCTargetOptions MCOptions;
    std::unique_ptr<const MCRegisterInfo> MRI(TheTarget->createMCRegInfo(TT));
    std::unique_ptr<const MCAsmInfo> MAI(
        TheTarget->createMCAsmInfo(*MRI, TT, MCOptions));
    // Decode with the host's full feature set when the target arch matches
    // the host; otherwise use the generic baseline.
    std::string CPU = "generic", Features;
    if (TT.getArch() == HostTriple.getArch()) {
      CPU = HostCPU;
      Features = HostFeatureStr;
    }
    std::unique_ptr<const MCSubtargetInfo> STI(
        TheTarget->createMCSubtargetInfo(TT, CPU, Features));
    std::unique_ptr<const MCInstrInfo> MCII(TheTarget->createMCInstrInfo());
    MCContext Ctx(TT, MAI.get(), MRI.get(), STI.get());
    std::unique_ptr<const MCDisassembler> Dis(
        TheTarget->createMCDisassembler(*STI, Ctx));
    if (!MRI || !MAI || !STI || !MCII || !Dis) {
      errs() << Name << ": could not create MC pieces for " << TT.str()
             << ", skipping\n";
      continue;
    }

    uint64_t ImageBase = COFFObj->getImageBase();
    std::vector<SectionData> Sections;
    for (const SectionRef &S : Obj.sections()) {
      uint64_t RVA = S.getAddress() - ImageBase;
      Expected<StringRef> C = S.getContents();
      if (!C) {
        consumeError(C.takeError());
        continue;
      }
      Sections.push_back({RVA, RVA + S.getSize(),
                          ArrayRef<uint8_t>(
                              reinterpret_cast<const uint8_t *>(C->data()),
                              C->size())});
    }
    llvm::sort(Sections,
               [](const SectionData &A, const SectionData &B) {
                 return A.RVA < B.RVA;
               });
    auto findSection = [&](uint32_t RVA) -> const SectionData * {
      for (const SectionData &S : Sections)
        if (RVA >= S.RVA && RVA < S.EndRVA)
          return &S;
      return nullptr;
    };

    // Exception directory: array of RUNTIME_FUNCTION.
    const SectionData *PDataSec = findSection(ExceptDir->RelativeVirtualAddress);
    if (!PDataSec || ExceptDir->RelativeVirtualAddress + ExceptDir->Size >
                        PDataSec->EndRVA) {
      errs() << Name << ": exception directory not mapped, skipping\n";
      continue;
    }
    ArrayRef<uint8_t> PData = PDataSec->Contents.drop_front(
        ExceptDir->RelativeVirtualAddress - PDataSec->RVA);
    size_t NumEntries =
        std::min<size_t>(ExceptDir->Size, PData.size()) /
        sizeof(Win64EH::RuntimeFunction);
    const Win64EH::RuntimeFunction *RT =
        reinterpret_cast<const Win64EH::RuntimeFunction *>(PData.data());

    std::vector<Range> Ranges;
    for (size_t E = 0; E < NumEntries; ++E) {
      if (RT[E].EndAddress <= RT[E].StartAddress)
        continue;
      Ranges.push_back({RT[E].StartAddress, RT[E].EndAddress});
    }
    llvm::sort(Ranges, [](const Range &A, const Range &B) {
      return A.Begin != B.Begin ? A.Begin < B.Begin : A.End < B.End;
    });
    std::vector<Range> Kept;
    uint64_t Overlapping = 0;
    for (const Range &R : Ranges) {
      if (!Kept.empty() && Kept.back().Begin == R.Begin &&
          Kept.back().End == R.End)
        continue; // identical range: dedupe
      if (!Kept.empty() && R.Begin < Kept.back().End) {
        ++Overlapping;
        continue;
      }
      Kept.push_back(R);
    }

    std::map<std::string, uint64_t> Counts;
    uint64_t Bytes = 0, Instructions = 0, Failures = 0;
    for (const Range &R : Kept) {
      const SectionData *Sec = findSection(R.Begin);
      if (!Sec)
        continue;
      uint64_t End = std::min<uint64_t>(R.End, Sec->EndRVA);
      Bytes += End - R.Begin;
      uint64_t Off = R.Begin;
      while (Off < End) {
        ArrayRef<uint8_t> Slice = Sec->Contents.slice(
            Off - Sec->RVA, std::min<uint64_t>(End - Off, 16));
        MCInst Inst;
        uint64_t Size = 0;
        MCDisassembler::DecodeStatus Status = Dis->getInstruction(
            Inst, Size, Slice, Off + ImageBase, nulls());
        if (Status == MCDisassembler::Fail || Size == 0) {
          ++Failures;
          ++Off;
          continue;
        }
        ++Counts[MCII->getName(Inst.getOpcode()).str()];
        ++Instructions;
        Off += Size;
      }
    }

    SmallString<256> CSVPath(OutDir);
    sys::path::append(CSVPath, Name + ".opcount.csv");
    raw_fd_ostream CSV(CSVPath, EC);
    if (EC) {
      errs() << Name << ": cannot open " << CSVPath << ": " << EC.message()
             << "\n";
      continue;
    }
    CSV << "opcode,count\n";
    std::vector<std::pair<std::string, uint64_t>> Sorted(Counts.begin(),
                                                       Counts.end());
    llvm::sort(Sorted, [](const auto &A, const auto &B) {
      return A.second != B.second ? A.second > B.second : A.first < B.first;
    });
    for (const auto &[Op, N] : Sorted)
      CSV << Op << "," << N << "\n";

    Summary << Name << "," << Path << "," << Kept.size() << "," << Overlapping
            << "," << Bytes << "," << Instructions << "," << Failures << "\n";
    outs() << Name << ": " << Kept.size() << " functions, " << Instructions
           << " instructions, " << Failures << " decode failures\n";
  }
  return 0;
}
