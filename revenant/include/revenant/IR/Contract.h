//===- revenant/IR/Contract.h - Frontend IR contract helpers -----*- C++ -*-===//
//
// Helpers to write and read the "decompiler form" LLVM IR contract described
// in docs/ir-contract.md: VA provenance, region annotations, opaque asm
// calls, the language module flag, and debug-info attachment. Every producer
// (lifter, transforms, structuring, type recovery) and every frontend uses
// this API instead of touching the metadata directly.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_IR_CONTRACT_H
#define REVENANT_IR_CONTRACT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Compiler.h"
#include <cstdint>
#include <optional>

namespace llvm {
class BasicBlock;
class DIBuilder;
class DICompileUnit;
class DIExpression;
class DILocalVariable;
class DIScope;
class DISubprogram;
class DISubroutineType;
class DIType;
class DIDerivedType;
class DICompositeType;
class DIEnumerator;
class GlobalVariable;
class LLVMContext;
class MDNode;
} // namespace llvm

namespace revenant::ir {

/// Custom metadata kinds owned by the contract.
inline constexpr llvm::StringLiteral MDKindVA = "revenant.va";
inline constexpr llvm::StringLiteral MDKindRegion = "revenant.region";
inline constexpr llvm::StringLiteral MDKindOpaque = "revenant.opaque";

/// Module flag carrying the detected producer language.
inline constexpr llvm::StringLiteral ModuleFlagLang = "revenant.lang";

/// Function attribute carrying the entry VA, value "0x<hex>".
inline constexpr llvm::StringLiteral FnAttrVA = "revenant.va";

/// Prefix for opaque asm-instruction stubs.
inline constexpr llvm::StringLiteral OpaqueAsmPrefix = "revenant.asm.";

/// Sentinel for "no block index" in region descriptors (encoded as i32 -1).
inline constexpr int32_t NoBlockIndex = -1;

/// The detected producer language (value of the `revenant.lang` flag).
enum class Lang { C, CPP, Rust };

/// Region kinds for `!revenant.region` descriptors.
enum class RegionKind {
  If,      /// "if"      {i32 follow}
  SC,      /// "sc"      {i32 partner}    short-circuit folded condition
  While,   /// "while"   {i32 header, i32 latch, i32 exit}
  DoWhile, /// "dowhile" {i32 header, i32 latch, i32 exit}
  For,     /// "for"     {i32 header, i32 latch, i32 exit}
  Loop,    /// "loop"    {i32 header, i32 latch, i32 exit} unshaped natural loop
  Switch,  /// "switch"  {i32 follow}
  Goto,    /// "goto"    {i32 target}     irreducible edge
};

/// One parsed `!revenant.region` descriptor. Operand meaning per kind:
///   If:      A=follow
///   SC:      A=partner
///   While/DoWhile/For/Loop: A=header B=latch C=exit (C may be NoBlockIndex)
///   Switch:  A=follow
///   Goto:    A=target
struct RegionAnnotation {
  RegionKind Kind;
  int32_t A = NoBlockIndex;
  int32_t B = NoBlockIndex;
  int32_t C = NoBlockIndex;
};

/// Contract verification severity.
enum class ContractSeverity { Error, Warning };

/// Verifier diagnostic sink: Severity, human-readable message.
using ContractDiagFn =
    llvm::function_ref<void(ContractSeverity, llvm::StringRef)>;

//===----------------------------------------------------------------------===//
// Module-level
//===----------------------------------------------------------------------===//

/// Set (or replace) the `revenant.lang` module flag.
void setModuleLang(llvm::Module &M, Lang L);
/// Read the `revenant.lang` module flag; nullopt if absent, malformed, or
/// an unrecognized value.
std::optional<Lang> getModuleLang(const llvm::Module &M);
/// Text of a Lang value: "c", "cpp", "rust".
llvm::StringRef langName(Lang L);

//===----------------------------------------------------------------------===//
// Functions and blocks
//===----------------------------------------------------------------------===//

/// Set the `"revenant.va"` function attribute ("0x<hex VA>").
void setFunctionVA(llvm::Function &F, uint64_t VA);
/// Read the `"revenant.va"` function attribute.
std::optional<uint64_t> getFunctionVA(const llvm::Function &F);

/// Name a block `bb_<hex VA>` (canonical) or `bb_x<index>` when it has no
/// asm provenance.
void nameBlock(llvm::BasicBlock &BB, uint64_t VA);
void nameBlock(llvm::BasicBlock &BB); // inserted-block form: bb_x<index in fn>
/// Parse the VA encoded in a `bb_<hex>` name; nullopt for `bb_x*` or other
/// names.
std::optional<uint64_t> getBlockVA(const llvm::BasicBlock &BB);

//===----------------------------------------------------------------------===//
// Instruction provenance
//===----------------------------------------------------------------------===//

/// Attach `!revenant.va !{i64 VA}` to an instruction.
void setInstructionVA(llvm::Instruction &I, uint64_t VA);
/// Read the `!revenant.va` attachment.
std::optional<uint64_t> getInstructionVA(const llvm::Instruction &I);
/// Copy the VA attachment from Src onto Dst (use when replacing an
/// instruction so provenance survives transforms).
void copyInstructionVA(const llvm::Instruction &Src, llvm::Instruction &Dst);

/// Attach `!revenant.va !{i64 VA}` to a global variable.
void setGlobalVA(llvm::GlobalVariable &GV, uint64_t VA);
std::optional<uint64_t> getGlobalVA(const llvm::GlobalVariable &GV);

//===----------------------------------------------------------------------===//
// Region annotations
//===----------------------------------------------------------------------===//

/// Set the terminator's `!revenant.region` to a single descriptor.
/// Indices in operand order per kind (see RegionAnnotation). Pass
/// NoBlockIndex where the schema uses -1.
void setRegionAnnotation(llvm::Instruction &Term, RegionKind Kind,
                         llvm::ArrayRef<int32_t> Indices);
/// Append a descriptor, producing the list form
/// `!{!{...}, !{...}}` when needed.
void addRegionAnnotation(llvm::Instruction &Term, RegionKind Kind,
                         llvm::ArrayRef<int32_t> Indices);
/// First descriptor of the terminator's `!revenant.region`, if present and
/// well-formed.
std::optional<RegionAnnotation>
getRegionAnnotation(const llvm::Instruction &I);
/// All descriptors (empty if absent or malformed).
llvm::SmallVector<RegionAnnotation, 2>
getRegionAnnotations(const llvm::Instruction &I);
void clearRegionAnnotation(llvm::Instruction &I);
/// Rewrite every region descriptor in F through Map(index)->index (use
/// after basic-block renumbering/reordering).
void remapRegionIndices(llvm::Function &F,
                        llvm::function_ref<int32_t(int32_t)> Map);
/// True if Kind denotes a natural loop (While/DoWhile/For/Loop).
bool isLoopRegionKind(RegionKind K);
/// Spell a RegionKind for printing ("if", "dowhile", ...).
llvm::StringRef regionKindName(RegionKind K);

//===----------------------------------------------------------------------===//
// Opaque asm calls
//===----------------------------------------------------------------------===//

/// True if F names an opaque asm stub (`revenant.asm.<mnemonic>`).
bool isOpaqueAsmCallee(const llvm::Function &F);
/// True if I is a call-like instruction carrying `!revenant.opaque`.
bool isOpaqueAsmCall(const llvm::Instruction &I);
/// Get or create `declare <RetTy> @revenant.asm.<Mnemonic>(...)`. The
/// variadic declaration allows an opcode to carry differing operand counts.
llvm::Function *getOrCreateOpaqueAsmFn(llvm::Module &M,
                                       llvm::StringRef Mnemonic,
                                       llvm::Type *RetTy,
                                       llvm::ArrayRef<llvm::Type *> ArgTys);
/// Build `call` to the opaque stub for Mnemonic with the marker attached.
/// VA is optional provenance (attaches `!revenant.va` when given).
llvm::CallInst *makeOpaqueAsmCall(llvm::Module &M, llvm::StringRef Mnemonic,
                                  llvm::Type *RetTy,
                                  llvm::ArrayRef<llvm::Value *> Args,
                                  llvm::Instruction *InsertBefore,
                                  std::optional<uint64_t> VA = std::nullopt);

//===----------------------------------------------------------------------===//
// Debug info
//===----------------------------------------------------------------------===//

/// Return the module's single shared DICompileUnit, creating it (plus
/// `llvm.dbg.cu` and the "Debug Info Version" flag) on first use. The
/// DIFile is SourceFileName under directory "revenant". Language follows
/// `revenant.lang`.
llvm::DICompileUnit &getOrCreateCompileUnit(llvm::Module &M,
                                            llvm::DIBuilder &DIB,
                                            llvm::StringRef SourceFileName);

/// Create and `!dbg`-attach the `DISubprogram` for F under the shared CU.
llvm::DISubprogram &createSubprogramFor(llvm::Function &F,
                                        llvm::DIBuilder &DIB,
                                        llvm::DICompileUnit &CU);

/// Create a DILocalVariable in Scope. ArgNo > 0 marks parameter ArgNo.
llvm::DILocalVariable *createLocalVariable(llvm::DIBuilder &DIB,
                                           llvm::DIScope *Scope,
                                           llvm::StringRef Name,
                                           llvm::DIType *Ty,
                                           unsigned ArgNo = 0);

/// Emit `#dbg_declare` for Var describing Alloca, placed right after the
/// alloca. Expr defaults to `DIExpression::get(...,{})`.
void attachLocalVariable(llvm::AllocaInst &Alloca, llvm::DILocalVariable *Var,
                         llvm::DIBuilder &DIB,
                         llvm::DIExpression *Expr = nullptr);

/// All-in-one: attach a named/typed local to Alloca inside F's scope,
/// creating the subprogram if missing. Returns the variable.
llvm::DILocalVariable *declareLocal(llvm::Function &F, llvm::AllocaInst &Alloca,
                                    llvm::StringRef Name, llvm::DIType *Ty,
                                    llvm::DIBuilder &DIB,
                                    llvm::DICompileUnit &CU);

/// DI type constructors (thin, allocation-correct wrappers).
llvm::DIBasicType *createBasicType(llvm::DIBuilder &DIB, llvm::StringRef Name,
                                   uint64_t SizeBits, unsigned DWARFEncoding);
llvm::DIDerivedType *createPointerType(llvm::DIBuilder &DIB,
                                       llvm::DIType *Pointee);
/// Members are DIDerivedType(DW_TAG_member) values; pass
/// `DIB.createMemberType` results.
llvm::DICompositeType *createStructType(llvm::DIBuilder &DIB,
                                        llvm::DIScope *Scope,
                                        llvm::StringRef Name,
                                        uint64_t SizeBits,
                                        llvm::ArrayRef<llvm::Metadata *> Members,
                                        unsigned DWARFTag =
                                            llvm::dwarf::DW_TAG_structure_type);
llvm::DICompositeType *createEnumType(
    llvm::DIBuilder &DIB, llvm::DIScope *Scope, llvm::StringRef Name,
    uint64_t SizeBits, llvm::ArrayRef<llvm::Metadata *> Enumerators);
llvm::DIEnumerator *createEnumerator(llvm::DIBuilder &DIB,
                                     llvm::StringRef Name, int64_t Value,
                                     bool IsUnsigned = false);
llvm::DISubroutineType *
createSubroutineType(llvm::DIBuilder &DIB,
                     llvm::ArrayRef<llvm::Metadata *> Params);
/// Attach a DIGlobalVariable `!dbg` to GV under the shared CU.
void attachGlobalVariable(llvm::GlobalVariable &GV, llvm::DIBuilder &DIB,
                          llvm::DICompileUnit &CU, llvm::StringRef Name,
                          llvm::DIType *Ty);

//===----------------------------------------------------------------------===//
// Verification
//===----------------------------------------------------------------------===//

/// Run every contract rule on M (assumes the stock LLVM verifier already
/// passed; call verifyModule yourself if needed). Reports each violation to
/// Diag; returns the count of Error-severity diagnostics.
unsigned verifyModuleContract(const llvm::Module &M, ContractDiagFn Diag);

} // namespace revenant::ir

#endif // REVENANT_IR_CONTRACT_H
