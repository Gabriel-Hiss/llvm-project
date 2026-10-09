//==- revenant/Core/Program.h - Analyzed program model ------------*- C++ -*-==//
//
// Mirrors crates/analysis/src/lib.rs: CFG edges and blocks, the fat
// `Function` and the compact `FuncRecord` form `Program` stores, xrefs,
// padding runs, externs and name tables.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CORE_PROGRAM_H
#define REVENANT_CORE_PROGRAM_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Va.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

namespace revenant {

/// Supplies symbol names for addresses referenced by instructions
/// (implemented by revenant::Resolver / the disasm layer).
class NameProvider;

/// One decoded Rich-header-ish enum kept out: see revenant/Object/PeFile.h
/// for loader-side records.

//==--------------------------------------------------------------------==//
// CFG edges
//==--------------------------------------------------------------------==//

/// Kind of a CFG edge (Rust `analysis::EdgeKind`).
enum class EdgeKind : uint8_t {
  /// Straight-line continuation into the next block.
  Fallthrough,
  /// Unconditional intra-function jump.
  Jump,
  /// Conditional branch taken.
  CondTaken,
  /// Conditional branch not taken (fallthrough of a jcc).
  CondNotTaken,
  /// Indirect jump resolved through a jump table; `SwitchTable`/`Values`
  /// carry the payload. One edge per distinct target.
  Switch,
  /// The guard jcc's out-of-range edge (default branch).
  SwitchDefault,
  /// A `jmp` to another function's start (or out of this function's
  /// pdata range into a different function).
  TailCall,
};

/// One CFG edge to a target VA.
struct Edge {
  Va To;
  EdgeKind Kind = EdgeKind::Fallthrough;
  /// Switch payload: table VA and the case constants (index + base
  /// offset) reaching this target. Only set for `EdgeKind::Switch`.
  Va SwitchTable;
  std::vector<int64_t> SwitchValues;
};

/// How a basic block ends (Rust `analysis::BlockEnd`).
enum class BlockEnd : uint8_t {
  /// Continues into the next block in address order.
  Fallthrough,
  /// Direct conditional/unconditional branch.
  Branch,
  /// Ends with a tail `jmp` to another function.
  TailCall,
  Return,
  /// ud2 / int3 / int 29h / call to a noreturn callee.
  Noreturn,
  /// Ends in an indirect jump we could not resolve (kept, never dropped).
  UnresolvedIndirect,
  /// Ran out of decodable bytes.
  EndOfFunction,
};

/// What the guard's `cmp` compares against the bound: a register or a
/// memory slot (stack slot at -O0). Register ids are MC register numbers
/// (X86::RAX, …; LLVM MC operands' reg id space).
struct CmpOperand {
  enum class Kind : uint8_t { Reg, Mem } Tag = Kind::Reg;
  /// `Kind::Reg`: the register holding the index.
  unsigned Reg = 0;
  /// `Kind::Mem`: effective-address descriptor.
  unsigned Base = 0;
  unsigned Index = 0;
  uint32_t Scale = 0;
  uint64_t Disp = 0;

