# X86 lift generation

`revenant-tblgen -gen-revenant-lift` reads the X86 instruction records, direct SelectionDAG patterns, and Revenant lift DSL files. It emits `RevenantLift.inc`, which defines `llvm::revenant::liftX86Inst`. The caller supplies an `X86LiftContext`, decoded `MCInst`, instruction VA, and size. The dispatcher sets the VA range, then tries the exact `RVLift` or matching `RVMatch` body before a supported direct pattern. A `false` result means the caller must emit an opaque instruction.

`X86Lift.td` owns shared DSL operators and scalar/system semantics. `X87.td`, `StringAtomic.td`, `System.td`, `Sse.td`, and `Avx.td` add family semantics through `RevenantX86.td`. The emitter lowers supported expression and statement DAGs to IRBuilder calls and X86 context hooks. Direct pattern inversion currently accepts one-result, no-implicit-def patterns whose source DAG fits the supported SelectionDAG expression set.

The generated footer reports the table census. The X86 target contains 21,973 instruction records. The current dispatcher has 3,699 DSL cases, 1,801 direct-pattern cases, and 16,473 opaque cases. These are table counts, not corpus instruction coverage; `lift-coverage.md` has the last corpus-wide weighted census. FMA3 scalar forms, AES-NI, PCLMULQDQ, RNG, MXCSR and software interrupts now have explicit hooks. AVX-512 masked operations, string-compare intrinsic semantics, CRC32 and x87 long forms still need non-opaque lifting and a fresh whole-corpus coverage census.

Build the generated dispatcher and context implementations, then run the provenance test:

```text
cmake --build build-rv --target revenant-lift-test -j4
```

The test creates IR instructions before and after changing the VA, then clears it and checks `!revenant.va` metadata.
