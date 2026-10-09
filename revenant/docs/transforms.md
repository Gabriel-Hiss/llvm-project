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
Folds verified multiply-high and correction DAGs back into constant
`udiv`/`sdiv`, then folds quotient-times-divisor residuals into
`urem`/`srem`. It checks every input for i8/i16 and quotient boundaries plus
integer extrema for i32/i64. It also canonicalizes select-based abs/min/max,
unsigned saturating add/sub, rotates and byte swaps. Intrinsic popcount forms
pass through unchanged. Ambiguous arithmetic remains intact.
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
inspect function names or oracle symbols.
It specializes a candidate general definition at one constant parameter, runs
InstCombine, SimplifyCFG and SCCP on temporary copies, then hashes normalized
bodies with constants abstracted. Hash matches require exact instruction,
operand, CFG and semantic-metadata equality modulo local names before merging.
The specialized function may retain its parameter or omit one parameter,
including a middle parameter. Direct callers then call the general definition
with the proven constant, and the specialized definition is deleted.

When no general definition survived, lifted Win64 register-slot stores provide
another recovery path. Every direct caller must have the same locally reaching
constant in the next RCX/RDX/R8/R9 slot. Calls and unknown writes stop the search.
The pass adds that formal and passes the observed constant at every caller.
Scalar body uses of that constant become uses of the formal. Constant-only
operands, including switch cases and struct GEP indices, stay constant.
This proves equivalence at the recovered callers; it does not prove that a
source-level formal existed. Conflicting constants, missing register evidence,
address-taken functions, exports and operand bundles prevent this recovery.
LunaSigs3 currently exposes no extra-argument IR metadata, so Unipo reads lifted
call sites directly. Run this recovery before dead register stores disappear.

Existing return restoration and DI-driven dead-argument restoration remain.
Named clone suffixes are only a source-name recovery path when a symbol exists,
not evidence used by structural matching. New calls preserve instruction
metadata and VA provenance. Structural matching rejects opaque instructions
and region annotations rather than discarding them during normalization.

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

On fresh normalized `c-clang-o2lto` IR, the statically linked
`revenant-opt -passes=revenant-unipo,verify` run succeeds: definitions change
from 3,401 to 3,358 (43 duplicate bodies folded), with 260 surviving
definitions changing return or parameter types. This is not a GCC named-clone
oracle corpus, so it is reported separately, not added to the 9-entry GCC
recovery score. The earlier plugin-DLL run failed because of cross-LLVMContext
constant/attribute state; the static run avoids that plugin boundary.

The new FileCheck fixtures derive their bodies from `c-clang-o2lto`
`cmp_unsigned` and scalar add/sub shapes, plus the observed `c-gcc-o2`
`sub_140001017` call sequence that stores 2 in RCX before calling
`sub_140003A10`. Controlled variants cover omitted/middle/retained parameters,
clone-only restoration, exact constant mismatches, address-taking, barriers,
conflicting callers and constant-only switch operands. They do not claim those
controlled specializations existed in the measured binaries.

Raw clang LTO emission succeeds with the current driver. Its `ir-final` path
currently aborts in RerollPass on `sub_140001930` because an APInt exceeds the
signed 64-bit extraction limit. Normalized `ir-final` scoring remains blocked
on the separate driver/normalization owners; raw measurement above is complete.
