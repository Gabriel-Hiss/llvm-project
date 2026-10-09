//==- revenant/tools/revenant/ExportParity.cpp - parity contract export ---==//
//
// Port of crates/engine/examples/parity_export.rs:
//
//   revenant --export-parity <pe> <outdir>
//
// writes `revenant-items.bin` (sorted unique `u32 rva, u32 len` for every
// listed instruction: function code plus decoded padding runs),
// `file-spans.tsv` (the `FileMap` partition of the whole file) and
// `meta.json`. Every listed entry must re-decode at a real instruction
// boundary; a non-partitioning map exits non-zero, matching the Rust
// tool's asserts.
//
// The item list sits on top of the full `Program::analyze` port
// (`analysis::Program::analyze(pe, None)`). Until `revenant/Analysis`
// lands its entry point, `analyzeForExport` returns the empty program so
// the byte map and meta still verify.
//
//===--------------------------------------------------------------------===//

#include "revenant/Core/Program.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/FileMap.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"

// Analysis entry locked by Swe2Discovery (header lands with
// RevenantAnalysis): revenant::analyze(Pe, Hooks) and
// revenant::instructionItems(Program).
#if defined(REVENANT_HAVE_ANALYSIS) && defined(__has_include)
#if __has_include("revenant/Analysis/Analysis.h")
#include "revenant/Analysis/Analysis.h"
#define REVENANT_CALL_ANALYSIS 1
#endif
#endif

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
namespace revenant {



/// `revenant-asm.jsonl`, an opt-in sidecar for the asm-text parity
/// metric (the lead's ASM 1:1 requirement): per listed instruction, its
/// Intel text as the fork prints it and its resolved branch/RIP-relative
/// target RVAs, so text comparison keys on operand identity rather than
/// symbol spelling. Only written when `REVENANT_PARITY_ASM` is set; the
/// contract files are never affected.
static int writeAsmSidecar(
    const Program &Prog, Disasm &D,
    const std::vector<std::pair<uint32_t, uint32_t>> &Items,
    StringRef OutDir) {
  SmallString<256> Path(OutDir);
  sys::path::append(Path, "revenant-asm.jsonl");
  std::error_code Ec;
  raw_fd_ostream OS(Path, Ec, sys::fs::OF_Text);
  if (Ec) {
    errs() << "revenant: " << Path << ": " << Ec.message() << "\n";
    return 1;
  }
  auto Esc = [](StringRef S) -> std::string {
    std::string Out;
    for (char C : S) {
      if (C == '"' || C == '\\')
        Out += '\\', Out += C;
      else if ((unsigned char)C < 0x20)
        Out += ' ';
      else
        Out += C;
    }
    return Out;
  };
  for (auto [R, Len] : Items) {
    std::optional<Instr> I = D.decodeOne(Prog.Img->vaOfRva(R));
    OS << "{\"rva\":" << R << ",\"len\":" << Len << ",\"asm\":\""
       << Esc(I ? D.format(*I) : "?") << "\"";
    if (I && I->HasTarget)
      if (auto T = Prog.Img->rvaOf(I->Target))
        OS << ",\"target\":" << *T;
    if (I && I->HasMemTarget)
      if (auto T = Prog.Img->rvaOf(I->MemTarget))
        OS << ",\"mem\":" << *T;
    OS << "}\n";
  }
  OS.close();
  return 0;
}

/// The analyzed program the export lists items for. Mirrors
/// `Session::open`: analysis always runs fresh, never from a cache, and
/// never with a PDB (the fork has no PDB loading; the parity harness
/// guarantees none is reachable).
static Expected<Program> analyzeForExport(const PeFile &Pe) {
#if defined(REVENANT_CALL_ANALYSIS)
  auto Hooks = createX86TargetHooks();
  return revenant::analyze(Pe, *Hooks);
#else
  Program Prog;
  Prog.Img = Pe.Img;
  return Prog;
#endif
}

/// Every instruction the listing shows — function code plus padding
/// filler — as `(rva, len)`, sorted and unique. With RevenantAnalysis
/// this delegates to its `instructionItems`; the local fallback is the
/// 1:1 port of `analysis::Program::instruction_items` (re-decode for the
/// length, an undecodable entry is an error carrying the RVA).
static Expected<std::vector<std::pair<uint32_t, uint32_t>>>
instructionItems(const Program &Prog, const Disasm &D) {
  std::vector<std::pair<uint32_t, uint32_t>> Items;
  auto Push = [&](Rva R) -> Error {
    Va Addr = Prog.Img->vaOfRva(R);
    std::optional<Instr> I = D.decodeOne(Addr);
    if (!I)
      return createStringError(std::make_error_code(std::errc::bad_message),
                               "invalid instruction at " + Twine::utohexstr(R));
    Items.push_back({R, I->Size});
    return Error::success();
  };
  for (auto &[Start, Rec] : Prog.Functions)
    for (Rva R : Rec.InstrRvas)
      if (Error E = Push(R))
        return std::move(E);

  for (const PaddingRun &Run : Prog.Padding)
    for (Rva R : Run.Instrs)
      if (Error E = Push(R))
        return std::move(E);
  llvm::sort(Items);
  Items.erase(std::unique(Items.begin(), Items.end()), Items.end());
  return Items;
}

int exportParity(PeFile &Pe, StringRef OutDir) {
  if (auto Ec = sys::fs::create_directories(OutDir)) {
    errs() << "revenant: " << OutDir << ": " << Ec.message() << "\n";
    return 1;
  }

  auto ProgOrErr = analyzeForExport(Pe);
  if (!ProgOrErr) {
    errs() << "revenant: " << toString(ProgOrErr.takeError()) << "\n";
    return 1;
  }
  Program &Prog = *ProgOrErr;
#if !defined(REVENANT_CALL_ANALYSIS)
  // Recorded by compare.py (exporter_stdout tail): the item list is
  // empty because RevenantAnalysis is not merged yet — never silent.
  outs() << "analysis=pending: empty item list (no RevenantAnalysis)\n";
#endif

  auto DOrErr = Disasm::create(*Pe.Img);
  if (!DOrErr) {
    errs() << "revenant: " << toString(DOrErr.takeError()) << "\n";
    return 1;
  }
  Disasm &D = *DOrErr;

#if defined(REVENANT_CALL_ANALYSIS)
  auto LibItems = revenant::instructionItems(Prog);
  if (!LibItems) {
    errs() << "revenant: " << toString(LibItems.takeError()) << "\n";
    return 1;
  }
  const std::vector<std::pair<uint32_t, uint32_t>> &Items = *LibItems;
#else
  auto ItemsOrErr = instructionItems(Prog, D);
  if (!ItemsOrErr) {
    errs() << "revenant: " << toString(ItemsOrErr.takeError()) << "\n";
    return 1;
  }
  const std::vector<std::pair<uint32_t, uint32_t>> &Items = *ItemsOrErr;
#endif

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

  if (sys::Process::GetEnv("REVENANT_PARITY_ASM"))
    if (int Rc = writeAsmSidecar(Prog, D, Items, OutDir))
      return Rc;

  FileMap Map = FileMap::build(Pe, Prog);
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
      SpansOs << Twine::utohexstr(S.SpanVa->Value);
    else
      SpansOs << "-";
    SpansOs << "\t" << fileByteKindLabel(S.Kind) << "\n";
  }
  SpansOs.close();

  // The Rust exporter asserts the partition covers the whole file.
  uint64_t Covered = 0;
  for (const FileSpan &S : Map.Spans) {
    if (S.Offset != Covered || S.End <= S.Offset) {
      errs() << "revenant: file map does not partition the file\n";
      return 1;
    }
    Covered = S.End;
  }
  if (Covered != Pe.FileSize) {
    errs() << "revenant: file map covers " << Covered << " of "
           << Pe.FileSize << " bytes\n";
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
  MetaOs << "{\"pdb_loaded\":false,\"functions\":" << Prog.Functions.size()
         << ",\"instructions\":" << Items.size()
         << ",\"file_bytes\":" << Pe.FileSize
         << ",\"represented_bytes\":" << Covered << ",\"bytes_by_kind\":{"
         << ByKind << "}}\n";
  MetaOs.close();

  outs() << "file_bytes=" << Pe.FileSize << " represented_bytes=" << Covered
         << "\n";
  outs() << "functions=" << Prog.Functions.size()
         << " unique_instructions=" << Items.size() << " padding_runs="
         << Prog.Padding.size() << "\n";
  return 0;
}

} // end namespace revenant
