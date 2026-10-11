//==- revenant/Analysis/Padding.h - Alignment filler recovery --*- C++ -*-==//
//
// Port of the padding classifier in crates/analysis/src/lib.rs:
// `is_filler`, `pad_len`, `filler_head`, `gap_fillers`, `decode_padding`
// plus the `PaddingRun` they produce (Core/Program.h).
//
// A padding run is compiler alignment filler between accepted code chunks:
// the nop family (any form), int3, and `xchg ax,ax` (`66 90`, which MC
// decodes as an xchg, not a nop): exactly the `analysis::is_filler` set.
// Runs are accepted only under the whole-piece rule, so filler bytes
// inside data blobs are never listed as instructions. `isFillerExt` adds
// the 5-byte `call $+5` shape as an opt-in for non-parity consumers; the
// decode path never uses it.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_PADDING_H
#define REVENANT_ANALYSIS_PADDING_H

#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"

namespace revenant {

class Disasm;
struct Image;
struct Instr;
struct PeFile;

/// True for compiler alignment filler, exactly the reference set in
/// `analysis::is_filler`: nop (any form), int3, and `xchg ax,ax`. This is
/// the only predicate the `decodePadding` path uses, so gap classification
/// matches the Rust tool instruction for instruction. The image overload
/// exists so gap sweeps can pass byte context later; it currently applies
/// the same rule as the decoder-only form below.
bool isFiller(const Instr &I, const Image &Img);

/// Decoder-only form: same strict rule, no byte context needed.
bool isFiller(const Instr &I);

/// Extended filler: the strict set plus the 5-byte `call $+5` (`E8 00 00
/// 00 00`, target == next IP) the toolchain leaves as alignment filler.
/// Opt-in only: never used by `decodePadding`, so parity with the Rust
/// tool is unaffected.
bool isFillerExt(const Instr &I);

/// Byte length of the padding at `Addr`: 0 for non-padding. A zero byte
/// or int3 counts as one padding byte; anything else must decode as
/// filler via `isFiller`.
uint64_t padLen(const Disasm &Dis, const Image &Img, Va Addr);

/// Head of the filler run containing `Addr`, when `Addr` sits inside a
/// filler instruction (e.g. a candidate addressed mid-nop). Nullopt when
/// `Addr` is already an instruction boundary.
std::optional<Va> fillerHead(const Disasm &Dis, Va Addr);

/// True when `Addr` opens a code region right after an int3 padding run
/// that itself sits directly after a `ret` instruction, and `Addr` is on a
/// 16-byte boundary: the MSVC x64 layout for the boundary between two
/// functions. Int3 (never the nop family) is the discriminator because
/// compilers align loops and cold blocks *inside* a function with nop
/// padding. The preceding `ret` separates that from an unconditional `jmp`
/// over int3 padding to an aligned continuation block inside one function.
/// Callers use this to decide that a branch target starts its own function
/// instead of being an intra-function block leader; a zero byte is not
/// padding here (immediates end in zero bytes inside real code).
bool startsAfterInt3(const Disasm &Dis, const Image &Img, Va Addr);

/// Emit `[Start, End)` as one padding run when the whole piece decodes
/// as contiguous filler ending exactly at `End`. Otherwise keep only a
/// leading run that starts exactly at the chunk end and closes on a
/// 16-byte boundary. No interior or suffix runs.
void gapFillers(const Disasm &Dis, const Image &Img, Va Start, Va End,
                std::vector<PaddingRun> &Out);

/// Find alignment filler between accepted code chunks in executable
/// sections: merge every chunk with the image inline-data ranges, bridge
/// tiny filler-only cracks between data ranges, then collect anchored
/// filler runs in each remaining gap piece. The mapped tail past the
/// last chunk follows the same rules. Leading bytes before the first
/// chunk are not padding. Result is sorted by start with exact
/// duplicates removed, matching `Program::Padding`.
std::vector<PaddingRun>
decodePadding(const PeFile &Pe,
              llvm::ArrayRef<std::pair<Va, Va>> Chunks,
              llvm::ArrayRef<std::pair<Va, Va>> Data);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_PADDING_H
