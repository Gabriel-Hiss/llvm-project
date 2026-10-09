//===- LiftState.h - per-function x86-64 lift state -------------*- C++ -*-===//
//
// Internal to lib/Lift/Function. Mirrors the `Lift` struct of crates/lift:
// register-file allocas, flag allocas, the frame model, block map, flag
// producer tracking and per-instruction operand access.
//
// The iced `Instruction` operand model maps onto MCInst + MCInstrDesc:
// logical operands are the TableGen `outs` then the untied `ins`, where a
// five-operand x86 memory group counts as one logical operand.
//
// `StackInfo` is a private port of crates/analysis/stackptr.rs; it moves to
// lib/Analysis once MuseStackPtr's lands.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIFT_LIFTSTATE_H
#define REVENANT_LIFT_LIFTSTATE_H

#include "revenant/Core/Program.h"
#include "revenant/Lift/LiftFunction.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegister.h"
#include "llvm/MC/MCRegisterInfo.h"

namespace llvm {
class MCInstrInfo;
class MCRegisterInfo;
class Target;
} // namespace llvm

namespace revenant {

class Disasm;

namespace lift {

//===--------------------------------------------------------------------===//
// Stack analysis (private port of crates/analysis/stackptr.rs).
//===--------------------------------------------------------------------===//

/// Per-instruction rsp deltas plus frame-pointer info for one function.
struct StackInfo {
  /// instr VA -> bytes of stack between entry rsp and rsp at that point.
  std::map<Va, int64_t> Deltas;
  /// If `mov rbp,rsp` / `lea rbp,[rsp+X]` established a frame pointer:
  /// rbp == entry_rsp - rbp_delta at that point.
  std::optional<int64_t> RbpDelta;
  /// Deepest offset touched via rsp or rbp (frame size).
  int64_t FrameSize = 0;
  /// Highest positive offset touched (incoming stack args region).
  int64_t ArgsSize = 0;
  /// VA of the instruction that modifies rsp non-constantly
  /// (`sub rsp, rax`, `and rsp,-N`, non-const `__chkstk`), if any.
  std::optional<Va> DynVA;

  int64_t deltaAt(Va Addr) const {
    auto It = Deltas.find(Addr);
    return It == Deltas.end() ? 0 : It->second;
  }
  /// Frame offset of `[rsp + disp]` at `va` (locals are negative).
  int64_t rspOff(Va Addr, int64_t Disp) const {
    return Disp - deltaAt(Addr);
  }
  /// Frame offset of `[rbp + disp]` when an fp exists.
  std::optional<int64_t> rbpOff(int64_t Disp) const {
    return RbpDelta ? std::optional<int64_t>(Disp - *RbpDelta) : std::nullopt;
  }
  /// Does this instruction execute with a dynamic rsp?
  bool isDynamicAt(Va Addr) const {
    return DynVA && Addr.Value >= DynVA->Value;
  }
};

/// Walk the CFG computing rsp deltas; records frame bounds. Returns an
/// error string on a join whose predecessors disagree on the delta.
llvm::Expected<StackInfo>
analyzeStack(const Program &P, const FuncRecord &F, const Disasm &D,
             llvm::ArrayRef<Instr> Instrs);

//===--------------------------------------------------------------------===//
// MC operand decoding helpers
//===--------------------------------------------------------------------===//

/// Logical (iced-like) operand kinds.
enum class LogOpKind { Reg, Mem, Imm, Invalid };

/// One logical operand: which MCInst operand index starts it and its kind.
struct LogOp {
  LogOpKind Kind = LogOpKind::Invalid;
  /// Index into `MCInst` operands; for Mem, the first of the 5-operand
  /// group. For Reg/Imm, the operand itself.
  unsigned MI = 0;
};

/// Build the logical operand view for `MI` from its MCInstrDesc: outs then
/// untied ins; tied use-operands merge into their def; a memory group is one
/// logical operand. Extra MI operands beyond the desc become trailing
/// logical operands (variable_ops).
std::vector<LogOp> logicalOps(const llvm::MCInst &MI,
                              const llvm::MCInstrDesc &Desc);

/// The shared x86 MC context objects (built once via TargetRegistry).
struct X86MCTools {
  const llvm::Target *T = nullptr;
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
  std::unique_ptr<llvm::MCInstrInfo> MII;
  static const X86MCTools &get();
};

//===--------------------------------------------------------------------===//
// Lift state
//===--------------------------------------------------------------------===//

/// Which flag producer the last arithmetic/compare instruction was; used to
/// fuse a following same-block jcc into a direct icmp (Rust FlagProd).
struct FlagProd {
  enum Kind { Cmp, Test, Arith } K;
  /// Cmp/Test/Arith: the compared/produced operands.
  llvm::Value *A = nullptr;
  llvm::Value *B = nullptr;
  /// Arith: the produced result.
  llvm::Value *Res = nullptr;
  /// Arith mnemonic class for flag formulas: 'a'dd, 's'ub, 'l'ogic,
  /// 'i'nc, 'd'ec, 'n'eg.
  char Mn = 0;
};

/// The per-function lifter state (Rust `struct Lift`).
class LiftState {
public:
  llvm::Module &Mod;
  llvm::Function &Fn;
  llvm::IRBuilder<> B;
  const Program &P;
  const FuncRecord &F;
  const Disasm &D;
  const Signatures *Sigs;
  StackInfo Stack;
  llvm::ArrayRef<Instr> Instrs;
  const X86MCTools &MC;

