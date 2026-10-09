# Revenant IR transforms

Pass agents run one pass as `revenant-opt -passes=revenant-<name> in.ll`,
and the whole decompiler pipeline as
`revenant-opt -revenant-pipeline in.ll -S -o out.ll`
(`-revenant-patterns` adds the opt-in patterns stage).
Stock LLVM passes mix freely with `revenant-*` ones in `-passes`.
Textual `.ll` is the interchange format; keep it parseable by LLVM 22
(rustc's CI LLVM is 23.1.3, forward compatible).

## Pipeline order

Engine order is lift -> cleanup -> idioms -> unopt -> structure
(`Session::decompile_with`; lift itself runs in the driver, not here).
`defaultPipeline(false)` in `Pipeline.h` is exactly:

```text
function(<cleanup>,revenant-idioms,<unopt>,revenant-structure)
```

`defaultPipeline(true)` inserts `revenant-patterns` between unopt and
structure, matching the engine `--patterns` path. Type recovery
(`revenant-types`) and SSA-out (`revenant-ssaout`) run post-structure,
pre-emission; they are not in the default text, pass them explicitly.

| stage | passes | source |
|---|---|---|
| cleanup | `sroa,mem2reg,early-cse,instcombine<no-verify-fixpoint>,simplifycfg<no-switch-to-lookup;no-hoist-common-insts;no-sink-common-insts;no-speculate-blocks>,gvn,dse,dce,sroa,instcombine<no-verify-fixpoint>` | `crates/lift/src/pipeline.rs` `CLEANUP_PASSES` |
| idioms (IR) | `revenant-idioms` | `crates/idioms` `ir_passes()` |
| unopt | `revenant-devectorize,revenant-reroll,revenant-unrotate,revenant-untailrec` | `crates/unopt` `passes()` |
| patterns (opt-in) | `revenant-patterns` | `crates/patterns`, engine `--patterns` |
| structure | `revenant-structure` | `crates/structure` |
| types (post) | `revenant-types` | type recovery, pre-emission |
| ssaout (post) | `revenant-ssaout` | mem2reg/sroa undo, pre-frontend |

## Registration convention

One library per transform, `RevenantTransform<Name>` under
`revenant/lib/Transforms/<Name>/`, header at
`revenant/include/revenant/Transforms/<Name>/<Name>.h`. The pass class is
`revenant::<Name>Pass : llvm::PassInfoMixin<...>`, a FunctionPassManager
pass with `run(Function&, FunctionAnalysisManager&)` and `isRequired()`.
Names are `revenant-<lowercase>`. The single registration site is
`revenant/lib/Transforms/Pipeline/Register.cpp`
(`registerRevenantPasses`); pass libs only define the class, guarded by
`__has_include` there so the tree builds before a pass lands. Keep the
standard `getPassPluginInfo` entry too, for `opt -load-pass-plugin`
interop; `revenant-opt` links everything statically and needs no plugin
flag.

## Passes

Expects/guarantees per pass. Every pass preserves `!revenant.va`
provenance on rewritten instructions and never drops `!revenant.opaque`;
contract shapes (`revenant/IR/Contract.h`, `docs/ir-contract.md`) hold
across all of them.

**cleanup (stock LLVM).** Expects raw lifted IR with `alloca` frames and
opaque `revenant.asm.*` calls. Guarantees promoted registers, dead-frame
removal, and a CFG still close to the asm: no switch-to-lookup tables, no
hoisted/sunk common instructions, no speculated blocks. The late
`sroa,instcombine` round removes never-read frames.

**revenant-idioms (owner: MuseTxIdioms).** Expects cleaned IR.
Folds the multiply-high + shift/add strength-reduction DAG back into
`udiv`/`sdiv`/`urem`/`srem` by a constant (Granlund-Montgomery), verified
on a witness set before rewriting; folds `x - (x/d)*d` spellings into
`urem`/`srem`. Guarantees no unverified rewrite: anything ambiguous stays
put. AST-level idioms (rust-panic-check, loop-rotation, mem-loop, minmax)
run after structuring, outside this LLVM pipeline.

**revenant-devectorize (owner: MuseTxDevec), phase 1: lane-0.** Expects
cleaned IR with `i128` xmm webs. Propagates scalar lane-0 values through
phis, transparent bitcasts and insert/extract pairs so scalar accumulators
kept in the low xmm lane scalarize. Guarantees a web scalarizes only when
every use reads lane 0 and all layouts agree; anything else keeps 128
bits. Runs first inside devectorize so the later phases see plain scalar
loops.

**revenant-devectorize, phases 2-3.** Expects lane-0 output.
Explodes auto-vectorized `i128`/`__v*` work into per-lane scalar ops:
vector streams become per-element scalars in loops, reduction accumulators
collapse to one scalar phi; straight-line slices scalarize for reroll.
Guarantees conservative bail-outs: ymm/zmm upper lanes, cross-lane ops,
unrecognized vector phis stay vector.

**revenant-reroll (owner: MuseTxReroll).** Expects scalarized loops.
Folds unrolled iteration copies back into one induction-variable step
(partial: keep one copy, divide step) and synthesizes loops for
fully-unrolled straight-line runs (full). Guarantees shape/offset matching
only; ambiguous copies stay put.

**revenant-unrotate (owner: MuseTxUnrotate).** Expects rerolled loops.
Rewrites `if (c) { do { body } while (c); }` to while shape when guard and
latch conditions are provably equal, splitting the self-loop block into a
test block plus body. Guarantees the rewrite only on proven-equal
conditions with safe `continue` behavior.

**revenant-untailrec (owner: MuseTxUnrotate).** Expects unrotated IR.
Undoes TailCallElim/loop-to-recursive-call: turns the recursive-call form
back into the loop it came from (toward IR 1:1 with the compiler's ir-pre).
Self-loop headers get a dedicated latch split out first, then the same
rewrite. Guarantees vetoes on any existing self-call, impure latch, or
non-covering phis; anything vetoed stays as-is.

**revenant-patterns (owner: MuseTxPatterns, opt-in).** Expects unopt
output. Matches oracle-generated library-idiom patterns (anchor-seeded
subgraph search) and records matches; contiguous single-block regions
above threshold become `__pat_*` helper calls. Catalog path and min-conf
are pass-owned `cl::opt` flags (`-revenant-patterns-catalog`,
`-revenant-patterns-min-conf=0.9`); because plugin options parse before
the plugin loads under stock `opt`, the pass also reads
`REVENANT_PATTERNS_CATALOG` / `REVENANT_PATTERNS_MIN_CONF` env vars.
Guarantees tolerance of commuted operands and extra code, and no change
when the pattern set is missing or empty or confidence is low.

**revenant-structure (owner: Swe2Structure).** Expects unopt (or patterns)
output. Regions become `if`/`while`/`dowhile`/`for`/`switch` with
`!revenant.region` on header terminators (block-list indices, `-1` none);
irreducible edges get `!revenant.region !{!"goto", target}`. Guarantees a
frontend ignoring annotations still emits correct code via labels + `goto`.

**revenant-types (owner: MuseTypeRecovery, post-structure).** Expects
structured IR with region annotations. Recovers locals/parameters and
struct/class/vtable/enum shapes, expressed as DI (`DILocalVariable`,
`DIBasicType`, `DIDerivedType`, `DICompositeType`) via RevenantIR helpers.
Guarantees region annotations and provenance survive; types only refine.

**revenant-ssaout (owner: MuseSsaOut, post-structure).** Expects typed
structured IR. Undoes mem2reg/sroa into named `alloca`s with DI for the
frontends. Guarantees stable `bb_<va>` names and intact annotations.

**revenant-unabi (owner: MuseDeoptAbi).** Module pass (not function): rewrites
function signatures, snapshotting the function list first so erasure is safe.
Runs explicitly via `-passes=revenant-unabi`, not in the default pipeline.
Guarantees `!revenant.va` preserved, `!revenant.opaque` never dropped, LLVM
22 parseable output.

**revenant-unloopvec (owner: MuseDeoptVecLoops).** Expects devectorize output
(or raw vectorizer skeletons). Collapses vector body + scalar remainder +
runtime checks back to one scalar loop. Preserves `!revenant.va`; bails
leaving the CFG untouched when unsure. Runs explicitly, not in the default
pipeline.

**revenant-unipo (owner: MuseDeoptIPO).** Module pass (not function):
rewrites function signatures, snapshotting the function list first so
erasure is safe. Undoes DeadArgElim return elimination (void back to the
dead computed value, e.g. errfile/pushmode/luaK_codek/g_iofile,
TIXML_SNPRINTF, xml_buffered_writer::flush, xpath_parser::error_oom),
dead-argument removal named by DISubprogram/dbg.value records (e.g.
rust-o3 dot4/vec_ops), and `.constprop.N`/`.isra.N`/`.specialized.N`/
`.lto_priv`/`.llvm.N` clone fold-back. Runs explicitly via
`-passes=revenant-unipo`, not in the default pipeline. Guarantees
`!revenant.va` preserved, `!revenant.opaque` never dropped, LLVM 22
parseable output.
