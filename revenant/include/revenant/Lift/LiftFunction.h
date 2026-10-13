//===- LiftFunction.h - function-level asm -> LLVM IR lifter ----*- C++ -*-===//
//
// One recovered function -> one LLVM function in "decompiler form" (see
// docs/ir-contract.md): blocks named bb_<VA>, register file and stack frame
// as entry-block allocas that SROA/mem2reg later dissolve, calls typed from
// recovered signatures, imports as named declarations, data/string globals
// per the contract, and opaque revenant.asm.<mnemonic> calls wherever the
// instruction-semantics table has no rule yet.
//
// Mirrors crates/lift (lib.rs/block.rs/frame.rs/calls.rs/regs.rs/mem.rs/
// dispatch.rs): the model is the Rust lifter's — GP registers are i64
// allocas, xmm registers are i128 allocas, flags zf/sf/cf/of/pf are i1
// allocas, and rsp is virtual: stack accesses are constant-offset GEPs into
// one byte-array frame alloca sized from the rsp-delta analysis.
//
// Signature recovery (revenant_sigs) is not ported yet; `lift::Signatures`
// below mirrors it 1:1 so callers can already feed recovered signatures, and
// the sigs port can adopt or adapt this type.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_LIFT_FUNCTION_H
#define REVENANT_LIFT_FUNCTION_H

#include "revenant/Analysis/EHTables.h"
#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCRegister.h"
#include "llvm/Support/Error.h"

namespace llvm {
class BasicBlock;
class Function;
class Module;
class TargetMachine;
} // namespace llvm

namespace revenant {

class Disasm;

namespace lift {

/// Where a parameter arrives (Rust `ParamLoc`).
struct ParamLoc {
  /// One of rcx/rdx/r8/r9/xmm0-3 as an MC register number (X86::RCX, ...).
  llvm::MCRegister Reg;
  /// `[entry_rsp + off]`, off >= 0x28. Valid only when `IsStack`.
  int64_t StackOff = 0;
  bool IsStack = false;

  static ParamLoc reg(llvm::MCRegister R) { return {R, 0, false}; }
  static ParamLoc stack(int64_t Off) { return {{}, Off, true}; }
};

struct Param {
  ParamLoc Loc;
  /// Widest observed read: 8/16/32/64/128.
  uint32_t Bits = 64;
  /// Lives in an xmm register.
  bool Float = false;
  std::optional<std::string> Name;
};

struct RetLoc {
  bool Float = false;
  uint32_t Bits = 64;
};

enum class SigSource { Pdb, Inferred, Library, Default };

struct Signature {
  std::vector<Param> Params;
  std::optional<RetLoc> Ret;
  bool NoReturn = false;
  SigSource Source = SigSource::Default;
};

/// Function-start or IAT-slot keyed signatures (Rust `Signatures`).
struct Signatures {
  /// Function start VA -> signature.
  std::map<Va, Signature> Funcs;
  /// IAT slot VA -> signature for the imported symbol.
  std::map<Va, Signature> Externs;
  /// Call site VA -> argument prefix length (indirect calls only).
  std::map<Va, size_t> IndirectArgs;
  /// Call-site VA -> observed register-arg count (backstop for callees
  /// whose own signature came back empty).
  std::map<Va, size_t> CallsiteArgs;

  const Signature *of(Va Addr) const {
    auto It = Funcs.find(Addr);
    return It == Funcs.end() ? nullptr : &It->second;
  }
};

/// The default Win64 assumption for a callee with no signature: four
/// integer register parameters (rcx, rdx, r8, r9) and an i64 return.
Signature defaultCalleeSignature();

/// Result of lifting one function.
struct LiftedFunction {
  /// The lifted `define` inside the caller's module.
  llvm::Function *Fn = nullptr;
  /// asm block start VA -> LLVM basic block.
  std::map<Va, llvm::BasicBlock *> BlockMap;
  /// Instructions lowered through opaque `revenant.asm.*` calls:
  /// (instruction VA, "mnemonic (disasm text)").
  std::vector<std::pair<Va, std::string>> AsmFallbacks;
};

/// Lift `F` (a recovered `FuncRecord`) into module `M`.
///
/// `D` supplies decode + operand info for `F.InstrRvas`. `Sigs` may be null
/// (callers get the conservative default signature, void functions). `TM`
/// only fills module triple/datalayout when the caller did not already set
/// them. `RunPasses` runs the lift-cleanup pipeline (`liftCleanupPasses()`)
/// on the module when true. `EH` may point at the decoded EH entry for
/// this function (see revenant/Analysis/EHTables.h): when non-null, calls
/// inside its try ranges become `invoke`s with native LLVM EH pads.
///
/// Errors: stack-delta inconsistency (joins disagree on rsp depth), decode
/// failure inside a block, or a broken operand shape. The function is left
/// in the module only on success.
llvm::Expected<LiftedFunction>
liftFunction(llvm::Module &M, const Program &P, const FuncRecord &F,
             const Disasm &D, const Signatures *Sigs = nullptr,
             llvm::TargetMachine *TM = nullptr, bool RunPasses = true,
             const FunctionEH *EH = nullptr);

/// The cleanup pipeline string, mirroring crates/lift CLEANUP_PASSES. Runs
/// via PassBuilder::parsePassPipeline; `sroa,mem2reg` dissolve the register
/// file and frame, the simplifycfg options keep the CFG close to the asm.
llvm::StringRef liftCleanupPasses();

/// The name the lifter requests for a global at `Addr`, before LLVM's
/// module-level uniquing appends `.N`. Each lifted function creates its own
/// global per address, so the final names depend on lift order.
std::string globalNameFor(const Program &P, Va Addr);

/// The module name `liftFunction` gives the definition of `F`.
std::string definitionName(const FuncRecord &F);

/// What `liftFunction` does to `M` before lifting a body named `Name` with
/// signature `Sig`: reuse a declaration of the same type, or rename a
/// clashing declaration to `Name.decl` and declare `Name` anew. Returns the
/// function the body goes into, or null when `Name` is already defined.
llvm::Function *claimDefinition(llvm::Module &M, llvm::StringRef Name,
                                const Signature *Sig);

} // namespace lift
} // namespace revenant

#endif // REVENANT_LIFT_FUNCTION_H
