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
(`revenant-types`) and SSA-out (`revenant-ssaout`) run post-structure in
`ir-final`. The fidelity-only `ir-pre-like` stage uses
`revenant-locals-to-slots` after type recovery instead of SSA-out.

The driver's `ir-clean` stage (`Session::decompileAll`, `cleanPipeline` in
`lib/CAPI/Stages.cpp`) runs `revenant-idioms` twice: once right after
`<cleanup>` and once as the last pass of the trailing
`function(instcombine,early-cse,revenant-lift-normalize)` group. The early
run rewrites the multiply-high DAGs while the compiler's shape survives;
the late run undoes what that trailing instcombine round refolded
(`llvm.scmp`, `llvm.smin`/`smax`/`umin`/`umax`, `llvm.abs`,
`llvm.uadd.sat`/`usub.sat`) back into the source-level compare/select form,
so `ir-final` carries the compiler's pre-optimization shape.

| stage | passes | source |
|---|---|---|
| cleanup | `sroa,mem2reg,early-cse,instcombine<no-verify-fixpoint>,simplifycfg<no-switch-to-lookup;no-hoist-common-insts;no-sink-common-insts;no-speculate-blocks>,gvn,dse,dce,sroa,instcombine<no-verify-fixpoint>` | `crates/lift/src/pipeline.rs` `CLEANUP_PASSES` |
| idioms (IR) | `revenant-idioms` | `crates/idioms` `ir_passes()` |
| unopt | `revenant-devectorize,revenant-reroll,revenant-unrotate,revenant-untailrec` | `crates/unopt` `passes()` |
| patterns (opt-in) | `revenant-patterns` | `crates/patterns`, engine `--patterns` |
| structure | `revenant-structure` | `crates/structure` |
| types (post) | `revenant-types` | type recovery, pre-emission |
| pre-like locals | `revenant-locals-to-slots` | argument spills, one slot per PHI web, return slots, after `revenant-types` |
| ssaout (post) | `revenant-ssaout` | mem2reg/sroa undo, pre-frontend |

When SSA-out creates a function's first `DISubprogram`, it also gives any
call without `!dbg` a `DILocation`. The line uses the low 32 bits of
`!revenant.va`, or 0 when the instruction has no VA metadata.

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
Folds verified multiply-high and correction DAGs back into constant
`udiv`/`sdiv`, then folds quotient-times-divisor residuals into
`urem`/`srem`. It checks every input for i8/i16 and quotient boundaries plus
integer extrema for i32/i64. It converts llvm.abs/min/max, three-way compare,
and unsigned saturation intrinsics back into compare/subtract/select IR.
Signed three-way compare flag chains become source-level comparisons/selects.
Source-level Kernighan popcount, rotate and byte-swap operations remain
arithmetic/loop IR; source builtin intrinsics such as llvm.bswap pass through.
Bit tests retain direct `and`/`icmp`. Ambiguous arithmetic remains intact.
AST-level idioms (rust-panic-check, loop-rotation, mem-loop, minmax) run after
structuring, outside this LLVM pipeline.

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
structured IR with region annotations. Recovers locals, fixed-size array
extents, parameter and return types, plus struct/class/vtable/enum shapes,
expressed as DI (`DILocalVariable`, `DISubroutineType`, `DIBasicType`,
`DIDerivedType`, `DICompositeType`) via RevenantIR helpers. Field types come
from the widest access at each constant offset. Guarantees region annotations
and provenance survive; types only refine.

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

**revenant-unipo.** Module pass, invoked with `-passes=revenant-unipo`. Structural matching folds only equal-signature bodies, and requires matching operands except for equivalent constant data and trap-only stubs. It does not infer a missing formal from constants found near call sites. Identical bodies or one constant-specialized body are not proof of shared source identity. Function folds and signature restoration require source-signature evidence; uncertain functions stay unchanged. The pass preserves `!revenant.va` and `!revenant.opaque` and is not in the default pipeline.

