//==- revenant/lib/Object/FileMap.cpp - File byte map ---------------------==//
//
// Port of crates/engine/src/file_map.rs: partition [0, file_size) into
// classified spans from the parsed PE layout plus the program's accepted
// code chunks and padding runs.
//
//===----------------------------------------------------------------------===//

#include "revenant/Object/FileMap.h"

#include "revenant/Core/Program.h"

#include <algorithm>

using namespace llvm;

namespace revenant {

// A claim on a file range: (start, end, va_of_first, executable).
using Claim = std::tuple<uint64_t, uint64_t, std::optional<Va>, bool>;

/// File bytes the loader maps for a section: `min(VirtualSize, RawSize)`,
/// with `VirtualSize == 0` falling back to `RawSize` as the PE loader
/// does.
static uint64_t mappedDataLen(const RawSection &S) {
  if (S.VirtualSize == 0)
    return S.RawSize;
  return std::min(S.VirtualSize, S.RawSize);
}

static FileByteKind claimKind(const Claim &C, const PeFile &Pe) {
  if (std::get<0>(C) == 0 && std::get<2>(C) &&
      *std::get<2>(C) == Pe.Img->Base && !std::get<3>(C))
    return FileByteKind::Header;
  if (!std::get<2>(C))
    return FileByteKind::Certificate;
  return FileByteKind::Data;
}

/// Check if a byte range looks like a jump table (array of RVAs).
/// Returns true if the range is a 4-byte aligned array of 4-byte values
/// that mostly look like RVAs pointing into the image.
static bool looksLikeJumpTable(const uint8_t *Data, uint64_t Len,
                                uint64_t ImageBase) {
  if (Len < 8 || Len % 4 != 0)
    return false;
  uint64_t Count = Len / 4;
  const uint32_t *Vals = reinterpret_cast<const uint32_t *>(Data);
  // Check if values look like RVAs (pointing into the image)
  uint64_t MinRva = 0x200;
  uint64_t MaxRva = 0x200000;
  uint64_t ValidCount = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    uint64_t V = Vals[I];
    if (V >= MinRva && V <= MaxRva)
      ++ValidCount;
  }
  // At least 25% of values should be valid RVAs
  if (ValidCount * 4 < Count)
    return false;
  // Values are valid RVAs; no monotonicity requirement
  // (jump tables may not be monotonic if sorted by case value)
  return true;
}

