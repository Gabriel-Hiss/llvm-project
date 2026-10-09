# revenant-patterns: inline library-idiom recognition

Port of `crates/patterns` (matcher, canonicalization, application, name
normalization). Recognizes inlined library routines (memcpy/memset/strlen
loops, Rust core/alloc helpers, C++ STL helpers) by IR shape and replaces
them with calls to the named routine.

Run one pass as:

```text
revenant-opt -passes=revenant-patterns in.ll -S -o out.ll
```

or with an oracle catalog (see below). The stage is opt-in: it is not in
`defaultPipeline(false)`; `defaultPipeline(true)` inserts it between unopt
and structure, matching the engine `--patterns` path. Empty or missing
catalog still applies the hand-built libc anchors; a function with no
match is left untouched.

## Catalog

The Rust crate compiles its snippet catalog with rustc/clang at
generation time. Here generation-time compiles are out of scope, so the
catalog comes from prebuilt oracle IR, cleaned with the shared cleanup
pipeline:

```text
opt -passes="<cleanup>" -S pat_rust_O3.ll -o pat_rust_O3_clean.ll
```

Current prebuilt inputs (read-only, owned by the lead):

- `target/pattern-snippets/pat_{rust_O0,rust_O3,c_O0,c_O2,cpp_O0,cpp_O2}.ll`
- Cleaned copies used by the lit test live in
  `revenant/test/Transforms/Patterns/Inputs/`.

Point the pass at a directory of cleaned oracle modules (or one `.ll`
file) with `-revenant-patterns-catalog=<path>` (static revenant-opt
link; the flag is owned by this lib, parse-once, never required) or the
`REVENANT_PATTERNS_CATALOG` environment variable (plugin-DLL path; stock
opt parses its options before the plugin loads, so plugin `cl::opt`
flags never reach it). Each oracle function whose name contains the
snippet filter (`pat_vec_push`, `pat_strlen`, ...) becomes one pattern
per snippet, with `@O0`/`@O2`/`@O3` name suffixes per opt level, exactly
like the Rust `emit_pattern` origin suffix.

`-revenant-patterns-min-conf=<f>` (default 0.9,
`REVENANT_PATTERNS_MIN_CONF` for the plugin path) is the minimum match
coverage for a `__pat_*` rewrite. Matches below it, multi-block matches,
and regions straddling unmatched instructions are annotation only (no IR
change; a wrong rewrite is worse than none).

## Shapes

Canonicalization mirrors `canon.rs`: SSA values become slots (params
first), `0`/`1`/`-1` stay literal, other repeated integers become
consistent wildcard slots, globals become `Glob`, `undef`/`poison` match
anything, pointer/64-bit/vector types erase to "any" (only sub-64 ints
keep their width), callee names become normalized needles plus the
snippet's `alt_needles` (other opt-level spellings: `grow_one`,
`panic_bounds_check`, `emplace_reallocate` ...). `llvm.*` calls outside
the kept anchor set (`llvm.mem`, `llvm.vector.reduce`, overflow
intrinsics ...) are erased before canonicalization, and unnamed oracle
blocks get stable synthetic names, both mirroring `generate.rs`.

Matching mirrors `matcher.rs`: anchor-seeded search over required
instructions (calls, plus the guard compare for `CallsAndGuard`
patterns), flooding along def-use edges and CFG adjacency, tolerant of
commuted operands, swapped icmp predicates, erased types, consistent
wildcard constants, extra call arguments and extra target code. Budgets
match the reference (6000 steps, 12 branches, 16 occurrences).

Name normalization mirrors `norm.rs` using LLVM's own demanglers
(`rustDemangle`, `itaniumDemangle`, `microsoftDemangle`): flatten to
lowercase tokens, drop hash tokens, then containment plus content-token
matching, so `grow_one` survives both rustc v0 and resolver spellings.

## Rewrites

`rewriteMatch` mirrors `apply.rs`: highest confidence first, claimed
regions never overlap, only single-block contiguous regions rewrite, at
most one live-out (which becomes the call destination via RAUW, typed
like its original definition). The helper `__pat_<helper>` takes the
matched anchor call's operands (dropping the `llvm.memcpy`/`memset`
trailing `isVolatile` flag), deduplicated, in order. Every replacement
inherits the replaced range's `!revenant.va` provenance and debug
location (manual copy until `revenant::ir::copyInstructionVA` lands);
`!revenant.opaque` is never dropped. Output stays parseable by LLVM 22.

One rewrite per function per run (indices shift after an edit; the
driver re-runs to a fixed point), same observable result as the Rust
plan-then-rebuild split.
