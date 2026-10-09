# X86 lift generation

`revenant-tblgen -gen-revenant-lift` reads the X86 instruction records, direct SelectionDAG patterns, and Revenant lift DSL files. It emits `RevenantLift.inc`, which defines `llvm::revenant::liftX86Inst`. The caller supplies an `X86LiftContext`, decoded `MCInst`, instruction VA, and size. The dispatcher sets the VA range, then tries the exact `RVLift` or matching `RVMatch` body before a supported direct pattern. A `false` result means the caller must emit an opaque instruction.

`X86Lift.td` owns shared DSL operators and scalar/system semantics. `X87.td`, `StringAtomic.td`, `System.td`, `Sse.td`, and `Avx.td` add family semantics through `RevenantX86.td`. The emitter lowers supported expression and statement DAGs to IRBuilder calls and X86 context hooks. Direct pattern inversion currently accepts one-result, no-implicit-def patterns whose source DAG fits the supported SelectionDAG expression set.

The generated footer reports the table census. At this revision, the X86 target contains 21,973 instruction records. The dispatcher handles all of them: 1,353 DSL cases and 88 direct-pattern cases emit IR semantics; the remaining 20,532 emit opaque calls. These are table counts, not corpus instruction coverage. Dispatch is complete, but most X86 opcodes still lack non-opaque lifting semantics.

Build the generated dispatcher and context implementations, then run the provenance test:

```text
cmake --build build-rv --target revenant-lift-test -j4
```

The test creates IR instructions before and after changing the VA, then clears it and checks `!revenant.va` metadata.