/// Executable claim `[foff, fend)`: Code where accepted instruction
/// chunks cover it, Padding for the program's accepted padding runs,
/// Data for known jump tables, Unknown otherwise. `DataLen` bounds what
/// actually maps to image VAs.
static void emitExecRegion(const Program &Prog, uint64_t FOff,
                           uint64_t FEnd, uint64_t DataLen, Va Va0,
                           std::vector<FileSpan> &Spans) {
  // Collect all known jump table ranges from all functions
  std::vector<std::pair<Va, Va>> DataRanges;
  for (auto &[Start, Rec] : Prog.Functions)
    for (const auto &Range : Rec.DataRanges)
      DataRanges.push_back(Range);
  // Gather code chunks intersecting this section's mapped extent.
  uint64_t VEnd = Va0.Value + DataLen; // saturating
  if (VEnd < Va0.Value)
    VEnd = UINT64_MAX;
  std::vector<std::pair<uint64_t, uint64_t>> Chunks;
  for (auto &[Start, Rec] : Prog.Functions)
    for (auto [A, B] : Rec.Chunks)
      if (A.Value < VEnd && B.Value > Va0.Value)
        Chunks.push_back(
            {std::min(std::max(A.Value, Va0.Value), VEnd),
             std::min(std::max(B.Value, Va0.Value), VEnd)});
  llvm::sort(Chunks);
  Chunks.erase(std::unique(Chunks.begin(), Chunks.end()), Chunks.end());
  // Merge overlapping/adjacent chunk extents.
  std::vector<std::pair<uint64_t, uint64_t>> Merged;
  for (auto [A, B] : Chunks) {
    if (!Merged.empty() && A <= Merged.back().second) {
      Merged.back().second = std::max(Merged.back().second, B);
      continue;
    }
    Merged.push_back({A, B});
  }

  uint64_t Base = Prog.Img->Base.Value;
  uint64_t Pos = FOff;
  // Emit a gap [pos, gend): padding runs inside it are Padding; known
  // jump tables are Data; the rest stays Unknown. A run only counts when
  // its decoded extent covers the whole sub-span it claims.
  auto emitGap = [&](uint64_t GEnd) {
    uint64_t GVa = Va0.Value + (Pos - FOff);
    uint64_t GVEnd = Va0.Value + (GEnd - FOff);
    uint64_t Sub = Pos;
    for (const PaddingRun &Run : Prog.Padding) {
      if (Base + Run.End <= GVa)
        continue;
      if (Base + Run.Start >= GVEnd)
        break;
      uint64_t Rs = std::max(FOff + (Base + Run.Start - Va0.Value), Pos);
      uint64_t Re = std::min(FOff + (Base + Run.End - Va0.Value), GEnd);
      if (Rs > Sub)
        Spans.push_back(
            {Sub, Rs, Va(Va0.Value + (Sub - FOff)), FileByteKind::Unknown});
      if (Rs < Re) {
        Spans.push_back(
            {Rs, Re, Va(Va0.Value + (Rs - FOff)), FileByteKind::Padding});
        Sub = Re;
      }
    }
    if (Sub < GEnd) {
      uint64_t GapLen = GEnd - Sub;
      uint64_t GVa2 = Va0.Value + (Sub - FOff);
      uint64_t GVe2 = Va0.Value + (GEnd - FOff);
      // Check if this gap is a known jump table from DataRanges
      bool isKnownJumpTable = false;
      for (const auto &Range : DataRanges) {
        if (Range.first.Value <= GVa2 && Range.second.Value >= GVe2) {
          isKnownJumpTable = true;
          break;
        }
      }
      if (isKnownJumpTable) {
        Spans.push_back(
            {Sub, GEnd, Va(GVa2), FileByteKind::Data});
      } else {
        auto Bytes = Prog.Img->dataAt(Va(GVa2), GapLen);
        bool allZeros = (Bytes.size() == GapLen) &&
                        std::all_of(Bytes.begin(), Bytes.end(),
                                    [](uint8_t b) { return b == 0; });
        if (allZeros) {
          Spans.push_back(
              {Sub, GEnd, Va(GVa2), FileByteKind::Padding});
        } else if (GapLen >= 8 && GapLen % 4 == 0 &&
                   Bytes.size() == GapLen &&
                   looksLikeJumpTable(Bytes.data(), GapLen, Base)) {
          Spans.push_back(
              {Sub, GEnd, Va(GVa2), FileByteKind::Data});
        } else {
          Spans.push_back(
              {Sub, GEnd, Va(GVa2), FileByteKind::Unknown});
        }
      }
    }
  };

  for (auto [A, B] : Merged) {
    if (Pos < FOff + (A - Va0.Value))
      emitGap(FOff + (A - Va0.Value));
    Spans.push_back(
        {FOff + (A - Va0.Value), FOff + (B - Va0.Value), Va(A),
         FileByteKind::Code});
    Pos = FOff + (B - Va0.Value);
  }
  // Tail of the mapped extent.
  uint64_t MappedEnd = FOff + DataLen;
  if (Pos < MappedEnd)
    emitGap(MappedEnd);
  if (MappedEnd < FEnd) {
    uint64_t TailLen = FEnd - MappedEnd;
    auto TailBytes = Prog.Img->dataAt(Va(Va0.Value + (MappedEnd - FOff)), TailLen);
    bool allZeros = (TailBytes.size() == TailLen) &&
                    std::all_of(TailBytes.begin(), TailBytes.end(),
                                [](uint8_t b) { return b == 0; });
    if (allZeros) {
      Spans.push_back({MappedEnd, FEnd, std::nullopt, FileByteKind::Overlay});
    } else {
      Spans.push_back({MappedEnd, FEnd, std::nullopt, FileByteKind::Unknown});
    }
  }
}

/// In a conflicting group an executable claim can't safely claim Code —
/// overlapping layout means the bytes' ownership is unresolved.
static FileByteKind groupClaimKind(const Claim &C, const PeFile &Pe) {
  if (std::get<3>(C))
    return FileByteKind::Unknown;
  return claimKind(C, Pe);
}

