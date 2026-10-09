//==- lib/Target/X86/X86StackEffects.cpp - x86 stack effects ---------==//
//
// x86-64 stack-pointer effects behind TargetHooks (Rust
// crates/analysis/src/stackptr.rs `apply` / `check_frame_ptr` / `imm` /
// `bump`, in MC terms). Free helpers returning exactly the TargetHooks
// types; LunaDiscovery wires them into X86TargetHooks (which owns that
// file) and declares the interface in Target/TargetHooks.h:
//
//   StackBase::{Rsp,Rbp}
//   StackSlots { Slots, HasDynamicMem }
//   FpEstablish::Kind::{None,MovRbpRsp,LeaRbpRsp} + Disp
//   stackDelta / stackSlots / fpEstablish / restoresSpFromFp /
//   movEaxImm / writesSpDynamically
//
// Conventions (no generated X86 headers needed):
// - Opcode identity via MCInstrInfo names: PUSH* (PUSH64r/PUSH64rmm/
//   PUSHF*), POP* (POP64r/POP64rmm/POPF*), SUB*/ADD* (any width; the
//   immediate is the MC operand), LEA64r/LEA64_32r, MOV (MOV64rr/MOV32rr
//   + MOV64ri32/MOV32ri for the chkstk feeder), LEAVE64/LEAVE.
// - Register identity via MCRegisterInfo names (lowercased): rsp/esp,
//   rbp/ebp, eax.
// - Memory operands via MCInstrDesc: the first OPERAND_MEMORY operand
//   starts the 5-slot [base,scale,index,disp,seg] group (same layout
//   X86::getFirstAddrOperandIdx uses for real instructions).
// - stackDelta returns the persistent delta *change* (push=+8, pop=-8,
//   sub rsp,imm=+imm, add rsp,imm=-imm, lea rsp,[rsp+disp]=-disp), to be
//   added to the running delta. Nullopt = no simple effect; consult
//   writesSpDynamically to separate "no write" from "dynamic write".
//   LEAVE / mov rsp,rbp return nullopt with writesSpDynamically=false:
//   the generic walk handles them through restoresSpFromFp first.
// - 32-bit ESP/EBP/EAX forms count as their 64-bit counterparts (Rust
//   matches RSP|ESP, RBP|EBP, and reads EAX for the chkstk feeder).
//
//===----------------------------------------------------------------------===//

