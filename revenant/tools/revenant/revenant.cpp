//==- revenant/tools/revenant - driver -----------------------------------==//
//
//   revenant info <pe>                     binary summary
//   revenant asm <pe> <va> [n]             linear decode, Intel syntax
//   revenant --export-parity <pe> <outdir> contract export files
//
//===----------------------------------------------------------------------===//

#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/FileMap.h"
#include "revenant/Object/PeFile.h"

#include <cstdint>
#include <string>

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace revenant;

// Implemented in ExportParity.cpp (parity contract export).
namespace revenant {
int exportParity(PeFile &Pe, StringRef OutDir);
}
static int usage() {
  errs() << "usage: revenant info <pe>\n"
            "       revenant asm <pe> <va> [n]\n"
            "       revenant --export-parity <pe> <outdir>\n";
  return 2;
}


static std::optional<PeFile> openPe(StringRef Path) {
  auto BufOrErr = MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    errs() << "revenant: " << Path << ": " << BufOrErr.getError().message()
           << "\n";
    return std::nullopt;
  }
  StringRef Data = (*BufOrErr)->getBuffer();
  auto PeOrErr = PeFile::parse(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Data.data()), Data.size()));
  if (!PeOrErr) {
    errs() << "revenant: " << Path << ": "
           << toString(PeOrErr.takeError()) << "\n";
    return std::nullopt;
  }
  return std::move(*PeOrErr);
}

static int cmdInfo(StringRef Path) {
  auto Pe = openPe(Path);
  if (!Pe)
    return 1;
  outs() << "file:     " << Path << "\n";
  outs() << "format:   PE32+ x86-64, image base "
         << format_hex(Pe->Img->Base.Value, 0) << "\n";
  outs() << "entry:    " << format_hex(Pe->EntryPoint.Value, 0) << "\n";
  if (Pe->Cv)
    outs() << "codeview: " << Pe->Cv->PdbPath << " (age " << Pe->Cv->Age
           << ")\n";
  outs() << "counts:   " << Pe->Img->Sections.size() << " sections, "
         << Pe->Imports.size() << " imports, " << Pe->Exports.size()
         << " exports, " << Pe->Pdata.size() << " pdata entries, "
         << Pe->TlsCallbacks.size() << " tls callbacks, "
         << Pe->RelocTargets.size() << " dir64 relocs\n";
  if (Pe->GoBuildId)
    outs() << "go:       build-id blob at "
           << format_hex(Pe->GoBuildId->Start.Value, 0) << "\n";
  for (const Section &S : Pe->Img->Sections)
    outs() << "  section " << S.Name << " "
           << format_hex(S.SecVa.Value, 0) << " "
           << format_hex(S.Vsize, 0)
           << (S.Flags.Execute ? " x" : S.Flags.Write ? " w" : " r")
           << "\n";
  return 0;
}

static int cmdAsm(StringRef Path, uint64_t Addr, uint64_t Count) {
  auto Pe = openPe(Path);
  if (!Pe)
    return 1;
  auto DOrErr = Disasm::create(*Pe->Img);
  if (!DOrErr) {
    errs() << "revenant: " << toString(DOrErr.takeError()) << "\n";
    return 1;
  }
  Disasm &D = *DOrErr;
  Va Ip(Addr);
  for (uint64_t I = 0; I < Count; ++I) {
    auto Instr = D.decodeOne(Ip);
    if (!Instr)
      break;
    ArrayRef<uint8_t> Bytes = Pe->Img->dataAt(Ip, Instr->Size);
    std::string Hex;
    for (uint8_t B : Bytes)
      Hex += formatv("{0:x-2}", B).str() + " ";
    outs() << format_hex(Ip.Value, 0) << ": "
           << left_justify(Hex, 32) << D.format(*Instr) << "\n";
    Ip = Ip.wrappingAdd(Instr->Size);
  }
  return 0;
}
static int cmdExportParity(StringRef Path, StringRef OutDir) {
  auto Pe = openPe(Path);
  if (!Pe)
    return 1;
  return exportParity(*Pe, OutDir);
}



static std::optional<uint64_t> parseVa(StringRef S) {
  StringRef T = S;
  uint64_t V = 0;
  if (T.consume_front_insensitive("0x")) {
    if (!T.getAsInteger(16, V))
      return V;
    return std::nullopt;
  }
  // Unprefixed: decimal digits only parse as decimal; anything else is
  // hex (listings print hex without a prefix).
  if (S.find_first_not_of("0123456789") == StringRef::npos &&
      !S.getAsInteger(10, V))
    return V;
  if (!S.getAsInteger(16, V))
    return V;
  return std::nullopt;
}

int main(int Argc, char **Argv) {
  InitLLVM X(Argc, Argv);
  if (Argc < 3)
    return usage();
  StringRef Cmd = Argv[1];
  if (Cmd == "info")
    return cmdInfo(Argv[2]);
  if (Cmd == "asm") {
    if (Argc < 4)
      return usage();
    auto VaOpt = parseVa(Argv[3]);
    if (!VaOpt) {
      errs() << "revenant: bad address " << Argv[3] << "\n";
      return 2;
    }
    uint64_t Count = 16;
    if (Argc > 4) {
      auto COpt = parseVa(Argv[4]);
      if (!COpt) {
        errs() << "revenant: bad count " << Argv[4] << "\n";
        return 2;
      }
      Count = *COpt;
    }
    return cmdAsm(Argv[2], *VaOpt, Count);
  }
  if (Cmd == "--export-parity") {
    if (Argc < 4)
      return usage();
    return cmdExportParity(Argv[2], Argv[3]);
  }
  return usage();
}