/// A run of overlapping claims. Sub-ranges under exactly one claim keep
/// that claim's kind/VA; bytes under 2+ claims are Unknown with no VA.
static void emitGroup(ArrayRef<Claim> Claims, const PeFile &Pe,
                      std::vector<FileSpan> &Spans) {
  uint64_t Start = UINT64_MAX, End = 0;
  for (const Claim &C : Claims) {
    Start = std::min(Start, std::get<0>(C));
    End = std::max(End, std::get<1>(C));
  }
  // Boundary points where claim coverage changes.
  std::vector<uint64_t> Bounds = {Start, End};
  for (const Claim &C : Claims) {
    Bounds.push_back(std::get<0>(C));
    Bounds.push_back(std::get<1>(C));
  }
  llvm::sort(Bounds);
  Bounds.erase(std::unique(Bounds.begin(), Bounds.end()), Bounds.end());
  for (size_t W = 0; W + 1 < Bounds.size(); ++W) {
    uint64_t S = Bounds[W], E = Bounds[W + 1];
    if (S >= E)
      continue;
    const Claim *Covering = nullptr;
    size_t CoveringCount = 0;
    for (const Claim &C : Claims)
      if (std::get<0>(C) <= S && std::get<1>(C) >= E) {
        Covering = &C;
        ++CoveringCount;
      }
    if (CoveringCount != 1) {
      Spans.push_back({S, E, std::nullopt, FileByteKind::Unknown});
      continue;
    }
    const Claim &C = *Covering;
    // A sole-covered sub-range still ends at the claim's mapped extent:
    // raw slack past VirtualSize has no VA.
    uint64_t VaEnd = E;
    if (std::get<2>(C)) {
      for (const RawSection &Rs : Pe.RawSections)
        if (Rs.FileOffset == std::get<0>(C) && Rs.SecVa == *std::get<2>(C)) {
          VaEnd = std::get<0>(C) + mappedDataLen(Rs); // saturating below
          if (VaEnd < std::get<0>(C))
            VaEnd = UINT64_MAX;
          break;
        }
    }
    uint64_t Split = std::clamp(VaEnd, S, E);
    FileByteKind K = groupClaimKind(C, Pe);
    if (Split > S) {
      std::optional<Va> V;
      if (std::get<2>(C))
        V = Va(std::get<2>(C)->Value + (S - std::get<0>(C)));
      Spans.push_back({S, Split, V, K});
    }
    if (Split < E)
      Spans.push_back({Split, E, std::nullopt, K});
  }
}

