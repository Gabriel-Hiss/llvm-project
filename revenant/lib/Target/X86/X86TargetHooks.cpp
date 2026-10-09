//==- lib/Target/X86/X86TargetHooks.cpp - x86 analysis hooks --------------==//
//
// The x86 implementation of the target queries used by discovery,
// jump-table recovery, and stack analysis.
//
//===----------------------------------------------------------------------===//

#include "revenant/Target/TargetHooks.h"

#include "revenant/Analysis/JumpTable.h"
#include "revenant/Target/X86StackEffects.h"

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

using namespace llvm;

namespace revenant {
namespace {

bool isAccumulator(const MCRegisterInfo &MRI, unsigned Reg, unsigned Width) {
  StringRef Name = MRI.getName(Reg);
  switch (Width) {
  case 16:
    return Name == "AX";
  case 32:
    return Name == "EAX";
  case 64:
    return Name == "RAX";
  default:
    return false;
  }
}

class X86TargetHooks final : public TargetHooks {
public:
  X86TargetHooks(std::unique_ptr<MCInstrInfo> MII,
                 std::unique_ptr<MCRegisterInfo> MRI)
      : MII(std::move(MII)), MRI(std::move(MRI)) {}

  StringRef mnemonic(const Instr &I) const override {
    return MII->getName(I.Inst.getOpcode());
  }

  bool isFiller(const Instr &I) const override {
    return isNopOrInt3(I) || isXchgAxAx(I);
  }

  TrapKind trapKind(const Instr &I) const override {
    StringRef Name = mnemonic(I);
    if (Name == "TRAP" || Name.starts_with("UD0") ||
        Name.starts_with("UD1") || Name.starts_with("UD2"))
      return TrapKind::Exception;
    if (Name == "INT3")
      return TrapKind::Absorbable;
    if (Name == "INT" && I.Inst.getNumOperands() != 0 &&
        I.Inst.getOperand(0).isImm() && I.Inst.getOperand(0).getImm() == 0x29)
      return TrapKind::Interrupt;
    return TrapKind::NotTrap;
  }

  bool isNopOrInt3(const Instr &I) const override {
    StringRef Name = mnemonic(I);
    if (Name == "INT3" || Name.starts_with("NOOP"))
      return true;
    if (Name == "XCHG16ar" && I.Inst.getNumOperands() > 0 &&
        I.Inst.getOperand(0).isReg())
      return isAccumulator(*MRI, I.Inst.getOperand(0).getReg(), 16);
    if (Name == "XCHG32ar" && I.Inst.getNumOperands() > 0 &&
        I.Inst.getOperand(0).isReg())
      return isAccumulator(*MRI, I.Inst.getOperand(0).getReg(), 32);
    if (Name == "XCHG64ar" && I.Inst.getNumOperands() > 0 &&
        I.Inst.getOperand(0).isReg())
      return isAccumulator(*MRI, I.Inst.getOperand(0).getReg(), 64);
    return false;
  }

  bool isOpaqueTailJmp(const Image &Img, const Instr &I) const override {
    if (!mnemonic(I).starts_with("JMP64"))
      return false;
    const MCInst &M = I.Inst;
    if (M.getNumOperands() == 0)
      return false;
    if (M.getOperand(0).isReg())
      return true;
    // X86 memory operands are base, scale, index, displacement, segment.
    if (M.getNumOperands() < 5)
      return false;
    if (M.getOperand(2).isReg() && M.getOperand(2).getReg() != 0)
      return false;
    const MCOperand &Base = M.getOperand(0);
    if (Base.isReg() && StringRef(MRI->getName(Base.getReg())) == "RIP")
      return I.HasMemTarget && !Img.isExecutable(I.MemTarget);
    return true;
  }

  std::optional<int64_t> stackDelta(const Instr &I) const override {
    return x86_stack::stackDelta(*MII, *MRI, I);
  }

  StackSlots stackSlots(const Instr &I) const override {
    return x86_stack::stackSlots(*MII, *MRI, I);
  }

  FpEstablish fpEstablish(const Instr &I) const override {
    return x86_stack::fpEstablish(*MII, *MRI, I);
  }

  bool restoresSpFromFp(const Instr &I) const override {
    return x86_stack::restoresSpFromFp(*MII, *MRI, I);
  }

  std::optional<int64_t> movEaxImm(const Instr &I) const override {
    return x86_stack::movEaxImm(*MII, *MRI, I);
  }

  bool writesSpDynamically(const Instr &I) const override {
    return x86_stack::writesSpDynamically(*MII, *MRI, I);
  }

  const JtInsnOps *jumpTableInsnOps() const override {
    return getX86JumpTableInsnOps();
  }

  std::optional<JumpTableInfo>
  matchJumpTable(const Image &Img, ArrayRef<std::pair<Va, Va>> CodeRanges,
                 ArrayRef<BasicBlock> Blocks, ArrayRef<Instr> Instrs,
                 size_t Dispatch, const std::vector<Va> &LocalRipRefs,
                 const std::vector<Va> &SharedRipRefs,
                 const std::map<Va, uint64_t> &Limits) const override {
    std::set<Va> Local(LocalRipRefs.begin(), LocalRipRefs.end());
    std::set<Va> Shared(SharedRipRefs.begin(), SharedRipRefs.end());
    JumpTableCtx Ctx{Img, CodeRanges, Local, Shared, Limits};
    return resolveJumpTable(Ctx, *this, Blocks, Instrs, Dispatch);
  }

private:
  bool isXchgAxAx(const Instr &I) const {
    if (mnemonic(I) != "XCHG16rr" || I.Inst.getNumOperands() < 2)
      return false;
    for (const MCOperand &Op : I.Inst)
      if (!Op.isReg() || StringRef(MRI->getName(Op.getReg())) != "AX")
        return false;
    return true;
  }

  std::unique_ptr<MCInstrInfo> MII;
  std::unique_ptr<MCRegisterInfo> MRI;
};

} // namespace

std::unique_ptr<TargetHooks> createX86TargetHooks() {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86Disassembler();

  Triple TT("x86_64-pc-windows-msvc");
  std::string Error;
  const Target *T = TargetRegistry::lookupTarget(TT, Error);
  if (!T)
    return nullptr;
  std::unique_ptr<MCRegisterInfo> MRI(T->createMCRegInfo(TT));
  std::unique_ptr<MCInstrInfo> MII(T->createMCInstrInfo());
  if (!MRI || !MII)
    return nullptr;
  return std::make_unique<X86TargetHooks>(std::move(MII), std::move(MRI));
}

} // namespace revenant
