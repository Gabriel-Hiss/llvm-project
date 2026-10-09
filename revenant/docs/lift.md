# X86 lift generation

`revenant-tblgen -gen-revenant-lift` reads the X86 instruction records, direct SelectionDAG patterns, and Revenant lift DSL files. It emits `RevenantLift.inc`, which defines `llvm::revenant::liftX86Inst`. The caller supplies an `X86LiftContext`, decoded `MCInst`, instruction VA, and size. The dispatcher sets the VA range, then tries the exact `RVLift` or matching `RVMatch` body before a supported direct pattern. A `false` result means the caller must emit an opaque instruction.

`X86Lift.td` owns shared DSL operators and scalar/system semantics. `X87.td`, `StringAtomic.td`, `System.td`, `Sse.td`, and `Avx.td` add family semantics through `RevenantX86.td`. The emitter lowers supported expression and statement DAGs to IRBuilder calls and X86 context hooks. Direct pattern inversion currently accepts one-result, no-implicit-def patterns whose source DAG fits the supported SelectionDAG expression set.

The generated footer reports the table census. At this revision, the X86 target contains 21,973 instruction records. The dispatcher handles all of them: 1,631 DSL cases and 1,801 direct-pattern cases emit IR semantics; the remaining 18,541 emit opaque calls. These are table counts, not corpus instruction coverage; `lift-coverage.md` has the corpus-weighted numbers (99.89% of non-control instructions lifted). Dispatch is complete, but AVX-512, FMA, x87 long forms and several system instructions still lack non-opaque lifting semantics.

Build the generated dispatcher and context implementations, then run the provenance test:

```text
cmake --build build-rv --target revenant-lift-test -j4
```

The test creates IR instructions before and after changing the VA, then clears it and checks `!revenant.va` metadata.

## Exception handling

The PE EH decoder associates MSVC FuncInfo3/4, SEH scope-table, and Itanium
LSDA data with the owning runtime-function record. MSVC try ranges come from
contiguous IP-to-state runs, not the full function extent.

During lifting, throwing calls in a decoded try range become invokes with
native LLVM EH pads. Out-of-line handler functions are associated through
their owning `Program` code ranges, lifted with the MSVC frame pointer in
`rdx`, and inlined into catch or cleanup pads. A catch funclet's constant
`rax` continuation selects the parent block when the lifted body exposes one.
The Clang frontend turns the native pads into C++ try/catch or SEH constructs.
