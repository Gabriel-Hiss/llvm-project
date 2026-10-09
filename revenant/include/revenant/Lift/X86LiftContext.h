//===- X86LiftContext.h - x86-64 lift state ---------------------*- C++ -*-===//
//
// X86 implementation of LiftContext: GPR/vector register file with subregister
// merge semantics, EFLAGS as separate i1 slots, the five-operand memory form,
// prefix tracking, stack helpers, and every semantic hook the generated
// lift table and the DSL bodies call.
//
// Register model: every register belongs to a root super-register that owns
// one alloca. A 32-bit GPR write zero-extends into the root; narrower writes
// merge at the subregister offset; the high 8-bit registers merge at bit 8.
// Vector roots store their full width as an integer and bitcast on access
// (legacy SSE forms merge, VEX 128-bit forms use xmmWrite which zeroes the
// upper lanes).
//
// Prefix model: the MC disassembler emits LOCK/REP/segment overrides as
// separate opcodes (LOCK_PREFIX, REP_PREFIX, ...) or REP_* pseudos. The
// context records them as pending state; rv_hasrep/rv_haslock read it and the
// next real instruction consumes it.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIFT_X86LIFTCONTEXT_H
#define REVENANT_LIFT_X86LIFTCONTEXT_H

#include "revenant/Lift/LiftContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm {
namespace revenant {

class X86LiftContext : public LiftContext {
public:
  /// Individual flags, matching crates/lift flags.rs: each is a named i1
  /// alloca. DF is included because string instructions consume it.
  enum FlagId { CF, ZF, SF, OF, PF, AF, DF, NUM_FLAGS };

  X86LiftContext(LLVMContext &C, Module &M, Function &F,
                 const MCInstrInfo &MCII, const MCRegisterInfo &TRI);

  // ---- LiftContext -------------------------------------------------------
  OpKind opKind(const MCInst &MI, unsigned MIOpNo) const override;
  Value *memAddr(const MCInst &MI, unsigned MIOpNo) override;
  Value *regRead(MCRegister Reg, Type *Ty) override;
  void regWrite(MCRegister Reg, Value *V) override;
  void jumpToVA(Value *Target) override;
  void condJump(Value *Pred, Value *Target) override;
  void callOp(const MCInst &MI, unsigned MIOpNo) override;
  void retOp() override;

  // ---- flags -------------------------------------------------------------
  virtual Value *flagRead(FlagId F);
  virtual void flagWrite(FlagId F, Value *I1);
  /// Undef i32 standing in for the SDAG-level EFLAGS operand of flag nodes.
  Value *eflagsIn();

  // ---- flag-setting arithmetic (result value, flags as side effect) ------
  virtual Value *flagsAdd(Value *A, Value *B);
  virtual Value *flagsSub(Value *A, Value *B);
  virtual Value *flagsLogic(Value *R);  // and/or/xor/test: CF=OF=0, AF=undef
  Value *flagsAdc(Value *A, Value *B);  // A + B + CF
  Value *flagsSbb(Value *A, Value *B);  // A - B - CF
  virtual Value *flagsNeg(Value *A);    // 0 - A; CF = (A != 0)
  virtual Value *flagsInc(Value *A);    // add semantics, CF preserved
  virtual Value *flagsDec(Value *A);    // sub semantics, CF preserved
  virtual Value *flagsCmp(Value *A, Value *B); // sub flags, discards result
  Value *flagsSMul(Value *A, Value *B); // low result; CF=OF=(high != sext)
  Value *flagsUMul(Value *A, Value *B); // low result; CF=OF=(high != 0)

  /// Shift family. Name: "shl"|"shr"|"sar"|"rol"|"ror". Flags follow Intel:
  /// count 0 -> all preserved; count 1 -> exact CF/OF; count >1 -> CF exact,
  /// OF undef. Result for count > width: 0 (Intel leaves it undefined).
  Value *flagsShift(StringRef Name, Value *V, Value *Cnt);
  /// SHLD/SHRD: (V, Extra, Cnt). Extra supplies the shifted-in bits.
  Value *flagsShift2(StringRef Name, Value *V, Value *Extra, Value *Cnt);

  // ---- condition codes -----------------------------------------------------
  /// i1 predicate from the current flags. CC is X86::CondCode (O=0..G=15).
  virtual Value *condPred(unsigned CC);
  /// condPred read from an operand carrying the immediate CondCode.
  Value *condPredOp(const MCInst &MI, unsigned MIOpNo);
  /// zext(condPred) to i8.
  Value *setccVal(unsigned CC);

  // ---- control flow --------------------------------------------------------
  /// Branch target for a pcrel immediate operand: nextVA + imm.
  Value *pcrelTarget(const MCInst &MI, unsigned MIOpNo);
  /// jmp: imm -> pending Jump to pcrel target; else jump to operand value.
  void brOp(const MCInst &MI, unsigned MIOpNo);
  void brccOp(const MCInst &MI, unsigned DstOpNo, unsigned CondOpNo);
  /// call: push return address, record Call pending flow.
  void callToOp(const MCInst &MI, unsigned MIOpNo);
  /// ret / ret imm16: record Return; stack pop handled by driver later.
  void retAdjOp(const MCInst &MI, unsigned AdjOpNo);