  /// frame alloca + entry-rsp pointer inside it.
  llvm::Value *Frame = nullptr;
  llvm::Value *Sp0 = nullptr;
  /// root GPR -> i64 alloca.
  std::map<llvm::MCRegister, llvm::AllocaInst *> Gpr;
  /// xmm0-15 -> i128 alloca.
  std::map<unsigned, llvm::AllocaInst *> Xmm;
  /// flag name -> i1 alloca.
  std::map<llvm::StringRef, llvm::AllocaInst *> Flags;
  std::map<Va, llvm::BasicBlock *> Blocks;
  std::map<Va, llvm::Value *> Globals;
  /// Current-block flag producer for fusion.
  std::optional<FlagProd> Prod;
  /// Guard jcc VA -> the index SSA value its producer compared.
  std::optional<std::pair<Va, llvm::Value *>> SwOperand;
  std::vector<std::pair<Va, std::string>> Fallbacks;
  /// VA of the instruction being lowered (for rsp reads).
  Va CurVa;
  /// Dynamic-alloca pointer slot + delta recorded at the dynamic point.
  llvm::AllocaInst *DynSp = nullptr;
  int64_t DynDelta = 0;

  LiftState(llvm::Module &M, llvm::Function &Fn, const Program &P,
            const FuncRecord &F, const Disasm &D, const Signatures *Sigs,
            StackInfo S, llvm::ArrayRef<Instr> Instrs);

  // ---- types / constants ---------------------------------------------------
  llvm::IntegerType *iN(unsigned Bits) const {
    return llvm::IntegerType::get(Mod.getContext(), Bits);
  }
  llvm::IntegerType *i64() const { return iN(64); }
  llvm::Type *ptrTy() const {
    return llvm::PointerType::getUnqual(Mod.getContext());
  }
  llvm::ConstantInt *ci(uint64_t V, unsigned Bits = 64) const {
    return llvm::ConstantInt::get(iN(Bits), V);
  }
  llvm::Value *extTo(llvm::Value *V, unsigned Bits);
  llvm::Value *sextTo(llvm::Value *V, unsigned Bits);
  /// Entry-block alloca inserted at the top of entry.
  llvm::AllocaInst *entryAlloca(llvm::Type *Ty, llvm::StringRef Name);
  /// Tag the current VA on every new IRBuilder instruction.
  void setVA(Va Addr);
  /// Attach `!revenant.va !{i64 VA}` to anything still untagged (sweep).
  void tagProvenance();

  // ---- instruction operand view ---------------------------------------------
  std::vector<LogOp> opsOf(const Instr &I) const;
  LogOpKind opKind(const Instr &I, unsigned L) const;
  /// Register of logical operand `L` (MC register).
  llvm::MCRegister opReg(const Instr &I, unsigned L) const;
  /// Sign-extended immediate of logical operand `L`.
  int64_t opImm(const Instr &I, unsigned L) const;
  /// Memory operand decomposition at logical operand `L`.
  struct MemOp {
    llvm::MCRegister Base;
    llvm::MCRegister Index;
    int64_t Scale = 1;
    int64_t Disp = 0;
    llvm::MCRegister Seg;
  };
  MemOp opMem(const Instr &I, unsigned L) const;
  /// Natural operand width in bits (register size, memory access size or
  /// the immediate's declared width).
  unsigned opBits(const Instr &I, unsigned L) const;

  // ---- registers -------------------------------------------------------------
  /// Read register `R` at its natural width (i64 roots, i32/16/8 views,
  /// i128 for vectors). rsp yields the live frame pointer.
  llvm::Value *regRead(llvm::MCRegister R);
  /// Write `V` (natural width) to `R`; 32-bit writes zero-extend, narrower
  /// writes merge, rsp writes are dropped (the frame owns rsp).
  void regWrite(llvm::MCRegister R, llvm::Value *V);
  /// The xmm slot tracked for a vector register (ymm/zmm share xmm's low
  /// lane; xmm16+ untracked).
  static std::optional<unsigned> xmmSlot(llvm::MCRegister R);
  llvm::Value *flagRead(llvm::StringRef N);
  void flagWrite(llvm::StringRef N, llvm::Value *I1);

