# Call recovery

`revenant-uninline` recovers a direct call only when the module still contains
its callee definition and the caller contains an equivalent instruction region.
The pass does not reconstruct deleted definitions or undo arbitrary instruction
combining. No failed match changes the module.

## Corpus measurement

The scripts under `test/Transforms/Uninline` consume the read-only parity
artifact indices. `measure.py` runs this slice's plugin followed by LLVM's
verifier on each available clang-cl O2, O3, LTO and rustc O3 post-optimization
translation unit. It records one CSV row per missing pre-optimization direct
call and writes a summary by compiler group. It excludes deleted callers,
recursive calls, intrinsics, external declarations and noinline callees.

A missing call is only a candidate inline event. Dead-code elimination and
constant folding can remove the same call without leaving an inlined region.
Repeated calls to the same target cannot be assigned to source sites from a
call-count difference. The CSV marks these ambiguities explicitly.

`capture_remarks.py` replays each target's compile-only `artifacts.cmd` recipe
with inline optimization records enabled, in an isolated scratch directory.
It never executes the recipe's move commands or links a binary. Rust uses the
artifact generator's edition, crate name, target, optimization and debug flags.
The capture index first compares the resulting IR against the existing
ir-post file, ignoring only the ModuleID comment. Clang optimization records
enable line-table debug info even for an otherwise debug-free build. The
measurement reader therefore also compares both modules after LLVM's
`strip-debug` pass. Exact equality of that normalized IR is required, and both
normalized hashes are retained in `verified-index.json`. Differences in code,
attributes or semantic metadata reject the records rather than treating a
recompile as evidence about a different corpus module.

The initial candidate classifier uses these definitions:

- Full means the original direct-call boundary was restored. It does not mean
  that allocas, debug records or other unrelated optimizations match ir-pre.
- Partial means the call boundary was only partly recovered. Because the pass
  makes atomic, proved rewrites, it never emits a partially restored boundary.
- None means no direct call was restored.

Detailed rows and compiler records are produced under the supplied output and
scratch paths. They are machine-local measurement artifacts, not test fixtures.
There is no prebuilt rustc O2 IR pair in this corpus. Rust O3 is the available
optimized reference; do not invent O2 results.

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
