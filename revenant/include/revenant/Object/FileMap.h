//==- revenant/Object/FileMap.h - File byte map --------------------*- C++ -*-==//
//
// Every byte of the on-disk file, partitioned into classified spans.
// Mirrors crates/engine/src/file_map.rs: derived state built from the
// parsed PE layout plus the accepted instruction chunks of the analyzed
// program. The partition invariant holds — spans are sorted, disjoint,
// nonempty, and cover [0, file_size) exactly.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_OBJECT_FILEMAP_H
#define REVENANT_OBJECT_FILEMAP_H

#include "revenant/Core/Va.h"
#include "revenant/Object/PeFile.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace revenant {

struct Program;

/// Classification of a file byte range.
enum class FileByteKind : uint8_t {
  Header,
  Code,
  Data,
  /// Whole gap between code chunks accepted as a `Program::Padding` run.
  Padding,
  /// Inter-section bytes, exec bytes that never decoded, ambiguous
  /// overlaps — anything not positively classified.
  Unknown,
  Certificate,
  /// File content past the last claimed region.
  Overlay,
};

inline const char *fileByteKindLabel(FileByteKind K) {
  switch (K) {
  case FileByteKind::Header:
    return "Header";
  case FileByteKind::Code:
    return "Code";
  case FileByteKind::Data:
    return "Data";
  case FileByteKind::Padding:
    return "Padding";
  case FileByteKind::Unknown:
    return "Unknown";
  case FileByteKind::Certificate:
    return "Certificate";
  case FileByteKind::Overlay:
    return "Overlay";
  }
  return "?";
}

constexpr size_t FILE_BYTE_KIND_COUNT = 7;

/// A maximal run of same-kind file bytes. `Va` is the image VA of the
/// span's first byte, when that byte maps into the image.
struct FileSpan {
  uint64_t Offset = 0;
  /// Exclusive end offset.
  uint64_t End = 0;
  std::optional<revenant::Va> SpanVa;
  FileByteKind Kind = FileByteKind::Unknown;
};

/// Sorted, disjoint, gapless classification of `[0, file_size)`.
struct FileMap {
  uint64_t FileSize = 0;
  std::vector<FileSpan> Spans;

  /// Build the partition from the parsed PE and the analyzed program.
  static FileMap build(const PeFile &Pe, const Program &Prog);

  /// The span covering `offset`, or nullptr when `offset >= file_size`.
  const FileSpan *spanAt(uint64_t Offset) const;

  /// File offset of `va`: the headers, or the mapped extent of its
  /// section. Zero-fill tails, image holes and unmapped bytes have no
  /// offset — and neither does a VA that multiple spans could answer for
  /// (ambiguous overlap).
  std::optional<uint64_t> offsetOfVa(Va Addr) const;

  /// VA of a mapped file byte.
  std::optional<Va> vaOfOffset(uint64_t Offset) const;

  /// Bytes covered by the span partition — must equal `FileSize`.
  uint64_t coveredBytes() const;

  /// Byte totals per kind, indexed by `(size_t)FileByteKind`.
  std::array<uint64_t, FILE_BYTE_KIND_COUNT> bytesByKind() const;
};

} // end namespace revenant

#endif // REVENANT_OBJECT_FILEMAP_H
