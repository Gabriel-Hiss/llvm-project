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
  if (auto Ec = sys::fs::create_directories(OutDir)) {
    errs() << "revenant: " << OutDir << ": " << Ec.message() << "\n";
    return 1;
  }

  Program Prog;
  Prog.Img = Pe->Img;

  // Every listed instruction as (u32 rva, u32 len), sorted unique. No
  // functions yet: the list is empty.
  SmallVector<std::pair<uint32_t, uint32_t>, 0> Items;
  for (auto &[Start, Rec] : Prog.Functions)
    for (Rva R : Rec.InstrRvas) {
      // Instr lens live in decode order; nothing to emit until analysis
      // fills records.
      (void)R;
    }
  llvm::sort(Items);
  Items.erase(std::unique(Items.begin(), Items.end()), Items.end());

  SmallString<256> ItemsPath(OutDir);
  sys::path::append(ItemsPath, "revenant-items.bin");
  std::error_code Ec;
  raw_fd_ostream ItemsOs(ItemsPath, Ec, sys::fs::OF_None);
  if (Ec) {
    errs() << "revenant: " << ItemsPath << ": " << Ec.message() << "\n";
    return 1;
  }
  for (auto [R, Len] : Items) {
    char B[4];
    support::endian::write32le(B, R);
    ItemsOs.write(B, 4);
    support::endian::write32le(B, Len);
    ItemsOs.write(B, 4);
  }
  ItemsOs.close();

  FileMap Map = FileMap::build(*Pe, Prog);
  SmallString<256> SpansPath(OutDir);
  sys::path::append(SpansPath, "file-spans.tsv");
  raw_fd_ostream SpansOs(SpansPath, Ec, sys::fs::OF_Text);
  if (Ec) {
    errs() << "revenant: " << SpansPath << ": " << Ec.message() << "\n";
    return 1;
  }
  SpansOs << "offset\tend\tva\tkind\n";
  for (const FileSpan &S : Map.Spans) {
    SpansOs << S.Offset << "\t" << S.End << "\t";
    if (S.SpanVa)
      SpansOs << format_hex(S.SpanVa->Value, 1);
    else
      SpansOs << "-";
    SpansOs << "\t" << fileByteKindLabel(S.Kind) << "\n";
  }
  SpansOs.close();

  // The partition must cover the whole file.
  uint64_t Covered = 0;
  for (const FileSpan &S : Map.Spans) {
    if (S.Offset != Covered || S.End <= S.Offset) {
      errs() << "revenant: file map does not partition the file\n";
      return 1;
    }
    Covered = S.End;
  }
  if (Covered != Pe->FileSize) {
    errs() << "revenant: file map covers " << Covered << " of "
           << Pe->FileSize << " bytes\n";
    return 1;
  }

  std::string ByKind;
  auto Counts = Map.bytesByKind();
  for (size_t I = 0; I < FILE_BYTE_KIND_COUNT; ++I) {
    if (!Counts[I])
      continue;
    if (!ByKind.empty())
      ByKind += ',';
    ByKind += "\"";
    ByKind += fileByteKindLabel((FileByteKind)I);
    ByKind += "\":";
    ByKind += std::to_string(Counts[I]);
  }
  SmallString<256> MetaPath(OutDir);
  sys::path::append(MetaPath, "meta.json");
  raw_fd_ostream MetaOs(MetaPath, Ec, sys::fs::OF_Text);
  if (Ec) {
    errs() << "revenant: " << MetaPath << ": " << Ec.message() << "\n";
    return 1;
  }
  MetaOs << "{\"pdb_loaded\":false,\"functions\":"
         << Prog.Functions.size() << ",\"instructions\":" << Items.size()
         << ",\"file_bytes\":" << Pe->FileSize
         << ",\"represented_bytes\":" << Covered << ",\"bytes_by_kind\":{"
         << ByKind << "}}\n";
  MetaOs.close();

  outs() << "file_bytes=" << Pe->FileSize << " represented_bytes=" << Covered
         << "\n";
  outs() << "functions=" << Prog.Functions.size()
         << " unique_instructions=" << Items.size() << " padding_runs="
         << Prog.Padding.size() << "\n";
  return 0;
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
