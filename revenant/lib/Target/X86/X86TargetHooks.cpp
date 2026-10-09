//==- lib/Target/X86/X86TargetHooks.cpp - x86 analysis hooks --------------==//
//
// x86-64 implementation of TargetHooks. Skeleton only: the analysis
// porters fill in jump-table shape matching, filler classification,
// noreturn traps and stack-pointer effects from
// crates/analysis/{lib.rs,jump_table.rs,noreturn.rs,stackptr.rs}.
//
//===----------------------------------------------------------------------===//

#include "revenant/Target/TargetHooks.h"

#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"

using namespace llvm;

namespace revenant {

namespace {

class X86TargetHooks final : public TargetHooks {
public:
  StringRef mnemonic(const Instr &I) const override {
    // TODO(analysis): hook MCInstrInfo in at construction and return the
    // opcode mnemonic.
    return "";
  }

  // TODO(analysis): nop (any form), int3, xchg ax,ax.
  bool isFiller(const Instr &I) const override { return false; }

  // TODO(analysis): ud2, int3 as trap, int 29h.
  bool isNoreturnTrap(const Instr &I) const override { return false; }

  // TODO(analysis): push/pop/sub rsp/add rsp imm effects.
  std::optional<int64_t> stackDelta(const Instr &I) const override {
    return std::nullopt;
  }

  // TODO(analysis): port analysis::jump_table::resolve.
  std::optional<JumpTableInfo>
  matchJumpTable(const Image &Img,
                 ArrayRef<std::pair<Va, Va>> CodeRanges,
                 ArrayRef<BasicBlock> Blocks, ArrayRef<Instr> Instrs,
                 size_t Dispatch, const std::vector<Va> &LocalRipRefs,
                 const std::vector<Va> &SharedRipRefs,
                 const std::map<Va, uint64_t> &Limits) const override {
    return std::nullopt;
  }
};

} // end anonymous namespace

std::unique_ptr<TargetHooks> createX86TargetHooks() {
  return std::make_unique<X86TargetHooks>();
}

} // end namespace revenant
