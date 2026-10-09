//==- revenant/lib/Object/PeFile.cpp - PE32+ loader -----------------------==//
//
// Port of crates/pe/src/lib.rs onto llvm::object::COFFObjectFile.
// Directory tables are read through the mapped Image (VA -> section
// bytes) so bound/truncated data fails soft the same way.
//
//===----------------------------------------------------------------------===//

#include "revenant/Object/PeFile.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::object;
using llvm::support::endian::read16le;
using llvm::support::endian::read32le;
using llvm::support::endian::read64le;

namespace revenant {

static Error peError(const Twine &Msg) {
  return createStringError(std::make_error_code(std::errc::invalid_argument),
                           Msg);
}

// Reads `N` raw bytes at an RVA through the mapped image.
static std::optional<std::vector<uint8_t>> readRva(const Image &Img, Rva R,
                                                 size_t N) {
  return Img.read(Img.vaOfRva(R), N);
}

//==--------------------------------------------------------------------==//
// Sections -> Image, raw section extents
//==--------------------------------------------------------------------==//

static Error buildImage(const COFFObjectFile &Obj, Va Base, Image &Img,
                        std::vector<RawSection> &RawSections,
                        uint64_t FileSize) {
  Img.Base = Base;
  for (SectionRef S : Obj.sections()) {
    const coff_section *Sh = Obj.getCOFFSection(S);
    Expected<StringRef> Name = S.getName();
    if (!Name)
      return peError("section name");
    StringRef Nm = *Name;
    while (!Nm.empty() && Nm.back() == '\0')
      Nm = Nm.drop_back();

    Expected<StringRef> Contents = S.getContents();
    if (!Contents)
      return peError("section data");

    uint32_t Ch = Sh->Characteristics;
    Section Sec;
    Sec.Name = Nm.str();
    // SectionRef::getAddress() is already absolute for COFF objects.
    Sec.SecVa = Va(S.getAddress());
    // Rust: `s.size().max(data.len())` — `size()` is the mapped extent.
    Sec.Vsize = std::max<uint64_t>(S.getSize(), Contents->size());
    Sec.Data.assign(Contents->bytes_begin(), Contents->bytes_end());
    Sec.Flags = {(Ch & COFF::IMAGE_SCN_MEM_READ) != 0,
                 (Ch & COFF::IMAGE_SCN_MEM_WRITE) != 0,
                 (Ch & COFF::IMAGE_SCN_MEM_EXECUTE) != 0};
    Img.Sections.push_back(std::move(Sec));

    RawSections.push_back(RawSection{Nm.str(), (uint64_t)Sh->PointerToRawData,
                                     (uint64_t)Sh->SizeOfRawData, Sec.SecVa,
                                     (uint64_t)Sh->VirtualSize,
                                     Sec.Flags.Execute});
  }
  if (Img.Sections.empty())
    return peError("no sections");
  return Error::success();
}

//==--------------------------------------------------------------------==//
// Imports: IAT slot VAs mirror crates/pe read_imports
//==--------------------------------------------------------------------==//

static Error readImports(const COFFObjectFile &Obj, const Image &Img,
                         Va Base, std::vector<Import> &Out) {
  for (const ImportDirectoryEntryRef &Dir : Obj.import_directories()) {
    const coff_import_directory_table_entry *Desc;
    if (Error E = Dir.getImportTableEntry(Desc))
      return std::move(E);
    StringRef Dll;
    if (Dir.getName(Dll))
      Dll = "";
    uint32_t FirstThunk = Desc->ImportAddressTableRVA;
    // ILT may be absent (bound imports); walk the IAT then.
    uint32_t Lookup = Desc->ImportLookupTableRVA;
    uint32_t ThunkRva = Lookup != 0 ? Lookup : FirstThunk;

    for (uint32_t I = 0;; ++I) {
      auto ThunkBytes = readRva(Img, ThunkRva + I * 8, 8);
      if (!ThunkBytes)
        break;
      uint64_t Data = read64le(ThunkBytes->data());
      if (Data == 0)
        break;
      Va IatVa = Va(Base.Value + FirstThunk + (uint64_t)I * 8);
      Import Imp;
      Imp.IatVa = IatVa;
      Imp.Dll = Dll.str();
      if (Data & (1ull << 63)) {
        Imp.Ordinal = (uint16_t)(Data & 0xFFFF);
      } else {
        // IMAGE_IMPORT_BY_NAME at the hint/name RVA: u16 hint, then a
        // NUL-terminated name.
        auto NameBytes =
            readRva(Img, (Rva)(Data & 0x7FFFFFFF), 2);
        if (NameBytes) {
          if (auto Name = Img.readCstr(
                  Img.vaOfRva((Rva)(Data & 0x7FFFFFFF) + 2), 1 << 16))
            Imp.Name = std::move(*Name);
          else
            Imp.Name = std::string();
        } else {
          Imp.Name = std::string();
        }
      }
      Out.push_back(std::move(Imp));
    }
  }
  return Error::success();
}

//==--------------------------------------------------------------------==//
// Exports: named + ordinal-only + forwarders, mirroring object::pe's
// ExportTable walk in read_exports.
//==--------------------------------------------------------------------==//

// A forwarder string "dll.sym" or "dll.#ord" — `object` splits on the
// last '.' into (dll, sym); sym of "#ord" is an ordinal forward.
static std::optional<std::string> forwarderString(const Image &Img, Rva Rva) {
  return Img.readCstr(Img.vaOfRva(Rva), 1 << 12);
}

static Error readExports(const COFFObjectFile &Obj, const Image &Img,
                         Va Base, std::vector<Export> &Out) {
  const data_directory *DD =
      Obj.getDataDirectory(COFF::EXPORT_TABLE);
  if (!DD || DD->RelativeVirtualAddress == 0)
    return Error::success();

  auto DirBytes =
      readRva(Img, DD->RelativeVirtualAddress, sizeof(export_directory_table_entry));
  if (!DirBytes)
    return peError("truncated export directory");
  const export_directory_table_entry &Dir =
      *reinterpret_cast<const export_directory_table_entry *>(DirBytes->data());

  Rva AddrTableRva = Dir.ExportAddressTableRVA;
  Rva NamePtrRva = Dir.NamePointerRVA;
  Rva OrdinalRva = Dir.OrdinalTableRVA;
  uint32_t NumAddrs = Dir.AddressTableEntries;
  uint32_t NumNames = Dir.NumberOfNamePointers;
  uint32_t OrdBase = Dir.OrdinalBase;
  Rva DirRva = DD->RelativeVirtualAddress;
  Rva DirEnd = DirRva + DD->Size;

  auto addrOf = [&](uint32_t I) -> std::optional<Rva> {
    auto B = readRva(Img, AddrTableRva + I * 4, 4);
    if (!B)
      return std::nullopt;
    return read32le(B->data());
  };

  std::set<Rva> NamedRvas;
  for (uint32_t I = 0; I < NumNames; ++I) {
    auto Nb = readRva(Img, NamePtrRva + I * 4, 4);
    auto Ob = readRva(Img, OrdinalRva + I * 2, 2);
    if (!Nb || !Ob)
      continue;
    Rva NameRva = read32le(Nb->data());
    uint16_t OrdIndex = read16le(Ob->data());
    auto Name = Img.readCstr(Img.vaOfRva(NameRva), 1 << 12);
    if (!Name)
      continue;
    auto RvaOpt = addrOf(OrdIndex);
    if (!RvaOpt)
      continue;
    Rva R = *RvaOpt;
    Export Ex;
    Ex.Name = std::move(*Name);
    if (R >= DirRva && R < DirEnd) {
      // RVA inside the export directory: forwarder string.
      Ex.ExportVa = Img.vaOfRva(R);
      Ex.Forwarder = forwarderString(Img, R);
    } else {
      Ex.ExportVa = Img.vaOfRva(R);
    }
    NamedRvas.insert(R);
    Out.push_back(std::move(Ex));
  }

  // Ordinal-only exports (no name): include them with a synthetic name.
  for (uint32_t I = 0; I < NumAddrs; ++I) {
    auto RvaOpt = addrOf(I);
    if (!RvaOpt || *RvaOpt == 0 || NamedRvas.count(*RvaOpt))
      continue;
    Rva R = *RvaOpt;
    uint32_t Ordinal = OrdBase + I;
    Export Ex;
    Ex.Name = formatv("#{0}", Ordinal).str();
    if (R >= DirRva && R < DirEnd) {
      Ex.ExportVa = Img.vaOfRva(R);
      Ex.Forwarder = forwarderString(Img, R);
    } else {
      Ex.ExportVa = Img.vaOfRva(R);
    }
    Out.push_back(std::move(Ex));
  }
  return Error::success();
}

//==--------------------------------------------------------------------==//
// .pdata + chained unwind
//==--------------------------------------------------------------------==//

// UNWIND_INFO::UnwindInfo flags field (top 5 bits of byte 0) may hold
// UNW_FLAG_CHAININFO; the chained runtime-function entry follows the
// (even-padded) unwind codes. Resolve to the primary unwind info and
// return the primary function's begin VA. Chained entries may point at
// another chained entry, so recurse to the ultimate primary (depth-capped
// against corrupt unwind data).
static std::pair<Va, std::optional<Va>>
resolveChainedUnwind(const Image &Img, Rva UnwindRva, unsigned Depth) {
  constexpr uint8_t UNW_FLAG_CHAININFO = 4;
  Va UnwindVa = Img.vaOfRva(UnwindRva);
  auto First = Img.read(UnwindVa, 4);
  if (!First)
    return {UnwindVa, std::nullopt};
  uint8_t Flags = (*First)[0] >> 3;
  if (!(Flags & UNW_FLAG_CHAININFO) || Depth >= 32)
    return {UnwindVa, std::nullopt};
  uint64_t Count = (*First)[2];
  // UNWIND_CODE is 2 bytes; the code array is padded to a multiple of two
  // codes (4 bytes) before the chained RUNTIME_FUNCTION.
  uint64_t CodesBytes = ((Count + 1) & ~1ull) * 2;
  Va ChainedVa = UnwindVa.wrappingAdd(4 + CodesBytes);
  auto Entry = Img.read(ChainedVa, 12);
  if (!Entry)
    return {UnwindVa, std::nullopt};
  Rva ParentBegin = read32le(Entry->data());
  Rva PrimUnwind = read32le(Entry->data() + 8);
  if (PrimUnwind == 0 || PrimUnwind == UnwindRva)
    return {UnwindVa, Img.vaOfRva(ParentBegin)};
  auto [Resolved, Deeper] = resolveChainedUnwind(Img, PrimUnwind, Depth + 1);
  return {Resolved, Deeper ? Deeper : std::optional<Va>(Img.vaOfRva(ParentBegin))};
}

static void readPdata(const Image &Img, std::vector<RuntimeFunction> &Out) {
  const Section *Sec = Img.sectionNamed(".pdata");
  if (!Sec)
    return;
  for (size_t Off = 0; Off + 12 <= Sec->Data.size(); Off += 12) {
    Rva Begin = read32le(Sec->Data.data() + Off);
    Rva End = read32le(Sec->Data.data() + Off + 4);
    Rva Unwind = read32le(Sec->Data.data() + Off + 8);
    auto [UnwindVa, ChainedTo] = resolveChainedUnwind(Img, Unwind, 0);
    Out.push_back(
        RuntimeFunction{Img.vaOfRva(Begin), Img.vaOfRva(End), UnwindVa,
                        ChainedTo});
  }
}

//==--------------------------------------------------------------------==//
// .reloc IMAGE_REL_BASED_DIR64
//==--------------------------------------------------------------------==//

// `.reloc` IMAGE_REL_BASED_DIR64 entries: the u64 stored at each
// relocation site is the pointer the binary actually uses post-relocation
// — values landing in executable sections are function-pointer
// candidates. The slot addresses themselves hold data, not code, so they
// are returned separately for `Image::InlineData`.
static void readRelocs(const COFFObjectFile &Obj, const Image &Img,
                       std::vector<Va> &Values, std::vector<VaRange> &Slots) {
  for (const BaseRelocRef &R : Obj.base_relocs()) {
    uint8_t Type = 0;
    if (R.getType(Type) || Type != COFF::IMAGE_REL_BASED_DIR64)
      continue;
    uint32_t Rva = 0;
    if (R.getRVA(Rva))
      continue;
    Va Addr = Img.vaOfRva(Rva);
    if (auto V = Img.readU64(Addr))
      Values.push_back(Va(*V));
    if (Img.isExecutable(Addr))
      Slots.push_back({Addr, Addr.wrappingAdd(8)});
  }
}

//==--------------------------------------------------------------------==//
// Go build-id blob
//==--------------------------------------------------------------------==//

// The Go linker emits a build-id record inside the first executable
// prologue for cgo ones: `\xff Go build ID: "<id>"\n \xff` followed by
// `0xcc` filler up to a 128-byte total. It sits inside `.text` but is
// pure data. Detect it by content and return the blob's VA range.
static std::optional<VaRange> goBuildId(const Image &Img) {
  static const uint8_t Sig[] = "\xff Go build ID: \"";
  const Section *Sec = nullptr;
  for (const Section &S : Img.Sections)
    if (S.Flags.Execute && !S.Data.empty()) {
      Sec = &S;
      break;
    }
  if (!Sec)
    return std::nullopt;
  size_t Off = 0;
  for (;;) {
    auto It = std::search(Sec->Data.begin() + Off, Sec->Data.end(),
                          Sig, Sig + sizeof(Sig) - 1);
    if (It == Sec->Data.end())
      return std::nullopt;
    size_t Pos = It - Sec->Data.begin();
    size_t End = std::min(Pos + 0x80, Sec->Data.size());
    Off = Pos + 1;
    if (End - Pos != 0x80)
      return std::nullopt;
    ArrayRef<uint8_t> D(Sec->Data.data() + Pos, 0x80);
    // The record ends at `\n \xff`; the rest of the blob is 0xcc.
    auto CcIt = std::find(D.begin(), D.end(), (uint8_t)0xcc);
    if (CcIt == D.end())
      continue;
    size_t TailAt = CcIt - D.begin();
    // The record ends at `\n \xff`; the rest of the blob is 0xcc.
    // Rust: `data[..tail_at].windows(3).rposition(w == b"\n \xff") + 3`.
    size_t Last = std::string::npos;
    for (size_t I = 0; I + 3 <= TailAt; ++I)
      if (D[I] == '\n' && D[I + 1] == ' ' && D[I + 2] == '\xff')
        Last = I;
    std::optional<size_t> TextEnd;
    if (Last != std::string::npos)
      TextEnd = Last + 3;
    if (TextEnd && *TextEnd == TailAt &&
        std::all_of(D.begin() + TailAt, D.end(),
                    [](uint8_t B) { return B == 0xcc; }))
      return VaRange{Sec->SecVa.wrappingAdd(Pos), Sec->SecVa.wrappingAdd(Pos + 0x80)};
  }
}

//==--------------------------------------------------------------------==//
// TLS callbacks
//==--------------------------------------------------------------------==//

// IMAGE_TLS_DIRECTORY64: AddressOfCallBacks is the sixth qword at offset 24.
// It is a VA and points to a null-terminated array of callback VAs.
// The Rust reference currently reads offset 40, outside this structure.
static std::vector<Va> readTls(const COFFObjectFile &Obj, const Image &Img,
                               Va Base) {
  const data_directory *DD = Obj.getDataDirectory(COFF::TLS_TABLE);
  if (!DD || DD->RelativeVirtualAddress == 0)
    return {};
  auto Dir = Img.read(Va(Base.Value + DD->RelativeVirtualAddress), 40);
  if (!Dir)
    return {};
  uint64_t CbArrayVa = read64le(Dir->data() + 24);
  std::vector<Va> Out;
  Va Addr(CbArrayVa);
  for (;;) {
    auto Cb = Img.readU64(Addr);
    if (!Cb || *Cb == 0)
      break;
    Out.push_back(Va(*Cb));
    Addr = Addr.wrappingAdd(8);
    if (Out.size() > 4096)
      break;
  }
  return Out;
}

//==--------------------------------------------------------------------==//
// CodeView (RSDS) and Rich header
//==--------------------------------------------------------------------==//

static std::optional<CodeView> readCodeView(const COFFObjectFile &Obj,
                                            ArrayRef<uint8_t> Bytes) {
  for (const debug_directory &Dbg : Obj.debug_directories()) {
    if (Dbg.Type != COFF::IMAGE_DEBUG_TYPE_CODEVIEW)
      continue;
    uint32_t Off = Dbg.PointerToRawData;
    uint32_t Sz = Dbg.SizeOfData;
    if (Off + Sz > Bytes.size() || Sz < 24)
      continue;
    const uint8_t *D = Bytes.data() + Off;
    if (memcmp(D, "RSDS", 4) != 0)
      continue;
    CodeView Cv;
    memcpy(Cv.Guid, D + 4, 16);
    Cv.Age = read32le(D + 20);
    Cv.PdbPath.assign((const char *)D + 24,
                      strnlen((const char *)D + 24, Sz - 24));
    return Cv;
  }
  return std::nullopt;
}

static std::vector<RichEntry> readRichHeader(ArrayRef<uint8_t> Bytes) {
  std::vector<RichEntry> Out;
  // "Rich" then a u32 xor key; entries walk back in 8-byte xored
  // (comp_id, count) pairs to the "DanS" marker (its own key-masked id).
  ArrayRef<uint8_t> B = Bytes;
  auto Rich = std::search(B.begin(), B.end(), "Rich", "Rich" + 4);
  if (Rich == B.end())
    return Out;
  size_t Pos = Rich - B.begin();
  if (Pos + 8 > B.size())
    return Out;
  uint32_t Key = read32le(B.data() + Pos + 4);
  if (Pos < 8 + 4)
    return Out;
  // Entries start 8 bytes before "Rich" (mask + entry). Walk back.
  std::vector<std::pair<uint32_t, uint32_t>> Reversed;
  size_t Cur = Pos;
  for (;;) {
    if (Cur < 8)
      return Out;
    Cur -= 8;
    uint32_t A = read32le(B.data() + Cur) ^ Key;
    uint32_t C = read32le(B.data() + Cur + 4) ^ Key;
    if (A == 0x536E6144 /* "DanS" */) {
      break;
    }
    Reversed.push_back({A, C});
  }
  // object::rich_header_info().unmasked_entries() yields entries in
  // DanS-first file order (reversed from the backwards walk).
  for (auto It = Reversed.rbegin(); It != Reversed.rend(); ++It)
    Out.push_back(RichEntry{(uint16_t)(It->first >> 16),
                            (uint16_t)(It->first & 0xFFFF), It->second});
  return Out;
}

//==--------------------------------------------------------------------==//
// PeFile::parse
//==--------------------------------------------------------------------==//

Expected<PeFile> PeFile::parse(ArrayRef<uint8_t> Bytes) {
  StringRef Data((const char *)Bytes.data(), Bytes.size());
  MemoryBufferRef Ref(Data, "pe");
  auto ObjOrErr = ObjectFile::createCOFFObjectFile(Ref);
  if (!ObjOrErr) {
    consumeError(ObjOrErr.takeError());
    return peError("not a PE file or unsupported format");
  }
  std::unique_ptr<COFFObjectFile> Obj = std::move(*ObjOrErr);

  uint16_t Machine = Obj->getMachine();
  if (Machine != COFF::IMAGE_FILE_MACHINE_AMD64)
    return peError(
        formatv("unsupported machine {0:x} (only x86-64 AMD64)", Machine).str());
  const pe32plus_header *Hdr = Obj->getPE32PlusHeader();
  if (!Hdr)
    return peError("not a PE32+ image");

  Va Base(Obj->getImageBase());
  PeFile Pe;
  auto Img = std::make_shared<Image>();
  Pe.FileSize = Bytes.size();

  if (Error E = buildImage(*Obj, Base, *Img, Pe.RawSections, Pe.FileSize))
    return std::move(E);
  Pe.Img = Img;

  Pe.EntryPoint = Va(Base.Value + Hdr->AddressOfEntryPoint);
  Pe.HeadersSize =
      std::min<uint64_t>(Hdr->SizeOfHeaders, Pe.FileSize);

  if (Error E = readImports(*Obj, *Img, Base, Pe.Imports))
    return std::move(E);
  if (Error E = readExports(*Obj, *Img, Base, Pe.Exports))
    return std::move(E);
  readPdata(*Img, Pe.Pdata);

  std::vector<VaRange> RelocSlots;
  readRelocs(*Obj, *Img, Pe.RelocTargets, RelocSlots);
  Img->InlineData = std::move(RelocSlots);
  Pe.GoBuildId = goBuildId(*Img);
  if (Pe.GoBuildId)
    Img->InlineData.push_back(*Pe.GoBuildId);
  llvm::sort(Img->InlineData,
             [](const VaRange &A, const VaRange &B) { return A.Start < B.Start; });
  // Merge overlapping/adjacent ranges: consumers rely on disjoint,
  // sorted intervals.
  {
    std::vector<VaRange> Merged;
    Merged.reserve(Img->InlineData.size());
    for (VaRange R : Img->InlineData) {
      if (!Merged.empty() && R.Start <= Merged.back().End) {
        if (R.End > Merged.back().End)
          Merged.back().End = R.End;
        continue;
      }
      Merged.push_back(R);
    }
    Img->InlineData = std::move(Merged);
  }

  Pe.TlsCallbacks = readTls(*Obj, *Img, Base);
  Pe.Cv = readCodeView(*Obj, Bytes);
  Pe.RichHeader = readRichHeader(Bytes);

  // Certificate table: SECURITY directory's "VA" is a file offset.
  if (const data_directory *Sec = Obj->getDataDirectory(COFF::CERTIFICATE_TABLE)) {
    uint64_t Off = Sec->RelativeVirtualAddress;
    uint64_t Size = Sec->Size;
    if (Off < Pe.FileSize && Off + Size > Off) {
      uint64_t End = std::min(Off + Size, Pe.FileSize);
      if (Off < End)
        Pe.Certificate = {Off, End};
    }
  }

  return Expected<PeFile>(std::move(Pe));
}

Expected<PeFile> PeFile::load(StringRef Path) {
  auto BufOrErr = MemoryBuffer::getFile(Path);
  if (!BufOrErr)
    return createFileError(Path, BufOrErr.getError());
  StringRef Data = (*BufOrErr)->getBuffer();
  return parse(ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Data.data()),
                                 Data.size()));
}

} // end namespace revenant
