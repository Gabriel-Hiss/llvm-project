//===- LiftX86.h - generated x86 lift table on top of LiftState -----------===//
//
// Internal to lib/Lift/Function. `GenContext` is the X86LiftContext the
// generated `liftX86Inst` table runs against, backed by one function's
// LiftState: GPR and low-xmm allocas, the flag allocas, the rsp frame model,
// image globals and the flag-producer fusion all stay in LiftState; the
// context only adapts them to the interface the generated bodies call.
//
// Registers, flags and memory the LiftState does not model (AF, DF, the high
// ymm lane, k/mm/segment registers, x87 state, pending prefixes) live in the
// X86LiftContext base as lazily created entry-block slots.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIFT_LIFTX86_H
#define REVENANT_LIFT_LIFTX86_H

#include "LiftState.h"
#include "revenant/Lift/X86LiftContext.h"

namespace revenant::lift {

class GenContext final : public llvm::revenant::X86LiftContext {
public:
  explicit GenContext(LiftState &LS);

  /// Run the generated table for `I` at the end of the current block.
  /// Returns false, leaving no new instructions behind, when the table has
  /// no semantics for the opcode (its case is the opaque fallback).
  bool lift(const Instr &I);

  // ---- LiftContext -----------------------------------------------------------
  llvm::Value *regRead(llvm::MCRegister Reg, llvm::Type *Ty) override;
  OpKind opKind(const llvm::MCInst &MI, unsigned MIOpNo) const override;
  void regWrite(llvm::MCRegister Reg, llvm::Value *V) override;
  llvm::Value *memAddr(const llvm::MCInst &MI, unsigned MIOpNo) override;
  llvm::Value *addrToPtr(llvm::Value *AddrI64) override;
  void emitOpaque(const llvm::MCInst &MI, llvm::StringRef Name) override;

  // ---- X86LiftContext --------------------------------------------------------
  llvm::Value *flagRead(FlagId F) override;
  void flagWrite(FlagId F, llvm::Value *I1) override;
  llvm::Value *condPred(unsigned CC) override;
  llvm::Value *flagsAdd(llvm::Value *A, llvm::Value *B) override;
  llvm::Value *flagsSub(llvm::Value *A, llvm::Value *B) override;
  llvm::Value *flagsLogic(llvm::Value *R) override;
  llvm::Value *flagsNeg(llvm::Value *A) override;
  llvm::Value *flagsInc(llvm::Value *A) override;
  llvm::Value *flagsDec(llvm::Value *A) override;
  llvm::Value *flagsCmp(llvm::Value *A, llvm::Value *B) override;
  void pushVal(llvm::Value *V) override;
  llvm::Value *popVal(unsigned Width) override;
  void leaveOp() override;
  void regWriteZ(llvm::MCRegister Reg, llvm::Value *V) override;

private:
  LiftState &LS;
  /// Instruction being lifted; memAddr maps its MCInst operand index back to
  /// the logical memory operand LiftState understands.
  const Instr *Cur = nullptr;
  /// Set by emitOpaque or an unsupported register: the instruction is rolled
  /// back and left to the hand-lifted path.
  mutable bool Opaque = false;
  /// Some instruction in the function reads or writes a ymm register, so
  /// VEX.128 writes must clear the upper lane.
  bool UsesYmm = false;
  /// i64 address values memAddr produced from a pointer, so the access goes
  /// through the original pointer instead of an inttoptr round trip.
  llvm::DenseMap<llvm::Value *, llvm::Value *> PtrOf;
  /// Upper 128 bits of ymm0-15 (the low half is LiftState's xmm slot).
  llvm::AllocaInst *YmmHi[16] = {};

  llvm::AllocaInst *ymmHi(unsigned N);
  /// GPR roots and rsp, which LiftState owns.
  static bool ownsScalar(llvm::MCRegister R);
};

} // namespace revenant::lift

#endif // REVENANT_LIFT_LIFTX86_H
