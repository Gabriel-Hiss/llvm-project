//===- revenant/Transforms/Pipeline/Pipeline.h - IR pipeline host -----*- C++ -*-===//
//
// The decompiler pipeline: lift -> cleanup -> idioms -> unopt -> structure.
// Each transform lives in its own library RevenantTransform<Name> under
// revenant/lib/Transforms/<Name>/ (owned by other agents). This header only
// declares the pipeline-level API: pass registration and the default
// pipeline text. Per-pass headers live in
// revenant/include/revenant/Transforms/<Name>/.
//
// Every Revenant pass is a FunctionPassManager pass registered under
// `revenant-<name>` via registerPipelineParsingCallback (see Register.cpp),
// except `revenant-unabi`, which rewrites function signatures and runs as
// a ModulePassManager pass. revenant-opt links all RevenantTransform*
// libs statically, so `revenant-opt -passes=revenant-<name> in.ll` works
// with no plugin flags.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_PIPELINE_H
#define REVENANT_TRANSFORMS_PIPELINE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {
class Module;
class PassBuilder;
class TargetMachine;
} // namespace llvm

namespace revenant {

/// Normalize machine-state IR before deoptimization. Stack splitting happens
/// before promotion; address and width recovery repeats after promotion so
/// register stores no longer hide the pointer and integer expression trees.
/// Keep CFG cleanup conservative to preserve vector epilogues and loop tiers.
inline constexpr llvm::StringLiteral CleanupPipeline =
    "revenant-lift-normalize,sroa,mem2reg,"
    "revenant-lift-normalize,instcombine<no-verify-fixpoint>,"
    "simplifycfg<no-switch-to-lookup;no-hoist-common-insts;"
    "no-sink-common-insts;no-speculate-blocks>,"
    "early-cse,sroa,instcombine<no-verify-fixpoint>,"
    "gvn,dse,dce,sroa,instcombine<no-verify-fixpoint>,"
    "revenant-lift-normalize";

/// The unopt stage in pipeline order, ported from
/// `crates/unopt/src/lib.rs` (`passes()`). `revenant-devectorize` runs the
/// lane-0 scalarization first (exposing scalar low-lane accumulators), then
/// produces the canonical one-element-per-iteration form that `reroll`
/// compacts further; `unrotate`/`untailrec` normalize loop structure.

inline constexpr llvm::StringLiteral UnoptPipeline =
    "revenant-devectorize,revenant-unloopvec,revenant-unloopvecwidths,revenant-reroll,"
    "revenant-unrotate,revenant-untailrec";

/// The IR-level idiom stage, ported from `crates/idioms/src/lib.rs`
/// (`ir_passes()`). Currently just magic-div; AST-level idiom passes
/// (rust-panic-check, loop-rotation, mem-loop, minmax) run on the
/// structured AST after revenant-structure, not in this LLVM pipeline.
inline constexpr llvm::StringLiteral IdiomsPipeline = "revenant-idioms";

/// Default decompiler pipeline text (engine order: lift -> cleanup ->
/// idioms -> unopt -> structure). `withPatterns` inserts the opt-in
/// `revenant-patterns` stage between unopt and structure, matching the
/// engine's `--patterns` path (pattern annotation on the cleaned IR before
/// structuring). Lift itself is not an LLVM pass; the driver lifts first
/// and then runs this text with revenant-opt/opt.
LLVM_ABI std::string defaultPipeline(bool withPatterns = false);

/// Run the cleanup pipeline (`CleanupPipeline`) over
/// M in-process, exactly as the lifter does after lifting and before
/// deoptimization. Returns true on success, false + ErrMsg on parse/run
/// failure. TM may be null (cleanup passes do not need a TargetMachine).
LLVM_ABI bool runLiftCleanup(llvm::Module &M, llvm::TargetMachine *TM,
                             std::string &ErrMsg);

/// Register every Revenant `revenant-<name>` pass with PB's Function
/// pipeline parsing callbacks. Called once by revenant-opt startup and by
/// the plugin entry point. Pass agents MUST NOT register their own names
/// elsewhere; this function is the single registration site.
LLVM_ABI void registerRevenantPasses(llvm::PassBuilder &PB);

/// Add every Revenant pass in default-pipeline order to FPM. Used by
/// in-process callers (unit tests, the revenant driver) that build the
/// pipeline without parsing text.
LLVM_ABI void addDefaultPipeline(llvm::FunctionPassManager &FPM,
                                 bool withPatterns = false);

/// Add every Revenant pass in default-pipeline order to MPM, wrapped in a
/// function-pass adaptor. Equivalent to parsing defaultPipeline().
LLVM_ABI void addDefaultModulePipeline(llvm::ModulePassManager &MPM,
                                       bool withPatterns = false);

} // namespace revenant

#endif // REVENANT_TRANSFORMS_PIPELINE_H
