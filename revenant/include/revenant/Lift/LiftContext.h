//===- LiftContext.h - operand/state interface for lifted IR ----*- C++ -*-===//
//
// Interface between generated/hand-written instruction semantics and the
// lifted function's CPU state. A lifter creates one context per function,
// positions the builder at the current block, then calls the semantics for
// each decoded instruction.
//
// State model (mirrors crates/lift of the Rust implementation):
//  - registers are entry-block allocas addressed through regRead/regWrite,
//    so SSA form falls out of mem2reg/SROA later;
//  - condition flags are separate i1 values (target subclass);
//  - memory is addressed by an i64 effective address produced by memAddr()
//    and accessed through memRead/memWrite;
//  - every emitted instruction carries !revenant.va metadata for the VA of
//    the instruction it was lifted from (setVA writes it into the builder's
//    MetadataToCopy, so all subsequent IRBuilder creations are tagged).
//
// The class is target-neutral: it knows nothing about x86. The x86 subclass
// (X86LiftContext) implements register/flag/addressing specifics.
//
// Until the revenant-core Instr lands, callers pass (MCInst, VA, Size).
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIFT_LIFTCONTEXT_H
#define REVENANT_LIFT_LIFTCONTEXT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/MC/MCInst.h"
#include <cstdint>
#include <vector>

namespace llvm {

namespace revenant {

class LiftContext {
public:
  using BuilderTy = IRBuilder<ConstantFolder, IRBuilderCallbackInserter>;

  /// How a machine operand participates in an MCInst.
  enum class OpKind { Reg, Imm, Mem, Invalid };

  /// A pending control transfer produced by a control-flow instruction.
  /// Function-level drivers consume these to wire real basic blocks; the
  /// single-instruction path just records them.
  struct PendingFlow {
    enum Kind { Jump, CondJump, Call, Return } K;
    uint64_t VA = 0;          // VA of the source instruction
    Value *TargetV = nullptr; // i64 target (jump/call), else nullptr
    Value *Pred = nullptr;    // i1 condition for CondJump
  };

  LiftContext(LLVMContext &C, Module &M, Function &F);
  virtual ~LiftContext() = default;

  LLVMContext &ctx() const { return Ctx; }
  Module &module() const { return Mod; }
  Function &function() const { return *Fn; }
  BuilderTy &builder() { return B; }

  // ---- provenance ----------------------------------------------------------

  /// VA of the instruction currently being lifted. Every instruction created
  /// after this call gets !revenant.va !{i64 <VA>} until setVA is called
  /// again (0 clears it).
  void setVA(uint64_t VA);
  uint64_t curVA() const { return CurVA; }

  /// End VA (VA + Size) of the instruction being lifted; needed for
  /// RIP-relative and call-return-address semantics.
  void setNextVA(uint64_t VA) { NextVA = VA; }
  uint64_t nextVA() const { return NextVA; }

  /// Emit `call void @revenant.asm.<name>(i64 <operands...>)` carrying
  /// !revenant.opaque for an instruction without lifted semantics.
  virtual void emitOpaque(const MCInst &MI, StringRef Name);

  // ---- types and constants -------------------------------------------------

  IntegerType *iN(unsigned Bits) { return IntegerType::get(Ctx, Bits); }
  Type *ptrTy() { return PointerType::getUnqual(Ctx); }
  ConstantInt *constI(uint64_t V, unsigned Bits) {
    return ConstantInt::get(iN(Bits), V);
  }
  Value *zextOrTrunc(Value *V, Type *To) { return B.CreateZExtOrTrunc(V, To); }
  Value *sextOrTrunc(Value *V, Type *To) { return B.CreateSExtOrTrunc(V, To); }

  // ---- registers -----------------------------------------------------------

  /// Registers live in entry-block allocas; keys and widths are
  /// target-defined. On x86 a 32-bit subregister write zeroes the top half,
  /// 8/16-bit writes merge, and the high-byte regs (AH..BH) update
  /// bits [15:8]. The subclass implements all of that in regRead/regWrite.
  virtual Value *regRead(MCRegister Reg, Type *Ty) = 0;
  virtual void regWrite(MCRegister Reg, Value *V) = 0;

  /// Allocate an entry-block alloca; subclasses call this from their
  /// constructor while laying out the register file.
  AllocaInst *regSlot(StringRef Name, Type *Ty);

  // ---- operands ------------------------------------------------------------

  /// Classify the machine operand at \p MIOpNo (the target knows its
  /// encoding, e.g. the five-operand memory group on x86).
  virtual OpKind opKind(const MCInst &MI, unsigned MIOpNo) const = 0;

  /// Effective address of the memory operand starting at \p MIOpNo, as i64.
  /// RIP-relative/absolute forms are resolved against the image by the
  /// target subclass (which sees the VA through setVA/setNextVA).
  virtual Value *memAddr(const MCInst &MI, unsigned MIOpNo) = 0;

  Value *readRegOp(const MCInst &MI, unsigned MIOpNo, Type *Ty);
  Value *readImmOp(const MCInst &MI, unsigned MIOpNo, Type *Ty);
  Value *memRead(const MCInst &MI, unsigned MIOpNo, Type *Ty);
  void memWrite(const MCInst &MI, unsigned MIOpNo, Value *V);
  Value *memReadAddr(Value *AddrI64, Type *Ty);
  void memWriteAddr(Value *AddrI64, Value *V);

  /// Read any operand at \p MIOpNo as \p Ty (reg -> regRead, imm -> constant,
  /// mem -> memRead). For memory this is a load; use memAddr for lea.
  Value *readOp(const MCInst &MI, unsigned MIOpNo, Type *Ty);
  /// Write to a reg or mem operand.
  void writeOp(const MCInst &MI, unsigned MIOpNo, Value *V);

  // ---- absolute addresses --------------------------------------------------

  /// Turn a concrete image VA (RIP-relative target, absolute memory operand,
  /// relocated immediate) into an IR pointer. The default materializes a
  /// constant inttoptr; a subclass backed by the module's image maps to the
  /// corresponding global.
  virtual Value *resolveVA(uint64_t VA);
  /// Same value as an i64 (for LEA / integer-valued operands).
  virtual Value *resolveVAI64(uint64_t VA);

  // ---- control flow --------------------------------------------------------

  /// Record a control transfer; the function-level driver resolves these into
  /// real branches/calls/rets once it owns the CFG. The defaults only record.
  virtual void jumpToVA(Value *Target);
  virtual void condJump(Value *Pred, Value *Target);
  virtual void callOp(const MCInst &MI, unsigned MIOpNo);
  virtual void retOp();

  std::vector<PendingFlow> &pendingFlows() { return Flows; }

protected:
  LLVMContext &Ctx;
  Module &Mod;
  Function *Fn;
  IRBuilderCallbackInserter Inserter;
  BuilderTy B;
  MDNode *VAMD = nullptr;
  unsigned VAMDKind;
  unsigned OpaqueMDKind;
  uint64_t CurVA = 0;
  uint64_t NextVA = 0;
  std::vector<PendingFlow> Flows;
};

} // namespace revenant
} // namespace llvm

#endif // REVENANT_LIFT_LIFTCONTEXT_H
