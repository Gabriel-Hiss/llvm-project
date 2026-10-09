//==- revenant/Target/X86StackEffects.h - x86 stack effects --*- C++ -*-==//
//
// Free helpers behind TargetHooks' stack queries (implemented in
// lib/Target/X86/X86StackEffects.cpp, owned by MuseStackPtr). LunaDiscovery
// owns X86TargetHooks.cpp and wires these into the overrides; this header
// is the exact API to call.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TARGET_X86STACKEFFECTS_H
#define REVENANT_TARGET_X86STACKEFFECTS_H

#include "revenant/Analysis/StackPtr.h"
#include "revenant/Core/Instr.h"

#include <cstdint>
#include <optional>

namespace llvm {
class MCInstrInfo;
class MCRegisterInfo;
} // namespace llvm

namespace revenant {

namespace x86_stack {

/// Persistent delta *change* (push=+8, pop=-8, sub rsp,imm=+imm,
/// add rsp,imm=-imm, lea rsp,[rsp+disp]=-disp). Nullopt = no simple
/// effect; use writesSpDynamically to separate none from dynamic.
std::optional<int64_t> stackDelta(const llvm::MCInstrInfo &MII,
                                  const llvm::MCRegisterInfo &MRI,
                                  const Instr &I);

/// Frame slots referenced without an index register: (Rsp|Rbp, disp).
/// Indexed references off rsp/rbp set HasDynamicMem.
StackSlots stackSlots(const llvm::MCInstrInfo &MII,
                      const llvm::MCRegisterInfo &MRI, const Instr &I);

/// mov rbp,rsp / lea rbp,[rsp+disp] establishment (disp = the LEA
/// displacement; generic computes rbp_delta = delta - disp).
FpEstablish fpEstablish(const llvm::MCInstrInfo &MII,
                        const llvm::MCRegisterInfo &MRI, const Instr &I);

/// leave / mov rsp,rbp (any MOV*rr width, incl. 32-bit esp,ebp).
bool restoresSpFromFp(const llvm::MCInstrInfo &MII,
                      const llvm::MCRegisterInfo &MRI, const Instr &I);

/// mov eax,imm value (any MOV* width with an immediate second operand).
std::optional<int64_t> movEaxImm(const llvm::MCInstrInfo &MII,
                                 const llvm::MCRegisterInfo &MRI,
                                 const Instr &I);

/// True when the instruction writes rsp/esp in a form stackDelta does not
/// model (register-source sub/add, indexed/absolute LEA to rsp,
/// mov/and/... to rsp). Push/pop/leave/restores are never dynamic.
bool writesSpDynamically(const llvm::MCInstrInfo &MII,
                         const llvm::MCRegisterInfo &MRI, const Instr &I);

} // end namespace x86_stack
} // end namespace revenant

#endif // REVENANT_TARGET_X86STACKEFFECTS_H
