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
The capture index checks the resulting IR against the existing ir-post file,
ignoring only the ModuleID comment. Records from a different IR must not be
presented as evidence of what happened to the original corpus module.

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