  // ---- operands ---------------------------------------------------------------
  /// Read logical operand `L` as an integer of `bits` width.
  llvm::Value *opReadInt(const Instr &I, unsigned L, unsigned Bits);
  /// Write `V` to logical operand `L`.
  void opWriteInt(const Instr &I, unsigned L, llvm::Value *V);

  // ---- frame / memory ----------------------------------------------------------
  llvm::Value *framePtrOff(int64_t Off);
  /// Effective address of memory operand `L` as a pointer.
  llvm::Value *memPtr(const Instr &I, unsigned L);
  llvm::Value *memRead(const Instr &I, unsigned L, unsigned Bits,
                       bool Float = false);
  void memWrite(const Instr &I, unsigned L, llvm::Value *V);
  /// Global pointer for an absolute image VA (RIP targets, IAT slots).
  llvm::Value *globalPtr(Va Addr);
  /// rsp -= size; [rsp] = V (frame store at delta).
  void pushVal(llvm::Value *V, const Instr &I);
  /// load [rsp]; rsp += size.
  llvm::Value *popVal(const Instr &I);
  /// Non-constant rsp adjustment -> LLVM dynamic alloca.
  void emitDynAlloca(const Instr &I);

  // ---- flags ---------------------------------------------------------------------
  void produce(FlagProd P);
  /// Materialize all arithmetic flags for res = op(a,b) of width bits.
  void materializeFlags(llvm::Value *Res, llvm::Value *A, llvm::Value *B,
                        char Mn);
  /// i1 predicate for X86::CondCode, fused against Prod when possible.
  llvm::Value *condFor(unsigned CC, Va JccVa);
  llvm::Value *condFromFlags(unsigned CC);
  std::optional<llvm::Value *> fuseCond(unsigned CC, const FlagProd &P);
  void materializeProd(const FlagProd &P);
  /// Evaluate a switch-index CmpOperand at block `at`.
  llvm::Value *evalCmpOperand(const CmpOperand &Loc, Va At);
  /// Flags are dead after any call.
  void clobberFlags();
  /// After a call, volatile regs hold arbitrary values.
  void clobberVolatile();

  // ---- calls ---------------------------------------------------------------------
  llvm::FunctionType *fnType(const Signature *S);
  llvm::Function *calleeFn(Va Target, const Signature *S);
  llvm::Function *calleeByName(llvm::StringRef Name, const Signature *S);
  std::vector<llvm::Value *> callArgs(const Signature *S,
                                    std::optional<size_t> Prefix);
  void coerceArgs(llvm::Function *F, std::vector<llvm::Value *> &Args);
  llvm::Value *readCallerArg(int64_t Off);
  void emitCall(const Instr &I);
  void emitTailCall(Va Target);
  void emitIndirectTailCall(const Instr &I);

  // ---- per-instruction lowering ---------------------------------------------------
  /// Lift one non-terminator-position instruction.
  llvm::Error liftInstr(const Instr &I);
  /// Hand-lifted core ops; returns false when the opcode is not covered.
  bool liftCore(const Instr &I);
  bool liftMov(const Instr &I);
  bool liftLea(const Instr &I);
  bool liftXchg(const Instr &I);
  bool liftAddSub(const Instr &I);
  bool liftUnary(const Instr &I);
  bool liftLogic(const Instr &I);
  bool liftCmp(const Instr &I);
  bool liftPush(const Instr &I);
  bool liftPop(const Instr &I);
  /// Opaque revenant.asm.<mnemonic> call preserving register/memory state.
  void liftOpaque(const Instr &I);

  // ---- helpers ----------------------------------------------------------------------
  llvm::Function *llvmIntrinsic(llvm::StringRef Name,
                                llvm::ArrayRef<llvm::Type *> Tys);
};

/// Lowercase mnemonic text for `I` ("mov", "punpckldq", ...) for
/// revenant.asm.<mnemonic> naming: the first token of the Intel listing,
/// or the TableGen name lowercased.
std::string mnemonicOf(const Instr &I, const Disasm &D);

/// Sanitize a symbol name for use as an LLVM global name (Rust `sanitize`).
std::string sanitizeName(llvm::StringRef Name);

/// True for the 15 tracked GPR roots (rax..r15, no rsp).
bool isTrackedGpr(llvm::MCRegister R);

/// The 64-bit root register for `R` (rax for eax/ax/al/ah, xmmN for ymmN,
/// or R itself); NoRegister when `R` has no tracked root.
llvm::MCRegister rootRegOf(llvm::MCRegister R, const llvm::MCRegisterInfo &MRI);

/// (bit offset within root, width in bits) of `R` inside `rootRegOf(R)`.
std::pair<unsigned, unsigned> regOffsetBits(llvm::MCRegister R,
                                            const llvm::MCRegisterInfo &MRI);

/// True for ah/ch/dh/bh (merge at bit 8).
bool isHigh8(llvm::MCRegister R);

} // namespace lift
} // namespace revenant

#endif // REVENANT_LIFT_LIFTSTATE_H