**revenant-locals-to-slots (owner: LunaLocalsToSlots).** Expects structured,
type-recovered SSA. Restores the `-O0` storage shape: every parameter gets an
entry-block `alloca` spilled in signature order, every PHI web coalesces into
one slot (each incoming value stores at its definition or on the split critical
edge, each use loads right before it), and a multi-return function stores and
reloads through a shared `retval` slot. Slot names come from the web's register
name (`v_<name>`); `retval` is reserved for the return slot. Guarantees one slot
per non-interfering web, no PHIs left behind, `!revenant.va` copied onto every
store/load/alloca from the value it replaces. A web whose members interfere
(one member has a use besides the destination PHI) keeps separate slots rather
than merging. Runs after `revenant-types` in the fidelity `ir-pre-like` stage,
replacing `revenant-ssaout`.

Rule selection was measured, not assumed. On all 64 `c-clang-o2` one-to-one
chunks (per-chunk opcode-multiset Jaccard, the harness headline metric) the
"every PHI web" rule scores 0.2127 and leaves 318 allocas / 0 PHIs against
ir-pre's 301 / 3. The "DI-backed + loop + retval" rule scores 0.2061 with
214 allocas / 125 PHIs, and adding cross-block temporary spills scores 0.2132
with 613 allocas / 125 PHIs. The three Jaccards sit within 0.007, so the
structural counts decide: clang `-O0` is pre-mem2reg, so ir-pre has no PHIs,
and only the all-PHI-web rule reproduces that (0 residual PHIs, allocas 6%
over instead of 29% under or 104% over). All three beat the `revenant-ssaout`
baseline (0.1787, 58.9 average instruction delta); the winner sits at 39.0.

## Robustness

Reroll's affine model accepts signed 64-bit constants only. Lifted i128
SIMD masks outside that range veto the candidate rather than narrowing
through `APInt::getSExtValue`. Constant differences use 65-bit arithmetic
and must fit signed 64 bits before the model consumes them. The minimum
signed stride also vetoes the candidate because its magnitude cannot fit.

Untailrec moves required header definitions into the recursive call block
in their original instruction order. Shared dependencies stay before
every user. It does not match split-latch clones by instruction name:
unnamed numeric SSA values have empty names, so names cannot identify them.

The engine verifies every function after each function pass. If a pass
produces invalid IR, it restores the previous body, reports the function,
stage and pass, and continues with the next pass. A returned lift error
also reports the failed function and continues with the remaining functions.
Assertions still require fixing their offending pass or lifter operation.

**revenant-unoutline (owner: Swe2DeoptOutliner).** Module pass that restores
small shared binary-derived fragments and direct-call thunks at their call
sites. It requires multiple direct callers and a single straight-line block;
uncertain functions remain calls. Runs explicitly with
`-passes=revenant-unoutline`.

**revenant-unipo, structural recovery.** Structural clone recovery does not
inspect function names or oracle symbols. It specializes a candidate general
definition at one constant parameter, runs InstCombine, SimplifyCFG and SCCP
on temporary copies, then compares normalized bodies with constants
abstracted. Hash matches require exact instruction, operand, CFG and
semantic-metadata equality modulo local names before merging. The specialized
function may retain its parameter or omit one parameter, including a middle
parameter. Direct callers then call the general definition with the proven
constant, and the specialized definition is deleted.

Return restoration requires scalar return metadata whose ABI width and category
match the dead value in the optimized body. Dead values alone cannot distinguish
a source return from sret state or an unrelated computed value. Dead formals
require an explicit poison `dbg.value` record; `DISubprogram` arity alone can be
stale. Named-clone recovery requires a clone suffix. Folded constant-global
recovery is limited to one global passed only as the first call argument, the
GCC constprop `this` shape.

New calls preserve instruction metadata and VA provenance. Structural matching
rejects opaque instructions, region annotations, and outlined `.part.N` or
`.cold` bodies rather than rewriting those paths.


Measured with the standalone plugin and `revenant-unipo,verify` on fresh
normalized GCC lifts:

| Target | Oracle clone entries | Recovered / 9 | Named clone entries merged/restored | Cold partitions |
| --- | ---: | ---: | --- | --- |
| cpp-gcc-o2 | 9 | 3 | pool free and allocate signatures restored; `_S_construct.isra` merged | both unchanged |
| cpp-gcc-o3 | 9 | 3 | pool free and allocate signatures restored; `_S_construct.isra` merged | both unchanged |

