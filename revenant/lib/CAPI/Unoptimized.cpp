//===- Unoptimized.cpp - whole-binary -O0 evidence from machine code ------===//
//
// See Unoptimized.h.
//
//===----------------------------------------------------------------------===//
#include "Unoptimized.h"

#include "X86BaseInfo.h"
#include "X86MCTargetDesc.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/TargetRegistry.h"

#include <string>

using namespace llvm;

namespace revenant {

StackSlotScanner::StackSlotScanner() {
  std::string Err;
  // Disasm::create initialized the X86 target before any function was
  // materialized.
  if (const Target *T = TargetRegistry::lookupTarget("x86_64-pc-windows-msvc", Err))
    MII.reset(T->createMCInstrInfo());
}

StackSlotScanner::~StackSlotScanner() = default;

StackSlots StackSlotScanner::scan(ArrayRef<Instr> Instrs) const {
  if (!MII)
    return StackSlots::Unknown;
  DenseSet<int64_t> Written, Read;
  for (const Instr &I : Instrs) {
    const MCInstrDesc &D = MII->get(I.Inst.getOpcode());
    int MemNo = X86II::getMemoryOperandNo(D.TSFlags);
    if (MemNo < 0)
      continue;
    unsigned Mem = MemNo + X86II::getOperandBias(D);
    if (Mem + X86::AddrNumOperands > I.Inst.getNumOperands())
      continue;
    const MCOperand &Base = I.Inst.getOperand(Mem + X86::AddrBaseReg);
    if (!Base.isReg() || Base.getReg() != X86::RSP)
      continue;
    const MCOperand &Index = I.Inst.getOperand(Mem + X86::AddrIndexReg);
    const MCOperand &Disp = I.Inst.getOperand(Mem + X86::AddrDisp);
    // `lea r, [rsp+X]` passes a stack address on; an indexed access reads
    // a computed slot.
    if ((Index.isReg() && Index.getReg() != 0) || !Disp.isImm() ||
        (!D.mayLoad() && !D.mayStore()))
      return StackSlots::Unknown;
    if (D.mayStore() && !D.mayLoad())
      Written.insert(Disp.getImm());
    else
      Read.insert(Disp.getImm());
  }
  for (int64_t Off : Written)
    if (!Read.contains(Off))
      return StackSlots::WriteOnly;
  return StackSlots::AllRead;
}

bool mostlyWriteOnly(unsigned WriteOnly, unsigned Known) {
  return Known != 0 && 2 * WriteOnly >= Known;
}

} // namespace revenant
