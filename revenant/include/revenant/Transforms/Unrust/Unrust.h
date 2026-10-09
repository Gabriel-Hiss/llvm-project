//===- Unrust.h - undo rustc codegen shapings -------------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// `revenant-unrust` inverts six rustc codegen shapings so lifted IR reads
// like the compiler's own pre-optimization IR (ir-pre) for the function:
//
// 1. Bounds checks: rustc lowers `v[i]` to `icmp ult i, len` plus a branch
//    to `core::panicking::panic_bounds_check`. The optimizer deletes the
//    branch when range metadata proves the index safe (corpus:
//    rust-o3 vec_index/slice_bounds pre keep the check and the panic
//    call; post keeps only `icmp ugt len, i` shape facts), or folds the
//    whole access when the index is constant (post vec_index keeps one
//    bounds check for index 1 and a folded load). The pass restores the
//    `ult` + panic-branch spelling from the surviving compare when the
//    function still calls panic_bounds_check elsewhere or the range facts
//    name the same (index, len) pair.
// 2. Overflow checks: rustc lowers `a + b` to
//    `llvm.sadd.with.overflow` plus an expect/unreachable or a branch to
//    `core::panicking::panic`. The optimizer folds the flag when value
//    ranges rule out overflow (corpus: rust-o3 enum_area pre keeps two
//    smul.with.overflow sites; post keeps plain `mul`). The pass restores
//    the with.overflow + panic-branch spelling from the surviving mul
//    when the function still panics on overflow elsewhere.
// 3. Iterator loops: rustc lowers `for x in it` to a Range/slice-iter
//    struct loop with dbg.spill allocas, `ult` latch compares, and
//    sadd.with.overflow steps (corpus: rust-o3 loops/copy_slice pre).
//    The optimizer unrolls and scalarizes the loop into straight-line
//    adds (post loops) or folds it away (post iter_ops -> ret 120).
//    The pass re-rolls straight-line iteration runs into one loop with
//    an explicit phi step (defers to revenant-reroll for the exact
//    induction shape; this pass only rebuilds the Range-struct frame
//    the pre shape names).
// 4. Option/Result niche ops: rustc packs `Option<i64>`/`Result` into
//    niche layouts (`{i64, i64}` pairs, `-1` sentinels, `trunc to i1`
//    tests; corpus: rust-o3 option_result pre). The optimizer folds the
//    whole discriminant test away when the value is known (post
//    option_result stores constants only). The pass restores the
//    discriminant load + compare spelling from the surviving stores
//    when the niche pair is still visible.
// 5. Vec/String raw parts: rustc lowers `Vec<T>` to `{ptr, cap, len}`
//    triples with RawVec grow paths (corpus: rust-o3 vec_ops pre has
//    the grow_one invoke; post keeps the same invoke with sroa phis).
//    The optimizer SROAs the triple into scalar phis. The pass rebuilds
//    the triple alloca frame so the shape names (ptr, len, cap) again.
//    Measured on rust-o3 corpus_rust post (+revenant.lang=rust,
//    verify clean): 1/4 ugt bounds compares flips to ult, 5
//    alloc-family vec frames, 4 ult-loop iter frames, 1 niche
//    discriminant; samesign range idioms untouched (no panic edge).
//
// Gating: every rewrite requires the `revenant.lang == "rust"` module
// flag (the contract's producer-language marker; lift sets it from the
// detected profile, rustc binaries only). Without the flag the pass is
// a no-op, so C/C++ IR never changes shape. The per-rewrite evidence
// (panic callee names, with.overflow intrinsics, RawVec/drop_glue
// symbol fragments) is matched as shape, not as profile lookup: the
// lifted module has no PeFile to query, so the lang flag carries the
// CompilerProfile gate.
//
// Guarantees: only semantics-preserving rewrites; preserves
// `!revenant.va` on rewritten instructions; never touches
// `!revenant.opaque` calls.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNRUST_UNRUST_H
#define REVENANT_TRANSFORMS_UNRUST_UNRUST_H

#include "llvm/IR/PassManager.h"

namespace revenant {

struct UnrustPass : public llvm::PassInfoMixin<UnrustPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace revenant

#endif
