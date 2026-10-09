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
//    name the same (index, len) pair. Lifted spellings: the panic path is
//    a dedicated `call + unreachable` block whose callee the sigs either
//    recover (`core::panicking::panic_bounds_check`,
//    `core::slice__index__slice_index_fail`, `core__str__slice_error_fail`)
//    or leave as a `sub_<VA>` declaration; the compare keeps whatever
//    predicate the binary spells (`ult`, flipped `ugt`, `samesign ugt`,
//    or `ule` with the panic edge on the true side, which also swaps the
//    branch successors).
// 2. Overflow checks: rustc lowers `a + b` to
//    `llvm.sadd.with.overflow` plus an expect/unreachable or a branch to
//    `core::panicking::panic`. The optimizer folds the flag when value
//    ranges rule out overflow (corpus: rust-o3 enum_area pre keeps two
//    smul.with.overflow sites; post keeps plain `mul`). The pass restores
//    the with.overflow + panic-branch spelling from the surviving mul
//    when the function still panics on overflow elsewhere. Lifted
//    spellings: the checked sites that survive release optimization
//    (RawVec capacity, `i64::from_str`) keep their intrinsic call and
//    branch untouched, and release builds that dropped the checks leave
//    plain ops with no panic edge, which nothing may re-invent.
// 3. Iterator loops: rustc lowers `for x in it` to a Range/slice-iter
//    struct loop with dbg.spill allocas, `ult` latch compares, and
//    sadd.with.overflow steps (corpus: rust-o3 loops/copy_slice pre).
//    The optimizer unrolls and scalarizes the loop into straight-line
//    adds (post loops) or folds it away (post iter_ops -> ret 120).
//    The pass re-rolls straight-line iteration runs into one loop with
//    an explicit phi step (defers to revenant-reroll for the exact
//    induction shape; this pass only rebuilds the Range-struct frame
//    the pre shape names). Lifted spelling: every local lives in one big
//    frame alloca, so the iter struct is already inside `%frame` and no
//    dead frame is added; a frameless function only fires when the `ult`
//    compare tests the induction phi with a constant step, which keeps
//    `n <= 1` base cases and magic-division compares out.
// 4. Option/Result niche ops: rustc packs `Option<i64>`/`Result` into
//    niche layouts (`{i64, i64}` pairs, `-1` sentinels, `trunc to i1`
//    tests; corpus: rust-o3 option_result pre). The optimizer folds the
//    whole discriminant test away when the value is known (post
//    option_result stores constants only). The pass restores the
//    discriminant load + compare spelling from the surviving stores
//    when the niche pair is still visible. Lifted spelling: the corpus
//    mangled name is gone (`sub_<VA>`), so the shape gate finds the
//    niche ladder instead: a -1/0 sentinel at slot 0 of an aggregate
//    that takes exactly one payload store at another offset. Struct
//    initializers that zero many fields and fmt write cursors that store
//    a count repeatedly break the shape and stay untouched.
// 5. Vec/String raw parts: rustc lowers `Vec<T>` to `{ptr, cap, len}`
//    triples with RawVec grow paths (corpus: rust-o3 vec_ops pre has
//    the grow_one invoke; post keeps the same invoke with sroa phis).
//    The optimizer SROAs the triple into scalar phis. The pass rebuilds
//    the triple alloca frame so the shape names (ptr, len, cap) again.
//    Lifted spelling: the triple sits inside the big frame alloca (no
//    rebuild), and the raw buffer pointer must walk through a phi, which
//    keeps argument-indexing functions like `Mutex<Vec<u8>>::lock` out.
// 6. Drop glue: at -O1 and above rustc inlines the `drop_glue::<Vec/
//    String>` shims into their callers; the undo outlines the guarded
//    region back into a `drop_glue.unrust` internal function (pure
//    outline: same test, same free, same order). Lifted spelling: the
//    dealloc callee is the Windows heap import the std allocator uses
//    (`HeapFree`/`HeapReAlloc`/`process_heap_alloc`) or an unnamed
//    `sub_<VA>` declaration; the outline only fires when every operand
//    the free arm reads is remappable into the new function, and bails
//    to the predicate fallback otherwise (the EH drop-glue switch reads
//    pointers loaded in other blocks).
//
// Measured on lifted normalized ir-clean (corpus rust-o2, rust-o3,
// minecalc-o3 binaries, `revenant-decompile --emit ir-clean`, verify
// clean; fires per function, corpus attribution through !rev.source and
// the parity source-map):
//   rust-o2  : 21 functions: bounds 19 flips + 1 samesign, niche 3,
//              iter frames 6, overflow rebuild 1, vec 0, drop 0
//   rust-o3  : same 21 (bounds 19, niche 3, iter 6, overflow 1)
//   minecalc-o3: 31 functions: bounds 36 flips + 9 samesign, niche 3,
//              iter frames 6, overflow rebuild 1, vec 0, drop 0
// Intact sites the pass deliberately leaves alone: 62-90 bounds-panic
// call sites and 4-5 `with.overflow` sites per binary (the checks
// survived release optimization; only their predicates get
// canonicalized). Corpus constructs map to: option_result -> 1 niche
// fire on the `Result<i64, String>` Err constructor; vec_index /
// slice_bounds -> already canonical `ult`; vec_ops / iter_ops / loops ->
// vectorized or inlined into main at -O3 (std range loops absorb the 6
// frame rebuilds); format_ops drop glue -> not outlinable in the lifted
// spelling. On compiler ir-post the pass keeps its previous behavior
// (1 flip, 1 niche, 1 samesign per level; the 3 old vec frames were
// struct-less SROA'd functions and fmt write shims, now excluded by the
// phi-walk requirement).
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
