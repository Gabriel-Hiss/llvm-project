//==- revenant/Disasm/Disasm.h - MC decoder ------------------------*- C++ -*-==//
//
// MCDisassembler + MCInstrAnalysis + Intel-syntax MCInstPrinter for
// x86-64. Mirrors crates/disasm/src/lib.rs: decode one instruction at a
// VA into `Instr`, format in Intel syntax with symbol resolution.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_DISASM_DISASM_H
#define REVENANT_DISASM_DISASM_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Core/Va.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace llvm {
class MCAsmInfo;
class MCContext;
class MCDisassembler;
class MCInst;
class MCInstPrinter;
class MCInstrAnalysis;
class MCInstrInfo;
class MCRegisterInfo;
class MCSubtargetInfo;
class Target;
class Triple;
} // namespace llvm

namespace revenant {

/// What kind of reference an operand expresses — lets the resolver pick
/// the right name space (function names vs data vs import slots).
enum class RefKind : uint8_t {
  Call,
  Jump,
  /// RIP-relative or absolute data access.
  Data,
  /// Effective address (lea) — usually a global or string.
  AddrOf,
};

/// Supplies symbol names for addresses referenced by instructions.
class NameProvider {
public:
  virtual ~NameProvider() = default;
  virtual std::optional<std::string> nameFor(Va Addr, RefKind Kind) const {
    return std::nullopt;
  }
};

/// Shared resolver handle.
using SharedNames = std::shared_ptr<const NameProvider>;

/// A no-name provider.
struct NoNames : NameProvider {};

/// x86-64 disassembler over a loaded `Image`.
class Disasm {
public:
  /// Build a decoder for `x86_64-pc-windows-msvc`. Calls
  /// `LLVMInitializeX86*` itself; safe to construct many times.
  static llvm::Expected<Disasm> create(const Image &Img);

  ~Disasm();
  Disasm(Disasm &&);
  Disasm &operator=(Disasm &&);
  Disasm(const Disasm &) = delete;

  /// Decode the single instruction starting at `va`.
  std::optional<Instr> decodeOne(Va Addr) const;

  /// Decode instructions in `[start, end)`. Stops at the end of readable
  /// bytes; decode errors stop iteration early.
  std::vector<Instr> decodeRange(Va Start, Va End) const;

  /// Decode all instructions in a byte slice based at `va`.
  std::vector<Instr> decodeSlice(Va Addr, llvm::ArrayRef<uint8_t> Bytes) const;

  /// Format one instruction in Intel syntax with symbol resolution.
  /// `Names` may be `std::make_shared<NoNames>()`.
  std::string format(const Instr &I,
                     const SharedNames &Names = {}) const;

  /// Format a whole sequence (shared printer).
  std::vector<std::string> formatAll(llvm::ArrayRef<Instr> Instrs,
                                     const SharedNames &Names = {}) const;

  /// Raw operand targets analysis needs: near branch target, RIP-relative
  /// memory target, and the ref kind.
  std::vector<std::pair<Va, RefKind>> refs(const Instr &I) const;

  const Image &image() const { return *Img; }

private:
  Disasm() = default;

  const Image *Img = nullptr;
  std::unique_ptr<llvm::Triple> TT;
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
  std::unique_ptr<llvm::MCAsmInfo> MAI;
  std::unique_ptr<llvm::MCSubtargetInfo> STI;
  std::unique_ptr<llvm::MCInstrInfo> MII;
  std::unique_ptr<llvm::MCContext> Ctx;
  std::unique_ptr<llvm::MCDisassembler> Dis;
  std::unique_ptr<llvm::MCInstrAnalysis> MIA;
  std::unique_ptr<llvm::MCInstPrinter> IP;
};

} // end namespace revenant

#endif // REVENANT_DISASM_DISASM_H
