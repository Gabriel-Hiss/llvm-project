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