  // ---- stack ---------------------------------------------------------------
  virtual void pushVal(Value *V);        // rsp -= size; store V
  virtual Value *popVal(unsigned Width); // load; rsp += size; yield value
  void pushFlagsVal();               // push packed rflags image (i16)
  void popFlagsVal();                // pop into flag slots
  virtual void leaveOp(); // rsp = rbp; rbp = pop

  // ---- moves / exchange ----------------------------------------------------
  void xchgOps(const MCInst &MI, unsigned AOp, unsigned BOp);
  /// xadd: returns dst+src with add flags, writes old dst to src operand.
  Value *xaddVal(const MCInst &MI, unsigned DstOp, unsigned SrcOp, Value *A,
                 Value *B);

  // ---- bit ops ---------------------------------------------------------------
  /// BT family. Op: "bt"|"bts"|"btr"|"btc". Returns the bit value (i1) for
  /// "bt"; for the set/reset/complement forms returns the modified base.
  /// CF = tested bit. For memory bases with large offsets the addressed bit
  /// crosses element boundaries; the helper does the full address arithmetic.
  Value *bitTestOp(StringRef Op, Value *Base, Value *Off, bool IsMem,
                   Value *MemAddr, unsigned EltWidth);
  Value *bsfOp(Value *V);   // index of lowest set bit; ZF = (V==0), undef res
  Value *bsrOp(Value *V);   // index of highest set bit; ZF = (V==0)

  /// Write a reg/mem operand, zero-extending the value to the root register
  /// width instead of merging (VEX 128-bit write semantics).
  void writeOpZ(const MCInst &MI, unsigned MIOpNo, Value *V);
  virtual void regWriteZ(MCRegister Reg, Value *V);

  /// float/double/x86_fp80 by bit count for typed memory loads.
  Type *fltTy(unsigned Bits);

  /// Vector/scalar helper dispatch for DSL ops that are easier in C++ than
  /// in IRBuilder text: lane access, shuffles, movemasks, broadcasts,
  /// packed shifts. Imm is -1 when unused. Returns nullptr for void ops.
  Value *vop(StringRef Name, Value *A, Value *Bv = nullptr,
             int64_t Imm = -1);
  /// PTEST: ZF = (a & b) == 0, CF = (~a & b) == 0, OF/SF/AF/PF = 0.
  void ptestOp(Value *A, Value *Bv);
  Value *tzcntOp(Value *V); // width when V==0 (BMI TZCNT)
  Value *lzcntOp(Value *V);
  Value *popcntOp(Value *V);
  Value *bswapOp(Value *V);
  Value *rorxOp(Value *V, Value *Cnt); // rotate right, no flags

  // ---- mul/div families ----------------------------------------------------
  /// Unsigned one-operand mul: rax*src -> rdx:rax, CF=OF=(high!=0).
  void mulPair(Value *Src, unsigned W, bool Signed);
  /// Unsigned/signed divide of rdx:rax by Src: quot->eax, rem->edx.
  /// Trap semantics are not modeled (division by zero yields undef).
  void divPair(Value *Src, unsigned W, bool Signed);
  /// CBW/CWDE/CDQE/CWD/CDQ/CQO: sign-extend the accumulator. W = target width.
  void sextAcc(unsigned W);

  // ---- flag image ----------------------------------------------------------
  Value *packFlags(unsigned Width);  // rflags image (CF|PF|AF|ZF|SF|OF, DF)
  void unpackFlags(Value *V);        // inverse of packFlags
  void sahfOp();                     // SF/ZF/AF/PF/CF from AH
  Value *lahfVal();                  // flags image into AH

  // ---- cmpxchg -------------------------------------------------------------
  /// cmpxchg: [addr] vs A; equal -> [addr]=new, ZF=1; else A=[addr], ZF=0.
  /// Writes full sub-style flags. Returns the loaded old value.
  Value *cmpxchgOp(Value *Addr, Value *Expected, Value *New);
  /// CMPXCHG8B/16B: rdx:rax vs rbx:rcx pair through memory of iW (W=64|128).
  void cmpxchgPair(Value *Addr, unsigned W);

  // ---- atomics ---------------------------------------------------------------
  Value *atomicRMW(StringRef Name, Value *Addr, Value *Val);
  void emitFence();

  // ---- bulk memory -----------------------------------------------------------
  void memMove(Value *Dst, Value *Src, Value *Len);
  void memSet(Value *Dst, Value *Val, Value *Len);