  bool operator==(const CmpOperand &O) const {
    return Tag == O.Tag && Reg == O.Reg && Base == O.Base &&
           Index == O.Index && Scale == O.Scale && Disp == O.Disp;
  }
};

/// One basic block.
struct BasicBlock {
  Va Start;
  /// Exclusive end (== start + decoded instruction bytes).
  Va End;
  /// Indices into `Function::Instrs`.
  size_t InstrsBegin = 0;
  size_t InstrsEnd = 0;
  std::vector<Edge> Edges;
  BlockEnd EndKind = BlockEnd::Fallthrough;
  /// For blocks ending in a resolved switch dispatch: the register or
  /// memory slot holding the table index at the `jmp`, plus the base
  /// offset (case = idx + off).
  std::optional<std::pair<CmpOperand, int64_t>> SwitchIndex;
};

/// Class of a cross-reference (Rust `analysis::XrefKind`).
enum class XrefKind : uint8_t {
  Call,
  Jump,
  Read,
  Write,
  AddrOf,
};

/// One reference to a target VA.
struct Xref {
  Va From;
  XrefKind Kind;
};

/// A discovered function (fat form; `Program` stores `FuncRecord`).
struct Function {
  Va Start;
  /// Max block end (functions need not be contiguous).
  Va End;
  std::string Name;
  /// A primary `.pdata` entry supplied this start and owns its code region.
  bool HasPdata = false;
  std::vector<Instr> Instrs;
  std::vector<BasicBlock> Blocks;
  /// Sorted, disjoint code ranges belonging to this function.
  std::vector<std::pair<Va, Va>> Chunks;
  /// Data byte ranges inside this function's region (resolved or
  /// recognized jump-table spans): consumed by the gap sweep and padding
  /// classification, never emitted as code.
  std::vector<std::pair<Va, Va>> DataRanges;
  /// Indirect jump sites that stayed unresolved.
  std::vector<Va> UnresolvedIndirects;
  bool IsNoreturn = false;
  /// Call sites to noreturn callees that terminate paths.
  std::vector<Va> NoreturnCalls;
  /// Direct call targets discovered while building (discovery feed).
  std::vector<Va> CallTargets;
  /// Block roots no CFG edge reaches (e.g. unwind landing pads found by
  /// the own-region sweep). Downstream consumers skip predecessor checks.
  std::vector<Va> ExtraRoots;
  /// No `.pdata` and no caller: found by the executable-gap sweep.
  bool Orphan = false;

  bool contains(Va Addr) const {
    for (auto [S, E] : Chunks)
      if (Addr >= S && Addr < E)
        return true;
    return false;
  }

  const BasicBlock *blockContaining(Va Addr) const {
    // Blocks are sorted by start and disjoint once built.
    size_t Lo = 0, Hi = Blocks.size();
    while (Lo < Hi) {
      size_t Mid = Lo + (Hi - Lo) / 2;
      if (Blocks[Mid].Start <= Addr)
        Lo = Mid + 1;
      else
        Hi = Mid;
    }
    if (Lo > 0 && Addr < Blocks[Lo - 1].End)
      return &Blocks[Lo - 1];
    for (const BasicBlock &B : Blocks)
      if (Addr >= B.Start && Addr < B.End)
        return &B;
    return nullptr;
  }
};

/// External call target (IAT slot -> import).
struct Extern {
  /// VA of the IAT slot.
  Va Slot;
  /// `dll!name` or `dll.#ord`.
  std::string Name;
  /// A noreturn API (ExitProcess, abort, ...).
  bool Noreturn = false;
};

//==--------------------------------------------------------------------==//
// Compact program records (RVA pools, Rust FuncRecord/BlockRec/EdgeRec)
//==--------------------------------------------------------------------==//

/// `EdgeKind` without payloads, one byte. `Switch` payloads live in
/// `FuncRecord::SwitchEdges`.
enum class EdgeTag : uint8_t {
  Fallthrough,
  Jump,
  CondTaken,
  CondNotTaken,
  Switch,
  SwitchDefault,
  TailCall,
};

/// Compact edge: `To` is `target - image.base`, or `UINT32_MAX` (real VA
/// in `FuncRecord::EdgeOut`).
struct EdgeRec {
  uint32_t To = 0;
  EdgeTag Kind = EdgeTag::Fallthrough;
};

/// Payload of one `EdgeTag::Switch` edge.
struct SwitchEdge {
  /// Index into `FuncRecord::Edges`.
  uint32_t Edge = 0;
  Va Table;
  std::vector<int64_t> Values;
};

/// One basic block in a `FuncRecord`: u32 RVAs + u32 pool ranges + u8 kind.
struct BlockRec {
  /// `start`/`end` are `va - image.base`.
  Rva Start = 0;
  /// Exclusive end RVA (== start + decoded instruction bytes).
  Rva End = 0;
  /// Indices into `FuncRecord::InstrRvas`.
  uint32_t InstrsBegin = 0;
  uint32_t InstrsEnd = 0;
  /// Indices into `FuncRecord::Edges`.
  uint32_t EdgesBegin = 0;
  uint32_t EdgesEnd = 0;
  /// Index into `FuncRecord::SwitchMeta`, or `UINT32_MAX`.
  uint32_t SwitchIndex = UINT32_MAX;
  BlockEnd EndKind = BlockEnd::Fallthrough;

