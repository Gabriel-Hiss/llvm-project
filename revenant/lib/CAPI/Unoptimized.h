//===- Unoptimized.h - whole-binary -O0 evidence from machine code -*- C++ -*-===//
//
// rustc -O0 stores the arguments and values it describes in debug info to
// stack slots nothing reads back (`<name>.dbg.spill`); optimized code has no
// write-only stack slot. Per function that is weak evidence (an outgoing
// stack argument looks the same), so the driver decides for the binary: when
// most user functions have such a slot, the binary was built without
// optimization, and every function it compiled itself is lifted as
// unoptimized (`revenant.o0`). clang -O0 reloads its parameter homes, so the
// rule is for rustc binaries.
//
//===----------------------------------------------------------------------===//
#ifndef REVENANT_LIB_CAPI_UNOPTIMIZED_H
#define REVENANT_LIB_CAPI_UNOPTIMIZED_H

#include "revenant/Core/Instr.h"

#include "llvm/ADT/ArrayRef.h"

#include <memory>

namespace llvm {
class MCInstrInfo;
}

namespace revenant {

/// Stack-slot evidence of one function's machine code.
enum class StackSlots : uint8_t {
  /// A stack address is taken or indexed: slots cannot be told apart.
  Unknown,
  /// Every rsp slot the function writes is also read.
  AllRead,
  /// Some rsp slot is written and never read.
  WriteOnly,
};

class StackSlotScanner {
public:
  StackSlotScanner();
  ~StackSlotScanner();
  StackSlots scan(llvm::ArrayRef<Instr> Instrs) const;

private:
  std::unique_ptr<llvm::MCInstrInfo> MII;
};

/// The binary-level verdict over the user functions' evidence.
bool mostlyWriteOnly(unsigned WriteOnly, unsigned Known);

} // namespace revenant

#endif