  // ---- prefixes --------------------------------------------------------------
  /// Record a pending prefix. Kind: "lock"|"rep"|"repe"|"repne"|"seg".
  /// "seg" takes the segment register as Reg.
  void prefixOp(StringRef Kind, MCRegister Reg = MCRegister());
  bool hasRep() const { return PendingRep == Rep; }
  bool hasRepE() const { return PendingRep == RepE || PendingRep == Rep; }
  bool hasRepNE() const { return PendingRep == RepNE; }
  bool hasAnyRep() const { return PendingRep != NoRep; }
  bool hasLock() const { return PendingLock; }
  MCRegister pendingSeg() const { return PendingSeg; }
  /// Clear pending prefix state. Called after each real instruction lifts.
  void consumePrefixes();

  // ---- x87 FPU -----------------------------------------------------------------
  /// ST(i) relative to TOP, as x86_fp80. The stack is eight allocas indexed
  /// by (Top + i) & 7 with Top kept in an alloca.
  Value *x87St(unsigned I);
  void x87StSet(unsigned I, Value *V);
  void x87Push(Value *V);            // top = top-1; ST(0) = V
  Value *x87Pop();                   // yield ST(0); top = top+1
  void x87Init();
  void x87IncTop();
  void x87DecTop();
  /// C0..C3 status-word flags (separate allocas, distinct from EFLAGS).
  Value *x87CFlag(unsigned I);
  void x87SetCFlag(unsigned I, Value *V);
  /// fcom ST(i): sets C0/C2/C3. Null V compares against 0 (FTST).
  void x87FCom(Value *V);
  /// fcomi ST(i): sets ZF/PF/CF in EFLAGS (unordered -> all 1).
  void x87FComI(Value *V);
  /// Status word as i16 (TOP and C0..C3 packed; rest zero).
  Value *x87StatusWord();
  void x87SetStatusWord(Value *V);
  void x87SetControlWord(Value *V);
  Value *x87ControlWord();
  /// Named constants: 0,1,pi,l2t,l2e,lg2,ln2,z(-0). Returns x86_fp80.
  Value *x87Const(StringRef Name);
  /// One-operand stack math: sqrt/sin/cos/patan/atan/prem/scale/abs/chs/
  /// rnd/extract/decstp/incstp/free/tst/exam implemented on TOP or pairs.
  Value *x87Math(StringRef Name, Value *A, Value *B = nullptr);
  /// FLDENV/FNSTENV 28-byte image at Addr.
  void x87Env(StringRef Dir, Value *Addr);
  /// Stack index (0-7) of an RSTi operand register, for DSL rv_x87idx.
  unsigned x87IndexOf(MCRegister Reg) const;
  /// FXCH: swap ST(0) with ST(i).
  void x87Xchg(unsigned I);


protected:
  enum RepKind { NoRep, Rep, RepE, RepNE };

  const MCInstrInfo &MCII;
  const MCRegisterInfo &TRI;
  DenseMap<MCRegister, AllocaInst *> Slots;      // root reg -> slot
  DenseMap<MCRegister, unsigned> SlotBits;       // root reg -> slot width
  AllocaInst *Flags[NUM_FLAGS] = {};
  AllocaInst *X87Stack = nullptr;   // [8 x x86_fp80]
  AllocaInst *X87Top = nullptr;     // i8
  AllocaInst *X87CW = nullptr;      // i16
  AllocaInst *X87CFlags[4] = {};    // C0..C3
  RepKind PendingRep = NoRep;
  bool PendingLock = false;
  MCRegister PendingSeg;

  /// Root register that owns Reg's slot (top of the super-register chain).
  MCRegister rootReg(MCRegister Reg) const;
  /// Width of Reg in bits via its minimal physreg class.
  unsigned regBits(MCRegister Reg) const;
  /// Byte offset of Reg inside its root, and its width in bits.
  std::pair<unsigned, unsigned> regOffsetBits(MCRegister Reg) const;
  AllocaInst *slotFor(MCRegister Reg);
  /// Read Reg's bits out of its root slot as iN(N = regBits(Reg)).
  Value *regBitsRead(MCRegister Reg);
  /// Merge a value of exactly regBits(Reg) bits into the root slot.
  /// ZeroExtend32: dst-width 32 in a 64-bit root zeroes the top half.
  void regBitsWrite(MCRegister Reg, Value *V);
  Value *toInt(Value *V, unsigned W);
  Value *fromInt(Value *V, Type *Ty);
  /// Parity of the low byte of V.
  Value *parityOf(Value *V);
  /// Common ZF/SF/PF writes for a result value.
  void flagsZSP(Value *R);
  Value *gepStack(Value *Idx);       // &X87Stack[idx]
  /// Lazily create the x87 stack/top/control/flag allocas.
  void ensureX87();
};

/// Lift one decoded X86 instruction, returning false when no generated
/// semantic or supported direct SelectionDAG pattern matches.
bool liftX86Inst(X86LiftContext &LC, const MCInst &MI, uint64_t VA,
                 uint64_t Size);

} // namespace revenant
} // namespace llvm

#endif // REVENANT_LIFT_X86LIFTCONTEXT_H
