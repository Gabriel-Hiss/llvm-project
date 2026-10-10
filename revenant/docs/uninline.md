# Call recovery

`revenant-uninline` recovers a direct call only when the module still contains
its callee definition and the caller contains an equivalent instruction region.
The pass does not reconstruct deleted definitions or undo arbitrary instruction
combining. No failed match changes the module.

## Evidence-confirmed tiny bodies

Single-block callees below the 3-instruction structural minimum stay
eligible only with lift-recorded evidence (`revenant/Evidence/Evidence.h`):
the callee must carry non-empty `uninlineEvidence` (source Location,
crash-metadata strings, libdb name) and each candidate region must pass
`regionMatchesEvidence` against it (same string global referenced, or
same library body called). The structural match still runs fully first;
evidence only confirms tiny exact matches such as assert-string readers.
The single-call wrapper veto stays absolute: evidence never lifts it.
Evidence-empty modules keep the old reject, so `--meta=none` output is
unchanged apart from `revenant.lib` confirmations, which stay correct
because the match itself remains exact.

## Corpus measurement

The scripts under `test/Transforms/Uninline` consume the read-only parity
artifact indices. `measure.py` runs this slice's plugin followed by LLVM's
verifier on each available clang-cl O2, O3, LTO and rustc O3 post-optimization
translation unit. With `--remarks` it records one CSV row per compiler inline
record; without it, one row per missing pre-optimization direct call. It
writes a summary by compiler group and excludes deleted callers, recursive
calls, intrinsics, external declarations and noinline callees.

`capture_remarks.py` replays each target's compile-only `artifacts.cmd` recipe
with inline optimization records enabled, in an isolated scratch directory.
It never executes the recipe's move commands or links a binary. Rust uses the
artifact generator's edition, crate name, target, optimization and debug flags.
The capture index first compares the resulting IR against the existing
ir-post file, ignoring only the ModuleID comment. Clang optimization records
enable line-table debug info even for an otherwise debug-free build, so the
replay carries `!dbg` attachments the corpus ir-post never had. The
measurement reader (`remarks.py`) therefore compares both modules after
LLVM's `strip-debug` pass and accepts the records only when every body line
agrees up to metadata/attribute numbering; both normalized hashes are
retained in `verified-index.json`. Rejections stay out of the denominator
rather than treating a recompile as evidence about a different corpus module.

With verified records the denominator is exact: 36,249 compiler-reported
inline events across 39 translation units (15 clang TUs in the main index
plus the 16 re-captured library TUs whose recipes the corpus generator had
overwritten, plus rust-o3). Exact-match recovery: 105 full (all with the
callee definition retained), 0 partial, 36,144 none. The pass makes atomic,
proved rewrites, so partial never fires. Examples of recovered sites: miniz
`mz_compress2 -> mz_deflateInit`, tinyxml2 `XMLElement::Attribute ->
XMLAttribute::Value`, pugixml `eval_node_set -> xpath_variable::get_node_set`.
Unrecovered events are dominated by callees whose bodies the later optimizer
passes simplified beyond exact matching, or that no longer exist as
definitions in ir-post. Detailed rows and compiler records are produced
under the supplied output and scratch paths. They are machine-local
measurement artifacts, not test fixtures. There is no prebuilt rustc O2 IR
pair in this corpus. Rust O3 is the available optimized reference; do not
invent O2 results.

`reinline.py` replays each verified event faithfully to separate
specialization from unrecoverable shapes: it strips the record-mode debug
info from ir-pre, runs the same `default<On>` pipeline the corpus recipe
ran, and compares the optimized caller against the ir-post caller up to
value numbering, predecessor order and debug attachments. Full means the
round trip reproduces the corpus caller exactly. Results on the same
denominator: 182 full, 0 partial, 36,067 none, broken down as clang-cl O2
0/16,795, O3 91/9,689, LTO 91/9,689, rustc O3 0/76. Of the 36,067 non-full,
36,044 are callees deleted from ir-post (nothing to outline back to, per
the assignment skipped), 19 lose the caller under the replay (large
C++ TUs where the whole-module replay diverges), and only 4 reproduce
with a different specialization. The 164 round-trip-only sites beyond the
exact-match set are the specialization dividend: multi-block callees such
as miniz `mz_zip_reader_end_internal` (9 sites) and
`mz_zip_array_ensure_capacity` (8 sites) whose inlined bodies the later
passes rewrote past exact matching but whose outline-plus-reinline still
reproduces ir-post. The pass itself still recovers only the exact-match
subset; the round trip is measurement, not a rewrite rule.