FileMap FileMap::build(const PeFile &Pe, const Program &Prog) {
  FileMap Map;
  Map.FileSize = Pe.FileSize;
  if (Pe.FileSize == 0)
    return Map;
  uint64_t FileSize = Pe.FileSize;

  // Claims: (start, end, va_of_first, executable). Headers and the
  // certificate are neither.
  std::vector<Claim> Claims;
  if (Pe.HeadersSize > 0)
    Claims.emplace_back(0, std::min(Pe.HeadersSize, FileSize),
                        std::optional<Va>(Pe.Img->Base), false);
  for (const RawSection &S : Pe.RawSections) {
    uint64_t End = S.FileOffset + S.RawSize; // saturating
    if (End < S.FileOffset)
      End = UINT64_MAX;
    End = std::min(End, FileSize);
    if (S.FileOffset < End)
      Claims.emplace_back(S.FileOffset, End, std::optional<Va>(S.SecVa),
                          S.Executable);
  }
  if (auto Cert = Pe.Certificate)
    Claims.emplace_back(Cert->first, std::min(Cert->second, FileSize),
                        std::optional<Va>(), false);
  llvm::sort(Claims,
             [](const Claim &A, const Claim &B) {
               return std::get<0>(A) < std::get<0>(B);
             });

  // Trailing unclaimed bytes past the last header/section extent are
  // Overlay; the certificate table is not part of that boundary (it sits
  // in overlay territory itself).
  uint64_t OverlayFrom = std::min(Pe.HeadersSize, FileSize);
  for (const RawSection &S : Pe.RawSections) {
    uint64_t End = S.FileOffset + S.RawSize;
    if (End < S.FileOffset)
      End = UINT64_MAX;
    OverlayFrom = std::max(OverlayFrom, std::min(End, FileSize));
  }

  std::vector<FileSpan> Spans;
  uint64_t Pos = 0;
  // Pending unclaimed interval flushed as one span.
  auto pushGap = [&](uint64_t S, uint64_t E) {
    if (S >= E)
      return;
    // Bytes past the last claimed region are Overlay
    if (S >= OverlayFrom && OverlayFrom > 0) {
      Spans.push_back({S, E, std::nullopt, FileByteKind::Overlay});
      return;
    }
    Spans.push_back({S, E, std::nullopt, FileByteKind::Padding});
  };

  size_t I = 0;
  while (I < Claims.size()) {
    // Group claims overlapping each other transitively.
    size_t J = I + 1;
    uint64_t GEnd = std::get<1>(Claims[I]);
    while (J < Claims.size() && std::get<0>(Claims[J]) < GEnd) {
      GEnd = std::max(GEnd, std::get<1>(Claims[J]));
      ++J;
    }
    uint64_t GStart = std::get<0>(Claims[I]);
    if (GStart > Pos)
      pushGap(Pos, GStart);

    const Claim &C = Claims[I];
    if (J - I == 1 && std::get<3>(C) && std::get<2>(C)) {
      // Sole claim, executable: split into Code / Padding / Unknown via
      // the accepted code chunks.
      uint64_t FOff = std::get<0>(C), FEnd = std::get<1>(C);
      Va Va0 = *std::get<2>(C);
      uint64_t DataLen = 0;
      for (const RawSection &S : Pe.RawSections)
        if (S.FileOffset == FOff && S.SecVa == Va0) {
          DataLen = mappedDataLen(S);
          break;
        }
      DataLen = std::min(DataLen, FEnd - FOff);
      emitExecRegion(Prog, FOff, FEnd, DataLen, Va0, Spans);
    } else if (J - I == 1) {
      // Sole non-executable claim. Mapped extent: min(vsize, raw_size)
      // for sections; the header claim maps fully.
      uint64_t FOff = std::get<0>(C), FEnd = std::get<1>(C);
      auto Va0 = std::get<2>(C);
      uint64_t Vsize = FEnd - FOff;
      for (const RawSection &S : Pe.RawSections)
        if (Va0 && S.SecVa == *Va0 && S.FileOffset == FOff) {
          Vsize = mappedDataLen(S);
          break;
        }
      if (Va0) {
        uint64_t Mapped = FOff + std::min(Vsize, FEnd - FOff);
        if (Mapped > FOff)
          Spans.push_back({FOff, Mapped, Va0, claimKind(C, Pe)});
        if (FEnd > Mapped)
          Spans.push_back({Mapped, FEnd, std::nullopt, claimKind(C, Pe)});
      } else {
        Spans.push_back({FOff, FEnd, std::nullopt, claimKind(C, Pe)});
      }
    } else {
      emitGroup(ArrayRef<Claim>(Claims).slice(I, J - I), Pe, Spans);
    }
    Pos = std::max(Pos, std::min(GEnd, FileSize));
    I = J;
  }
  if (Pos < FileSize)
    pushGap(Pos, FileSize);

  // Merge adjacent same-kind spans with continuous VA.
  std::vector<FileSpan> Merged;
  Merged.reserve(Spans.size());
  for (const FileSpan &S : Spans) {
    if (!Merged.empty()) {
      FileSpan &Last = Merged.back();
      bool VaContinuous;
      if (Last.SpanVa && S.SpanVa)
        VaContinuous =
            Last.SpanVa->Value + (Last.End - Last.Offset) == S.SpanVa->Value;
      else
        VaContinuous = !Last.SpanVa && !S.SpanVa;
      if (Last.Kind == S.Kind && Last.End == S.Offset && VaContinuous) {
        Last.End = S.End;
        continue;
      }
    }
    Merged.push_back(S);
  }
  Map.Spans = std::move(Merged);
  return Map;
}

const FileSpan *FileMap::spanAt(uint64_t Offset) const {
  auto It = std::partition_point(
      Spans.begin(), Spans.end(),
      [Offset](const FileSpan &S) { return S.End <= Offset; });
  if (It == Spans.end() || Offset >= It->End)
    return nullptr;
  return &*It;
}

std::optional<uint64_t> FileMap::offsetOfVa(Va Addr) const {
  std::optional<uint64_t> Found;
  for (const FileSpan &S : Spans) {
    if (!S.SpanVa)
      continue;
    uint64_t Len = S.End - S.Offset;
    if (Addr >= *S.SpanVa && Addr.Value < S.SpanVa->Value + Len) {
      uint64_t Off = S.Offset + (Addr - *S.SpanVa); // saturating
      if (Found && *Found != Off)
        return std::nullopt;
      Found = Off;
    }
  }
  return Found;
}

std::optional<Va> FileMap::vaOfOffset(uint64_t Offset) const {
  const FileSpan *S = spanAt(Offset);
  if (!S || !S->SpanVa)
    return std::nullopt;
  return Va(S->SpanVa->Value + (Offset - S->Offset));
}

uint64_t FileMap::coveredBytes() const {
  uint64_t Sum = 0;
  for (const FileSpan &S : Spans)
    Sum += S.End - S.Offset;
  return Sum;
}

std::array<uint64_t, FILE_BYTE_KIND_COUNT> FileMap::bytesByKind() const {
  std::array<uint64_t, FILE_BYTE_KIND_COUNT> Out = {};
  for (const FileSpan &S : Spans)
    Out[(size_t)S.Kind] += S.End - S.Offset;
  return Out;
}

} // end namespace revenant
