//===- PairReturns.cpp - rax:rdx return evidence from call sites ----------===//
//
// See PairReturns.h.
//
//===----------------------------------------------------------------------===//
#include "PairReturns.h"

#include "X86MCTargetDesc.h"

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/TargetParser/Triple.h"

#include <string>

using namespace llvm;

namespace revenant {

namespace {
/// How far past a call the scan looks for the first rdx access.
constexpr unsigned Window = 16;

enum class RdxUse { None, Read, Written };

/// `xor edx, edx` and `sub edx, edx` name rdx only to clear it.
bool zeroIdiom(const MCInst &I, const MCRegisterInfo &MRI) {
  switch (I.getOpcode()) {
  case X86::XOR32rr:
  case X86::XOR64rr:
  case X86::SUB32rr:
  case X86::SUB64rr:
    return I.getNumOperands() >= 3 && I.getOperand(1).isReg() &&
           I.getOperand(2).isReg() &&
           I.getOperand(1).getReg() == I.getOperand(2).getReg() &&
           MRI.regsOverlap(I.getOperand(1).getReg(), X86::RDX);
  default:
    return false;
  }
}

RdxUse rdxUse(const MCInst &I, const MCInstrDesc &D, const MCRegisterInfo &MRI) {
  if (zeroIdiom(I, MRI))
    return RdxUse::Written;
  for (unsigned K = D.getNumDefs(); K < I.getNumOperands(); ++K) {
    const MCOperand &Op = I.getOperand(K);
    if (Op.isReg() && Op.getReg() && MRI.regsOverlap(Op.getReg(), X86::RDX))
      return RdxUse::Read;
  }
  for (MCPhysReg R : D.implicit_uses())
    if (MRI.regsOverlap(R, X86::RDX))
      return RdxUse::Read;
  if (D.hasDefOfPhysReg(I, X86::RDX, MRI))
    return RdxUse::Written;
  return RdxUse::None;
}
} // namespace

PairReturnScanner::PairReturnScanner() {
  std::string Err;
  Triple TT("x86_64-pc-windows-msvc");
  // Disasm::create initialized the X86 target before any function was
  // materialized.
  if (const Target *T = TargetRegistry::lookupTarget(TT, Err)) {
    MII.reset(T->createMCInstrInfo());
    MRI.reset(T->createMCRegInfo(TT));
  }
}

PairReturnScanner::~PairReturnScanner() = default;

void PairReturnScanner::scan(const Function &F,
                             DenseSet<uint64_t> &Targets) const {
  if (!MII || !MRI)
    return;
  for (const BasicBlock &BB : F.Blocks)
    for (size_t N = BB.InstrsBegin; N < BB.InstrsEnd; ++N) {
      const Instr &Call = F.Instrs[N];
      if (Call.Flow != FlowKind::Call || !Call.HasTarget)
        continue;
      // The instructions after the call: the rest of this block, or the
      // block the call falls through to.
      size_t K = N + 1, End = BB.InstrsEnd;
      if (K == End) {
        const BasicBlock *Next = F.blockContaining(Call.nextVa());
        if (!Next || Next->Start != Call.nextVa())
          continue;
        K = Next->InstrsBegin;
        End = Next->InstrsEnd;
      }
      for (unsigned Seen = 0; K < End && Seen < Window; ++K, ++Seen) {
        const Instr &I = F.Instrs[K];
        if (I.Flow != FlowKind::None)
          break;
        RdxUse U = rdxUse(I.Inst, MII->get(I.Inst.getOpcode()), *MRI);
        if (U == RdxUse::Read)
          Targets.insert(Call.Target.Value);
        if (U != RdxUse::None)
          break;
      }
    }
}

} // namespace revenant