`clones.py` hashes single-block windows (3..16 instructions, SROA
traffic abstracted) across each ir-post TU: json-clang-o2 alone has
104,742 classes, 42,936 repeated, 38,138 cross-function. `outline.py`
then matches each deleted callee's normalized ir-pre body against those
windows on the same 36,249-event denominator: 0 full, 0 partial, 36,249
none in every group (O2 0/16,795, O3 0/9,689, LTO 0/9,689, rustc 0/76).
The miss split is structural, not a threshold artifact: 11,403 sites
have fewer than 3 normalized instructions left (the callee was a thin
wrapper whose spills SROA removed, e.g. `vector::back` shrinks to 7
loads/geps and the no-spill filter keeps 2), 18,954 have no window
matching at all (fully specialized away: the memcpy in
`TDEFL_READ_UNALIGNED_WORD2` dissolves into byte loads/shifts, the
`back()` address math folds into surrounding GEPs), 2 have exactly one
occurrence (outlining needs a repeat), and 5,890 are retained callees
the exact path already handles. `outline_synthesize.py` performs the
CodeExtractor step on one TU (miniz O3: 16,082 classes, 4,171 repeated,
top class 92 occurrences) and emits new `internal` outlines, but the
emitted calls carry a void prototype with untyped parameters, so the
module fails verification today (`%p0 defined with type ptr but
expected i32`). Conclusion for the integrator: the deleted-callee mass
is small STL/template wrappers specialized past window recognition plus
single-caller functions (1,587 of 2,410 deleted callees in json have
exactly one caller); synthesizing them back does not reproduce the
original callee bodies, it invents new ones. No pass change follows
from this phase.

Example, from the assigned worktree:

```
python revenant/test/Transforms/Uninline/capture_remarks.py \
  --corpus C:/Users/User/RustroverProjects/Revenant/target/parity \
  --scratch meas/remarks \
  --rust-source C:/Users/User/RustroverProjects/Revenant/.worktrees/harness/corpus/src/rust/main.rs
python revenant/test/Transforms/Uninline/measure.py \
  --corpus C:/Users/User/RustroverProjects/Revenant/target/parity \
  --opt C:/Users/User/RustroverProjects/llvm-project/build-revenant/bin/opt.exe \
  --plugin build-rv-uninline/RevenantTransformUninline.dll \
  --output meas/before
```

## Exact multi-block regions

The CFG matcher binds blocks by successor position, then binds instruction
definitions before comparing operands. This permits loop-carried PHIs and
forward references. PHIs compare incoming block identity, not textual order.
All opcodes, types, constants and instruction flags must agree.

The entry may keep a caller prefix. A unique return block may keep a caller
suffix. Every other instruction and edge belongs to the matched region.
Internal blocks reject outside predecessors; the entry rejects backedges.
Only the returned definition may escape, and every reconstructed argument
must dominate the replacement call and come from outside the region.

The rewrite splits off the exit suffix, builds the call at the original entry,
redirects the entry to that suffix, replaces outside return-value uses, clears
cyclic references and deletes the internal blocks. LLVM's split operation
repairs successor PHIs. The new call and branch retain available virtual
addresses and debug locations. Opaque and structured-region annotations cause
rejection rather than losing their meaning. Allocation, EH, atomics, marker
intrinsics and ABI-changing attributes are outside the current proof.

Multiple callee returns must branch to one continuation in the caller. Each
continuation PHI must either merge the corresponding callee return values or
carry the same outside value on every matched return edge. The rewrite removes
those incoming edges and adds the entry edge with the call result or the proved
uniform value. Unrelated outside predecessors stay in place. A second varying
output, a side exit, or a non-dominating uniform value rejects the whole match.

Stores and direct calls are legal when all operations and their order match.
Volatile accesses, atomics, convergent calls, operand bundles and frame-sensitive
intrinsics are rejected. Input and return contracts must also remain valid:
noundef needs a value-tracking proof, other input restrictions need the same
caller argument contract, and return ranges need a constant-range proof.
Semantic instruction metadata must match, while addresses and debug locations
are carried to the replacement call.

Pass `--remarks meas/remarks/index.json` to `measure.py` for the confirmed
inline-event denominator. Every row then retains the compiler's source
location. Deleted callers and deleted callees remain in the denominator as
none. If repeated identical targets have only some calls restored, their
individual sites are unresolved rather than arbitrarily assigning successes
to source locations. These rows are reported separately.

The corpus fixtures use `mz_zip_get_mode` and `mz_zip_set_last_error` from
miniz-clang-o3. Additional controlled cases cover loop PHIs, reordered PHI
inputs, multiple returns, caller context, metadata, side edges and extra live
values. A multiblock callee needs actual branching or at least three ordinary
instructions. An outlined call plus an unconditional continuation is not new
evidence for another callee, which prevents replacement calls from turning
into chains of unrelated forwarding functions during the same module run.

Return range checking follows PHI and select arms, with pessimistic memoization
for cycles. It accepts ranges proved by constants or instruction metadata and
rejects an unproved return contract. `cfg-contracts.ll` checks that both input
contracts and load range metadata are present before rebuilding a call.
