//==- revenant/Object/PeFile.h - PE32+ loader ----------------------*- C++ -*-==//
//
// PE (x86-64 only) parsing on top of llvm::object::COFFObjectFile, plus
// the bits Object doesn't fully cover: .pdata chained unwind entries, TLS
// callbacks, import IAT slot addresses, the Rich header, and the Go
// build-id blob. Mirrors crates/pe/src/lib.rs.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_OBJECT_PEFILE_H
#define REVENANT_OBJECT_PEFILE_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Va.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace llvm::object {
class COFFObjectFile;
}

namespace revenant {

/// One imported symbol slot in the IAT.
struct Import {
  /// VA of the IAT slot (the address a `call [rip+X]` goes through).
  Va IatVa;
  std::string Dll;
  /// Symbol name, or empty for ordinal-only imports.
  std::optional<std::string> Name;
  std::optional<uint16_t> Ordinal;
};

/// One named export.
struct Export {
  /// VA of the exported symbol. For forwarders this is the address of the
  /// forwarder string inside the export directory.
  Va ExportVa;
  std::string Name;
  /// `"dll.sym"` / `dll.#ord` target for forwarded exports.
  std::optional<std::string> Forwarder;
};

/// One `.pdata` runtime function (begin, end, unwind info VA). Chained
/// unwind entries are resolved: `Unwind` points at the primary function's
/// unwind info and `ChainedTo` holds the primary function's begin VA.
struct RuntimeFunction {
  Va Begin;
  Va End;
  Va Unwind;
  std::optional<Va> ChainedTo;
};

/// CodeView RSDS record from the debug directory.
struct CodeView {
  uint8_t Guid[16];
  uint32_t Age = 0;
  std::string PdbPath;
};

/// One decoded Rich header entry.
struct RichEntry {
  /// Product/tool id (`comp_id >> 16`).
  uint16_t ProdId = 0;
  /// Build number (`comp_id & 0xffff`).
  uint16_t Build = 0;
  uint32_t Count = 0;
};
/// A name retained in the image's COFF symbol table.
struct CoffSymbol {
  Va Address;
  std::string Name;
  bool IsFunction = false;
};


/// A section's extent in the raw file (PointerToRawData/SizeOfRawData),
/// kept distinct from the mapped virtual extent: zero-filled virtual
/// tails add no file bytes and a section with no raw data owns none.
struct RawSection {
  std::string Name;
  /// File offset of the first raw byte.
  uint64_t FileOffset = 0;
  /// Bytes claimed in the file (SizeOfRawData).
  uint64_t RawSize = 0;
  Va SecVa;
  /// Virtual size (mapped extent, may exceed `RawSize`).
  uint64_t VirtualSize = 0;
  /// Raw bytes past the mapped extent (`min(VirtualSize, RawSize)` ..
  /// `RawSize`): linker fill the image never maps. Empty when there is
  /// no slack or the slack could not be read.
  std::vector<uint8_t> RawSlack;
  bool Executable = false;
};

/// A parsed PE32+ image with all the loader-side records analysis needs.
struct PeFile {
  /// One copy of the mapped image, shared with `Program::Img`.
  std::shared_ptr<Image> Img;
  Va EntryPoint;
  std::vector<Import> Imports;
  std::vector<Export> Exports;
  std::vector<RuntimeFunction> Pdata;
  /// u64 values found in `.reloc` IMAGE_REL_BASED_DIR64 slots — the
  /// relocated pointers the binary stores. Values landing in executable
  /// sections are function-pointer candidates.
  std::vector<Va> RelocTargets;
  std::vector<Va> TlsCallbacks;
  std::optional<CodeView> Cv;
  std::vector<RichEntry> RichHeader;
  std::vector<CoffSymbol> CoffSymbols;
  /// Total input file length in bytes.
  uint64_t FileSize = 0;
  /// SizeOfHeaders clamped to `FileSize` — file extent of the headers.
  uint64_t HeadersSize = 0;
  /// Per-section raw file extents, in section-table order.
  std::vector<RawSection> RawSections;
  /// Certificate table file range (SECURITY directory's address field is
  /// a FILE OFFSET, unlike every other directory) — `[offset, end)`.
  std::optional<std::pair<uint64_t, uint64_t>> Certificate;
  /// Control Flow Guard function-pointer slots from the load config
  /// directory (IMAGE_LOAD_CONFIG_DIRECTORY64): `call [slot]` invokes the
  /// stored handler. `_guard_check_icall` checks the call target carried in
  /// rcx; `_guard_dispatch_icall` tail-jumps to the target in rax. Non-CFG
  /// images leave both empty.
  std::optional<Va> GuardCFCheckPtr;
  std::optional<Va> GuardCFDispatchPtr;
  /// The Go linker's build-id blob inside `.text`, when present. Doubles
  /// as the Go-binary flag for analysis rules that only Go's codegen
  /// warrants.
  std::optional<VaRange> GoBuildId;

  /// Parse a PE32+ image from raw file bytes. Fails for non-PE,
  /// non-AMD64, or truncated files.
  static llvm::Expected<PeFile> parse(llvm::ArrayRef<uint8_t> Bytes);

  /// Read a file and parse it.
  static llvm::Expected<PeFile> load(llvm::StringRef Path);

  /// True when `va` sits in a section's virtual tail: beyond `raw_size`
  /// but under `virtual_size` it is mapped-but-unbacked, so the loader
  /// zero-fills it. Raw slack (file bytes past `virtual_size`) and
  /// ambiguous overlaps are NOT zero-fill.
  bool vaIsZeroFill(Va Addr) const {
    for (const RawSection &S : RawSections) {
      if (!(S.VirtualSize > S.RawSize && Addr >= S.SecVa))
        continue;
      uint64_t D = Addr - S.SecVa;
      if (D >= S.RawSize && D < S.VirtualSize)
        return true;
    }
    return false;
  }
};

} // end namespace revenant

#endif // REVENANT_OBJECT_PEFILE_H