  Va startVa(const Image &Img) const { return Img.vaOfRva(Start); }
  Va endVa(const Image &Img) const { return Img.vaOfRva(End); }
};

/// A function kept compact: instruction addresses as RVAs (decoded bytes
/// live in the image), block instr/edge lists as ranges into flat pools.
struct FuncRecord {
  Va Start;
  /// Max block end (functions need not be contiguous).
  Va End;
  std::string Name;
  /// A primary `.pdata` entry supplied this start and owns its code region.
  bool HasPdata = false;
  /// Instruction RVAs (`va - image.base`), in original decode order;
  /// block `instrs` ranges index into this array.
  std::vector<Rva> InstrRvas;
  /// Indices into `InstrRvas`, sorted by RVA, for va -> index lookups.
  std::vector<uint32_t> Sorted;
  std::vector<BlockRec> Blocks;
  /// Flat edge pool; a block's edges are `Edges[EdgesBegin..EdgesEnd]`.
  std::vector<EdgeRec> Edges;
  /// Payloads of `EdgeTag::Switch` edges, in edge-index order.
  std::vector<SwitchEdge> SwitchEdges;
  /// `(CmpOperand, offset)` of each block that had a `switch_index`,
  /// addressed by `BlockRec::SwitchIndex`.
  std::vector<std::pair<CmpOperand, int64_t>> SwitchMeta;
  /// `(edge index, target)` for edges whose target is not a u32 RVA
  /// off `image.base` (out-of-image jumps); `EdgeRec::To` is UINT32_MAX.
  std::vector<std::pair<uint32_t, Va>> EdgeOut;
  /// Sorted, disjoint code ranges belonging to this function.
  std::vector<std::pair<Va, Va>> Chunks;
  /// Indirect jump sites that stayed unresolved.
  std::vector<Va> UnresolvedIndirects;
  /// Call sites to noreturn callees that terminate paths.
  std::vector<Va> NoreturnCalls;
  /// Direct call targets discovered while building (discovery feed).
  std::vector<Va> CallTargets;
  /// Block roots no CFG edge reaches (landing pads etc.).
  std::vector<Va> ExtraRoots;
  /// No `.pdata`, no caller: found by the executable-gap sweep.
  bool Orphan = false;
  bool IsNoreturn = false;

  /// VA of instruction `i` (0-based into `InstrRvas`).
  Va instrVa(const Image &Img, size_t I) const {
    return Img.vaOfRva(InstrRvas[I]);
  }

  /// Instruction index at `va` (decode order), if one starts there.
  std::optional<size_t> instrIndex(const Image &Img, Va Addr) const {
    auto Rva = Img.rvaOf(Addr);
    if (!Rva)
      return std::nullopt;
    auto It = std::partition_point(
        Sorted.begin(), Sorted.end(),
        [&](uint32_t J) { return InstrRvas[J] < *Rva; });
    if (It != Sorted.end() && InstrRvas[*It] == *Rva)
      return *It;
    return std::nullopt;
  }

  /// Instruction count below `rva`: for row arithmetic (duplicates count).
  size_t instrsBelow(Rva R) const {
    return std::partition_point(Sorted.begin(), Sorted.end(),
                                [&](uint32_t J) { return InstrRvas[J] < R; }) -
           Sorted.begin();
  }

  /// Compact edges of a block.
  llvm::ArrayRef<EdgeRec> edgesOf(const BlockRec &B) const {
    return llvm::ArrayRef<EdgeRec>(Edges).slice(B.EdgesBegin,
                                                B.EdgesEnd - B.EdgesBegin);
  }

