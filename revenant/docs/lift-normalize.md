# Lift normalization

`revenant-lift-normalize` runs in `CleanupPipeline`, which `Stages.cpp` uses
for `ir-clean` before any deoptimization pass. It also runs through
`runLiftCleanup` and the default `revenant-opt` pipeline.

The order is normalization, SROA, mem2reg, normalization, instcombine,
conservative simplifycfg, early-cse, SROA, instcombine, then the existing
GVN/DSE/DCE cleanup. A final normalization restores typed GEPs that
instcombine canonicalizes back to byte offsets. The second normalization
sees promoted register values rather than register loads and stores.

The lifter has already converted StackPtr deltas to constant byte GEPs rooted
at `frame`. Normalization groups overlapping access intervals into stack
slots. Equal-offset, equal-type accesses get scalar allocas. Overlapping or
mixed-type accesses keep one byte array, so SROA can partition them without
changing aliasing. Frames with escaping pointers, dynamic indices, volatile
or atomic accesses remain intact. No inferred object boundary overrides
observable pointer aliasing.

On integral-pointer targets, pointer-sized address add/sub trees become
non-inbounds byte GEPs. Pointer-valued register PHIs become pointer PHIs with
casts on incoming edges. Integer-only users keep the original register PHI.
EH edges are excluded. Arithmetic carrying no-wrap flags is left alone.
A byte GEP becomes an element GEP when all memory users agree on the access
type and its displacement is exactly divisible by that type's allocation
size. Scaled indices must use the target's pointer-index width.

`rv/MuseTypeRecovery` is already integrated as `revenant-types`. Its
TypeInference collects load/store field types and emits DI, not an IR type
rewriter. Normalization uses the same load/store evidence for element GEPs;
it does not turn a guessed DI layout into an LLVM struct. TypeRecovery still
runs later so it describes the normalized IR.

Truncated modular i64 add/sub/mul/and/or/xor operations become narrow
operations. Original wide operations remain when they have other users.
PHIs whose incoming values consistently extend i32 become i32 PHIs followed
by the corresponding extension. Constants must fit that extension. Cleanup
then folds truncation/extension chains. Widths are not guessed from register
names or signed comparisons.

Tests reduce the byte addressing, register PHIs and frame representation
from `c-clang-o2`'s lifted `sub_140024848`. They also cover overlapping slots,
escaping frames, scaled element addresses, and pointer induction backedges.

## Corpus measurement

Measured on full retained lifted modules, without reading PDBs or rebuilding
corpus binaries. C baselines are `MuseDeoptIV/build-rv-univ/clean-*.ir-clean.ll`.
The C++ baseline is `MuseDeoptCxx/scratch/cpp-o2-final.ir-final.ll`. The latter
already includes structure and SSA-out; it is not a fresh raw-lift comparison.
All normalized modules passed LLVM verification. Normalization plus SROA and
mem2reg also passed on both full C raw modules.

| Corpus | Functions | inttoptr before/after | ptrtoint before/after | i64 arithmetic before/after | Typed GEP before/after |
| --- | ---: | ---: | ---: | ---: | ---: |
| c-o2 | 3391 | 15435 / 7045 | 1334 / 1285 | 17437 / 6483 | 4059 / 12050 |
| c-clang-o2 | 3405 | 15839 / 7141 | 1320 / 1271 | 17856 / 6554 | 4015 / 12185 |
| cpp-o2 | 3563 | 15652 / 7254 | 1352 / 1302 | 17813 / 6669 | 4112 / 12168 |

Arithmetic counts include add, sub, mul, signed/unsigned division and remainder,
shifts, and bitwise operations with i64 result type. Cast counts include
instructions, not constant expressions. Typed GEP means a source element type
other than i8, including preexisting aggregate GEPs.

The compiler comparison is `artifacts/main.post.ll` for clang-cl O2. MSVC does
not provide LLVM IR, so c-o2 and cpp-o2 have no literal compiler ir-post module.
The matching-language clang-cl modules provide context, not a function-matched
denominator. Lifted modules include linked runtime code that the TU IR lacks.

| Compiler TU | Functions | inttoptr | ptrtoint | i64 arithmetic | Typed GEP |
| --- | ---: | ---: | ---: | ---: | ---: |
| c-clang-o2 main.post.ll | 42 | 0 | 10 | 47 | 104 |
| cpp-clang-o2 main.post.ll | 42 | 13 | 32 | 106 | 100 |

`revenant-univ` ran with verification on all three normalized modules. Full-file
counts of generated `.idx` i32 PHIs remain zero in each module. The previously
measured C baselines and a fresh C++ baseline run also had zero generated index
PHIs. The normalized `fill_vec` at clang-cl VA 0x1400017F0 has a unit-step
i64 PHI, widened i32 trip count, and typed i32 element GEP, but still does not
produce the matcher's generated index PHI.

The latest `revenant-uncxx` ran on all normalized modules and generated one
`revenant.cxx.vector.empty` call in each C module. The C++ module generated one
vector.empty and one vector.size call. All three baselines had zero helper calls.
Count helper calls, not declarations. The C hits are runtime-code matches, not
evidence that the C source used STL.

The latest `revenant-unswitch` ran with verification before and after
normalization. Added switch terminators were 150 / 150 for c-o2, 154 / 157 for
c-clang-o2, and 147 / 157 for cpp-o2. These are rewrite counts, not source-switch
fidelity scores. The current pass also expands ordinary range checks, such as a
vector-loop threshold, into switches. Its baseline therefore is not zero.

| Corpus | Univ generated i32 index PHIs before/after | Uncxx helper calls before/after | Unswitch new terminators before/after |
| --- | ---: | ---: | ---: |
| c-o2 | 0 / 0 | 0 / 1 | 150 / 150 |
| c-clang-o2 | 0 / 0 | 0 / 1 | 154 / 157 |
| cpp-o2 | 0 / 0 | 0 / 2 | 147 / 157 |

Reduced regression tests also cover repeated switch predecessor edges. Pointer
PHIs reuse one incoming cast per predecessor; creating two equivalent casts
for duplicate edges is invalid LLVM IR.

The results still contain thousands of integer-to-pointer casts. Base parameters
and loads have integer types until signature/field recovery proves pointers.
Opaque instructions and address-taken or dynamically indexed frames stay
conservative. This pass does not invent pointer field layouts or scalar trip
counts merely to make a deoptimization matcher fire.