#include "revenant/Target/X86StackEffects.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace revenant {
namespace x86_stack {

namespace {

std::string lower(llvm::StringRef S) {
  std::string Out(S.str());
  for (char &C : Out)
    C = (char)std::tolower((unsigned char)C);
  return Out;
}

bool isSpName(llvm::StringRef N) { return N == "rsp" || N == "esp"; }
bool isFpName(llvm::StringRef N) { return N == "rbp" || N == "ebp"; }

std::string regName(const llvm::MCRegisterInfo &MRI, unsigned Reg) {
  const char *N = MRI.getName(llvm::MCRegister(Reg));
  return N ? lower(N) : std::string();
}

bool isSpReg(const llvm::MCRegisterInfo &MRI, unsigned Reg) {
  return isSpName(regName(MRI, Reg));
}

bool isFpReg(const llvm::MCRegisterInfo &MRI, unsigned Reg) {
  return isFpName(regName(MRI, Reg));
}

bool isEaxReg(const llvm::MCRegisterInfo &MRI, unsigned Reg) {
  std::string N = regName(MRI, Reg);
  return N == "eax";
}


struct MemRef {
  bool Found = false;
  unsigned Base = 0;
  unsigned Index = 0;
  int64_t Disp = 0;
  bool BaseIsReg = false;
  bool IndexIsReg = false;
};


bool isPushName(llvm::StringRef N) {
  return N.starts_with("PUSH");
}

bool isPopName(llvm::StringRef N) {
  return N.starts_with("POP") && !N.starts_with("POPCNT");
}

/// Register-to-register move in either encoding (`8B /r` decodes as
/// `MOV64rr_REV`). `Width` is the register-name stem, e.g. "MOV64rr".
bool isMovRR(llvm::StringRef N, llvm::StringRef Stem) {
  return N == Stem || (N.starts_with(Stem) && N.drop_front(Stem.size()) == "_REV");
}

/// Def operand 0 is a register (the SUB/ADD/MOV/LEA destination form the
/// reference models: `writes_rsp` = op0 is rsp/esp).
bool defIsSp(const llvm::MCInstrInfo &MII, const llvm::MCRegisterInfo &MRI,
             const llvm::MCInst &M) {
  // A memory-destination form (`add [rsp+8],rbx`) lists the address base as
  // operand 0 without defining it; only a real register def counts.
  if (M.getNumOperands() < 1 || !M.getOperand(0).isReg() ||
      MII.get(M.getOpcode()).getNumDefs() < 1)
    return false;
  return isSpReg(MRI, M.getOperand(0).getReg());
}

/// Immediate operand for SUB/ADD rsp,imm: first immediate operand at or
/// after index 1 (past the tied def).
std::optional<int64_t> arithImm(const llvm::MCInst &M) {
  for (unsigned I = 1, E = M.getNumOperands(); I < E; ++I)
    if (M.getOperand(I).isImm())
      return M.getOperand(I).getImm();
  return std::nullopt;
}

/// All 5-slot memory groups in the MCInst (see stackSlots for the shape).
std::vector<MemRef> memGroups(const llvm::MCInst &M) {
  std::vector<MemRef> Out;
  for (unsigned S = 0; S + 5 <= M.getNumOperands(); ++S) {
    if (!M.getOperand(S).isReg() || !M.getOperand(S + 1).isImm() ||
        !M.getOperand(S + 2).isReg() || !M.getOperand(S + 4).isReg())
      continue;
    if (!M.getOperand(S + 3).isImm())
      continue;
    int64_t Scale = M.getOperand(S + 1).getImm();
    if (Scale != 1 && Scale != 2 && Scale != 4 && Scale != 8)
      continue;
    MemRef R;
    R.Found = true;
    R.Base = M.getOperand(S).getReg();
    R.Index = M.getOperand(S + 2).getReg();
    R.Disp = M.getOperand(S + 3).getImm();
    R.BaseIsReg = R.Base != 0;
    R.IndexIsReg = R.Index != 0;
    Out.push_back(R);
  }
  return Out;
}

} // end anonymous namespace

std::optional<int64_t> stackDelta(const llvm::MCInstrInfo &MII,
                                  const llvm::MCRegisterInfo &MRI,
                                  const Instr &I) {
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  if (isPushName(Name))
    return 8;
  if (isPopName(Name))
    return -8;
  if (Name.starts_with("SUB") && defIsSp(MII, MRI, M)) {
    // Register-source sub (sub rsp, rax) is dynamic, not a delta.
    if (auto Imm = arithImm(M))
      return *Imm;
    return std::nullopt;
  }
  if (Name.starts_with("ADD") && defIsSp(MII, MRI, M)) {
    if (auto Imm = arithImm(M))
      return -*Imm;
    return std::nullopt;
  }
  if ((Name == "LEA64r" || Name == "LEA64_32r") && defIsSp(MII, MRI, M)) {
    for (const MemRef &R : memGroups(M))
      if (R.BaseIsReg && isSpReg(MRI, R.Base) && !R.IndexIsReg)
        return -R.Disp;
    return std::nullopt;
  }
  return std::nullopt;
}

StackSlots stackSlots(const llvm::MCInstrInfo &MII,
                      const llvm::MCRegisterInfo &MRI, const Instr &I) {
  StackSlots Out;
  const llvm::MCInst &M = I.Inst;
  (void)MII;
  for (const MemRef &R : memGroups(M)) {
    if (R.IndexIsReg) {
      // Indexed form (`[base+idx*scale]`): table dispatch, never a frame
      // slot. Indexed off rsp/rbp additionally marks dynamic memory.
      if (R.BaseIsReg &&
          (isSpReg(MRI, R.Base) || isFpReg(MRI, R.Base)))
        Out.HasDynamicMem = true;
      continue;
    }
    if (!R.BaseIsReg)
      continue;
    if (isSpReg(MRI, R.Base))
      Out.Slots.emplace_back(StackBase::Rsp, R.Disp);
    else if (isFpReg(MRI, R.Base))
      Out.Slots.emplace_back(StackBase::Rbp, R.Disp);
  }
  return Out;
}

FpEstablish fpEstablish(const llvm::MCInstrInfo &MII,
                        const llvm::MCRegisterInfo &MRI, const Instr &I) {
  FpEstablish Out;
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  if ((isMovRR(Name, "MOV64rr") || isMovRR(Name, "MOV32rr")) &&
      M.getNumOperands() >= 2 &&
      M.getOperand(0).isReg() && M.getOperand(1).isReg() &&
      isFpReg(MRI, M.getOperand(0).getReg()) &&
      isSpReg(MRI, M.getOperand(1).getReg())) {
    Out.Tag = FpEstablish::Kind::MovRbpRsp;
    return Out;
  }
  if ((Name == "LEA64r" || Name == "LEA64_32r") && M.getNumOperands() >= 1 &&
      M.getOperand(0).isReg() && isFpReg(MRI, M.getOperand(0).getReg())) {
    for (const MemRef &R : memGroups(M))
      if (R.BaseIsReg && isSpReg(MRI, R.Base) && !R.IndexIsReg) {
        Out.Tag = FpEstablish::Kind::LeaRbpRsp;
        Out.Disp = R.Disp;
      }
  }
  return Out;
}

bool restoresSpFromFp(const llvm::MCInstrInfo &MII,
                      const llvm::MCRegisterInfo &MRI, const Instr &I) {
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  if (Name == "LEAVE" || Name == "LEAVE64")
    return true;
  // mov rsp, rbp (any MOV*rr width, including 32-bit esp,ebp).
  if (Name.starts_with("MOV") &&
      (Name.ends_with("rr") || Name.ends_with("rr_REV")) &&
      M.getNumOperands() >= 2 && M.getOperand(0).isReg() &&
      M.getOperand(1).isReg() && isSpReg(MRI, M.getOperand(0).getReg()) &&
      isFpReg(MRI, M.getOperand(1).getReg()))
    return true;
  return false;
}

std::optional<int64_t> movEaxImm(const llvm::MCInstrInfo &MII,
                                 const llvm::MCRegisterInfo &MRI,
                                 const Instr &I) {
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  if (!Name.starts_with("MOV"))
    return std::nullopt;
  if (M.getNumOperands() < 2 || !M.getOperand(0).isReg() ||
      !M.getOperand(1).isImm())
    return std::nullopt;
  if (!isEaxReg(MRI, M.getOperand(0).getReg()))
    return std::nullopt;
  return M.getOperand(1).getImm();
}

bool writesSpDynamically(const llvm::MCInstrInfo &MII,
                         const llvm::MCRegisterInfo &MRI, const Instr &I) {
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  // Modeled or neutralized elsewhere: never dynamic.
  if (isPushName(Name) || isPopName(Name))
    return false;
  if (Name == "LEAVE" || Name == "LEAVE64")
    return false;
  if (restoresSpFromFp(MII, MRI, I))
    return false;
  if ((Name.starts_with("SUB") || Name.starts_with("ADD")) &&
      defIsSp(MII, MRI, M))
    return !arithImm(M).has_value();
  if ((Name == "LEA64r" || Name == "LEA64_32r") && defIsSp(MII, MRI, M)) {
    // The simple [rsp+disp] form is modeled; any other LEA writing rsp
    // (indexed, absolute, rip-relative) is dynamic.
    for (const MemRef &R : memGroups(M))
      if (R.BaseIsReg && isSpReg(MRI, R.Base) && !R.IndexIsReg)
        return false;
    return true;
  }
  // Any other def of rsp/esp (mov rsp,rax; and rsp,-N; xchg; ...),
  // explicit or implicit.
  if (defIsSp(MII, MRI, M))
    return true;
  const llvm::MCInstrDesc &Desc = MII.get(M.getOpcode());
  for (llvm::MCPhysReg R : Desc.implicit_defs())
    if (R && isSpReg(MRI, R))
      return true;
  return false;
}

std::optional<std::pair<unsigned, int64_t>>
spCopy(const llvm::MCInstrInfo &MII, const llvm::MCRegisterInfo &MRI,
       const Instr &I) {
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  if (M.getNumOperands() < 1 || !M.getOperand(0).isReg() ||
      isSpReg(MRI, M.getOperand(0).getReg()))
    return std::nullopt;
  const unsigned Dst = M.getOperand(0).getReg();
  if (isMovRR(Name, "MOV64rr") && M.getNumOperands() >= 2 &&
      M.getOperand(1).isReg() && isSpReg(MRI, M.getOperand(1).getReg()))
    return std::pair<unsigned, int64_t>(Dst, 0);
  if (Name == "LEA64r")
    for (const MemRef &R : memGroups(M))
      if (R.BaseIsReg && isSpReg(MRI, R.Base) && !R.IndexIsReg)
        return std::pair<unsigned, int64_t>(Dst, R.Disp);
  return std::nullopt;
}

std::optional<unsigned> spRestoreFrom(const llvm::MCInstrInfo &MII,
                                      const llvm::MCRegisterInfo &MRI,
                                      const Instr &I) {
  const llvm::MCInst &M = I.Inst;
  llvm::StringRef Name = MII.getName(M.getOpcode());
  if (isMovRR(Name, "MOV64rr") && M.getNumOperands() >= 2 && M.getOperand(0).isReg() &&
      M.getOperand(1).isReg() && isSpReg(MRI, M.getOperand(0).getReg()) &&
      !isFpReg(MRI, M.getOperand(1).getReg()))
    return (unsigned)M.getOperand(1).getReg();
  return std::nullopt;
}

} // end namespace x86_stack
} // end namespace revenant
