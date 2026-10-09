//==- lib/Target/X86/X86JumpTables.cpp - x86 jump-table operands ---------==//
//
// X86 MC instruction normalization for the generic jump-table engine.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/JumpTable.h"
#include "revenant/Disasm/Disasm.h"

#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86.h"

#include <memory>
#include <string>

using namespace llvm;

namespace revenant {
namespace {

static bool has(StringRef Name, StringRef Part) { return Name.contains(Part); }

class X86JtInsnOps final : public JtInsnOps {
public:
  X86JtInsnOps() {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    Triple TT("x86_64-pc-windows-msvc");
    std::string Err;
    const Target *T = TargetRegistry::lookupTarget(TT, Err);
    if (T)
      MII.reset(T->createMCInstrInfo());
  }

  JtInsn operands(const Instr &I) const override {
    JtInsn O;
    if (!MII || I.Inst.getOpcode() >= MII->getNumOpcodes())
      return O;
    StringRef N = MII->getName(I.Inst.getOpcode());
    const MCInst &MI = I.Inst;
    const unsigned Count = MI.getNumOperands();
    if (has(N, "MOVZX")) O.Mn = JtMnemonic::Movzx;
    else if (has(N, "MOVSX64rm32") || has(N, "MOVSXD")) O.Mn = JtMnemonic::Movsxd;
    else if (has(N, "MOV")) O.Mn = JtMnemonic::Mov;
    else if (has(N, "LEA")) O.Mn = JtMnemonic::Lea;
    else if (has(N, "ADD")) O.Mn = JtMnemonic::Add;
    else if (has(N, "SUB")) O.Mn = JtMnemonic::Sub;
    else if (has(N, "CMP")) O.Mn = JtMnemonic::Cmp;
    else if (has(N, "AND")) O.Mn = JtMnemonic::And;
    else if (has(N, "SHL") || has(N, "SAL")) O.Mn = JtMnemonic::Shl;
    else if (has(N, "SHR")) O.Mn = JtMnemonic::Shr;
    else if (has(N, "IMUL")) O.Mn = JtMnemonic::Imul;
    else if (has(N, "NOOP") || has(N, "NOP")) O.Mn = JtMnemonic::Nop;
    else if (has(N, "INT3")) O.Mn = JtMnemonic::Int3;
    else if (has(N, "XCHG")) O.Mn = JtMnemonic::Xchg;
    if (N == "JCC_1" || N == "JCC_2" || N == "JCC_4") {
      if (Count && MI.getOperand(Count - 1).isImm()) {
        switch (MI.getOperand(Count - 1).getImm()) {
        case X86::COND_A: O.Cond = JtCond::A; break;
        case X86::COND_AE: O.Cond = JtCond::Ae; break;
        case X86::COND_B: O.Cond = JtCond::B; break;
        case X86::COND_BE: O.Cond = JtCond::Be; break;
        case X86::COND_L: O.Cond = JtCond::L; break;
        case X86::COND_LE: O.Cond = JtCond::Le; break;
        case X86::COND_G: O.Cond = JtCond::G; break;
        case X86::COND_GE: O.Cond = JtCond::Ge; break;
        default: break;
        }
      }
    }

    auto IsReg = [&](unsigned K) { return K < Count && MI.getOperand(K).isReg(); };
    auto IsImm = [&](unsigned K) { return K < Count && MI.getOperand(K).isImm(); };
    auto Reg = [&](unsigned K) -> unsigned {
      return IsReg(K) ? (unsigned)MI.getOperand(K).getReg().id() : 0;
    };
    auto SetMem = [&](unsigned K) {
      if (K + 4 >= Count) return false;
      O.MemBase = Reg(K);
      O.MemScale = IsImm(K + 1) ? (uint32_t)MI.getOperand(K + 1).getImm() : 1;
      O.MemIndex = Reg(K + 2);
      O.MemDisp = IsImm(K + 3) ? (uint64_t)MI.getOperand(K + 3).getImm() : 0;
      O.MemIpRel = O.MemBase == X86::RIP;
      if (O.MemIpRel)
        O.MemDisp = I.HasMemTarget
                        ? I.MemTarget.Value
                        : I.nextVa().wrappingAdd(O.MemDisp).Value;
      O.MemSize = memorySize(N);
      return true;
    };
    if (O.Mn == JtMnemonic::Lea && Count >= 6) {
      O.Op0 = JtOpKind::Reg;
      O.Reg0 = Reg(0);
      O.Op1 = JtOpKind::Mem;
      SetMem(1);
      return O;
    }
    if (N.contains("rm") && Count >= 6 && O.Mn != JtMnemonic::Other) {
      O.Op0 = JtOpKind::Reg;
      O.Reg0 = Reg(0);
      O.Op1 = JtOpKind::Mem;
      SetMem(1);
      return O;
    }
    if ((N.contains("mr") || N.contains("mi")) && Count >= 6) {
      O.Op0 = JtOpKind::Mem;
      SetMem(0);
      if (IsReg(5)) { O.Op1 = JtOpKind::Reg; O.Reg1 = Reg(5); }
      else if (IsImm(5)) {
        O.Op1 = JtOpKind::Imm;
        O.Imm = MI.getOperand(5).getImm();
        O.ImmForm = immediateForm(N);
      }
      return O;
    }
    if (O.Mn == JtMnemonic::Other && Count >= 5) {
      O.Op0 = JtOpKind::Mem;
      SetMem(0);
      return O;
    }
    if (IsReg(0)) {
      O.Op0 = JtOpKind::Reg;
      O.Reg0 = Reg(0);
    }
    if (IsReg(1) && !(N.contains("ri") && IsImm(2))) {
      O.Op1 = JtOpKind::Reg;
      unsigned Src = (N.contains("rr") && IsReg(2) && Reg(1) == Reg(0))
                         ? 2
                         : 1;
      O.Reg1 = Reg(Src);
    } else if (IsImm(1) || (N.contains("ri") && IsImm(2))) {
      O.Op1 = JtOpKind::Imm;
      unsigned ImmOp = IsImm(1) ? 1 : 2;
      O.Imm = MI.getOperand(ImmOp).getImm();
      O.ImmForm = immediateForm(N);
    } else if (Count >= 7 && SetMem(2)) {
      O.Op1 = JtOpKind::Mem;
    } else if (Count == 6 && SetMem(1)) {
      O.Op1 = JtOpKind::Mem;
    }
    return O;
  }