  /// Target VA of edge `i` (global index into `Edges`).
  Va edgeTo(const Image &Img, size_t I) const {
    uint32_t To = Edges[I].To;
    if (To != UINT32_MAX)
      return Img.vaOfRva(To);
    for (auto [Idx, Target] : EdgeOut)
      if (Idx == I)
        return Target;
    return Img.Base;
  }

  /// `(CmpOperand, offset)` of a block's switch dispatch, if it had one.
  std::optional<std::pair<CmpOperand, int64_t>>
  blockSwitchIndex(const BlockRec &B) const {
    if (B.SwitchIndex == UINT32_MAX)
      return std::nullopt;
    return SwitchMeta[B.SwitchIndex];
  }
};

/// One gap between code chunks that linear-decodes as filler.
struct PaddingRun {
  /// First filler's RVA (`va - image.base`).
  Rva Start = 0;
  /// Exclusive end RVA.
  Rva End = 0;
  /// RVA of each decoded filler instruction, in order.
  std::vector<Rva> Instrs;
};

/// xrefs as flat sorted arrays: `Froms[Offsets[i]..Offsets[i+1]]` with
/// matching `Kinds` are the sources of `Targets[i]`.
struct Xrefs {
  /// Target RVAs (`va - base`), sorted.
  std::vector<Rva> Targets;
  /// Ref-group boundaries into `Froms`/`Kinds`; `size() == Targets.size()+1`.
  std::vector<uint32_t> Offsets;
  /// Source RVA of each ref.
  std::vector<Rva> Froms;
  std::vector<XrefKind> Kinds;
  /// Image base for RVA -> VA translation.
  Va Base;

  /// `(from, kind)` pairs that reference `va`.
  std::vector<Xref> xrefsTo(Va Addr) const {
    std::vector<Xref> Out;
    auto Off = Addr.offsetFrom(Base);
    if (!Off || *Off > UINT32_MAX)
      return Out;
    auto It = llvm::lower_bound(Targets, (Rva)*Off);
    if (It == Targets.end() || *It != (Rva)*Off)
      return Out;
    size_t I = It - Targets.begin();
    for (uint32_t J = Offsets[I]; J < Offsets[I + 1]; ++J)
      Out.push_back({Va(Base.Value + Froms[J]), Kinds[J]});
    return Out;
  }

  /// Every xreffed target VA.
  std::vector<Va> targetVas() const {
    std::vector<Va> Out;
    Out.reserve(Targets.size());
    for (Rva R : Targets)
      Out.push_back(Va(Base.Value + R));
    return Out;
  }

  size_t targetCount() const { return Targets.size(); }
  size_t totalRefs() const { return Froms.size(); }
};

/// The analyzed program: image, functions, padding, xrefs, names.
struct Program {
  std::shared_ptr<Image> Img;
  /// function start -> compact record
  std::map<Va, FuncRecord> Functions;
  /// Executable-section gaps between code chunks whose whole range is a
  /// linear decode of `nop`/`int3` instructions, sorted by `Start`.
  std::vector<PaddingRun> Padding;
  /// `Froms[Offsets[i]..Offsets[i+1]]` are the sources of `Targets[i]`.
  Xrefs Xr;
  /// referenced strings: va -> text
  std::shared_ptr<std::map<Va, std::string>> Strings;
  /// IAT slot -> extern.
  std::shared_ptr<std::map<Va, Extern>> Externs;
  /// VA -> display name (functions, data, imports).
  std::shared_ptr<std::map<Va, std::string>> Names;
  /// Control Flow Guard slots from the load config directory (see
  /// `PeFile::GuardCFCheckPtr`): `call [GuardCFDispatchPtr]` is an indirect
  /// call through the target in rax; `call [GuardCFCheckPtr]` is a
  /// validation-only call before a real `call rcx`.
  std::optional<Va> GuardCFCheckPtr;
  std::optional<Va> GuardCFDispatchPtr;
  /// The shared resolver for disasm formatting.
  std::shared_ptr<const NameProvider> Resolver;
};

} // end namespace revenant

#endif // REVENANT_CORE_PROGRAM_H