The pool constprop clones regain their folded pool-object formal by rebinding
the equivalent internal constant global. Their remaining arguments stay
site-specific, so callers retain distinct allocation sizes rather than
requiring a unanimous constant. The `_S_construct.isra` pair folds only when
the body match proves equivalent initialized string data and equivalent
trap-only `sub_0x` callees; pooled data is treated as equal only when constant
initializers match and its address is not compared. Four named entries remain:
`_M_construct.isra` at VA `0x14001bca0` (O2) / `0x14001bf40` (O3), whose body
CFG/opcode shapes differ from unspecialized `_M_construct` at `0x14001bbf0` /
`0x14001be90`; `d_demangle_callback.constprop` at `0x14000aa20` / `0x14000ac30`
(no equivalent general body survived; its two callers pass different callback
values); `next_is_type_qual.isra` at `0x140002430` / `0x140002640` (no matching
general body/call evidence); and `is_designated_init.part.0.isra` at
`0x1400023f0` / `0x140002600` (an outlined `.part.0` region, intentionally
excluded). The outlined `.part.0` owners `d_encoding` and
`d_index_template_argument` are likewise left intact. No oracle/PDB data is
consulted by the pass.

The first static `c-clang-o2lto` run folded 43 definitions and changed 260
surviving signatures. A linker-map/PDB audit found 41 folded call targets had
different source symbols from their replacements. One pair shared the generic
`$$000000` map label, which does not establish clone identity; one deleted
definition had no changed direct-call target. No folded pair was confirmed as
a clone of one source function.

The 260 signature edits comprised 256 return changes and four added parameters.
The PDB identifies `_onexit` and `at_quick_exit` as returning function pointers,
but the pass recovered `i32`; it identifies `__acrt_set_locale_changed` as
`void()` but the pass recovered `i32`, and `__acrt_can_use_xstate_apis` as
`bool()` but the pass recovered `i64`. `getCallIndex` returns a `DName` by
value, while the pass exposed an `i64` return despite the MSVC sret ABI. The
four added parameters were on startup-argument helpers whose PDB prototypes
each have one formal. These are scoring findings only; the pass never reads
the map or PDB.

After the linker-map/PDB audit, the pass was tightened. It no longer folds
constant-substituted signatures, guesses returns from dead values without scalar
DI evidence, adds formals from ABI register stores, or treats globals in later
call-argument positions as folded `this` pointers. The static
`c-clang-o2lto` run now keeps all 3,401 definitions and makes zero signature
changes; `revenant-unipo,verify` succeeds. The map and PDB remain scoring-only.

The new FileCheck fixtures derive their bodies from `c-clang-o2lto`
`cmp_unsigned` and scalar add/sub shapes, plus the observed `c-gcc-o2`
`sub_140001017` call sequence that stores 2 in RCX before calling
`sub_140003A10`. They cover same-signature structural matches, constant
specialization rejection, DI-backed return recovery, address-taking, and
outlined cold partitions. They do not claim controlled fixture clones existed
in the measured binaries.

Raw clang LTO emission succeeds with the current driver. Its `ir-final` path
currently aborts in RerollPass on `sub_140001930` because an APInt exceeds the
signed 64-bit extraction limit. Normalized `ir-final` scoring remains blocked
on the separate driver/normalization owners; raw measurement above is complete.
**revenant-unrust (owner: MuseDeoptRust2).** Expects a module with
`revenant.lang == "rust"` (any other module is a no-op). Inverts six rustc
codegen shapings toward the compiler's ir-pre spelling: bounds compares back
to `icmp ult idx, len` with the panic path recognized by name
(`panic_bounds_check`, `slice_index_fail`, `slice_error_fail`) or by the
lifted `call + unreachable` panic-block shape (`ule` spellings also swap the
branch successors); folded overflow checks back to
`llvm.*.with.overflow` when the function still panics; Range/slice-iter
frames rebuilt only when the function holds no alloca at all and the `ult`
compare tests an induction phi with a constant step; Option/Result niche
discriminant tests restored from the sentinel-plus-payload ladder; Vec/String
triple frames rebuilt only when the raw buffer pointer walks through a phi;
inlined drop glue re-outlined into `drop_glue.unrust` when every operand the
free arm reads is remappable. Guarantees semantics-preserving rewrites only,
`!revenant.va` preserved, `!revenant.opaque` untouched. Runs explicitly via
`-passes=revenant-unrust`, not in the default pipeline.