  unsigned fullReg(unsigned Reg) const override {
    return Reg ? getX86SubSuperRegister(Reg, 64).id() : 0;
  }

  std::unique_ptr<JtFillDecoder> fillDecoder(const Image &Img) const override {
    return std::make_unique<FillDecoder>(Img);
  }

private:
  class FillDecoder final : public JtFillDecoder {
  public:
    explicit FillDecoder(const Image &Img) {
      auto D = Disasm::create(Img);
      if (D) Decoder = std::make_unique<Disasm>(std::move(*D));
    }
    std::optional<JtFill> decodeOne(Va Addr) override {
      if (!Decoder) return std::nullopt;
      auto I = Decoder->decodeOne(Addr);
      if (!I) return std::nullopt;
      JtInsn O = getX86JumpTableInsnOps()->operands(*I);
      JtFill F;
      F.Size = I->Size;
      const bool XchgAxAx =
          O.Mn == JtMnemonic::Xchg && O.Reg0 == X86::AX && O.Reg1 == X86::AX;
      F.Filler = O.Mn == JtMnemonic::Nop || O.Mn == JtMnemonic::Int3 ||
                 XchgAxAx;
      F.MultiNop = O.Mn == JtMnemonic::Nop && I->Size > 1;
      return F;
    }
  private:
    std::unique_ptr<Disasm> Decoder;
  };

  static JtImmForm immediateForm(StringRef N) {
    if (has(N, "ri8") || has(N, "mi8")) return JtImmForm::I8Wide;
    if (has(N, "8ri") || has(N, "8mi") || has(N, "i8"))
      return JtImmForm::I8;
    if (has(N, "i16")) return JtImmForm::I16;
    if (has(N, "64ri32") || has(N, "64mi32"))
      return JtImmForm::I32To64;
    if (has(N, "i32")) return JtImmForm::I32;
    if (has(N, "i64")) return JtImmForm::I64;
    return JtImmForm::None;
  }
  static uint32_t memorySize(StringRef N) {
    if (has(N, "8rm") || has(N, "rm8") || has(N, "m8")) return 1;
    if (has(N, "16rm") || has(N, "rm16") || has(N, "m16")) return 2;
    if (has(N, "32rm") || has(N, "rm32") || has(N, "m32")) return 4;
    if (has(N, "64rm") || has(N, "rm64") || has(N, "m64")) return 8;
    return 0;
  }
  std::unique_ptr<MCInstrInfo> MII;
};

} // namespace

const JtInsnOps *getX86JumpTableInsnOps() {
  static const X86JtInsnOps Ops;
  return &Ops;
}

} // namespace revenant
