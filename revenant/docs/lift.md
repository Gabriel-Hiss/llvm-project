# X86 lift generation

`revenant-tblgen -gen-revenant-lift` reads the X86 instruction records, direct SelectionDAG patterns, and Revenant lift DSL files. It emits `RevenantLift.inc`, which defines `llvm::revenant::liftX86Inst`. The caller supplies an `X86LiftContext`, decoded `MCInst`, instruction VA, and size. The dispatcher sets the VA range, then tries the exact `RVLift` or matching `RVMatch` body before a supported direct pattern. A `false` result means the caller must emit an opaque instruction.

`X86Lift.td` owns shared DSL operators and scalar/system semantics. `X87.td`, `StringAtomic.td`, `System.td`, `Sse.td`, and `Avx.td` add family semantics through `RevenantX86.td`. The emitter lowers supported expression and statement DAGs to IRBuilder calls and X86 context hooks. Direct pattern inversion currently accepts one-result, no-implicit-def patterns whose source DAG fits the supported SelectionDAG expression set.

The generated footer reports the table census. The X86 target contains 21,973 instruction records. The current dispatcher has 3,772 DSL cases, 1,801 direct-pattern cases, and 16,400 opaque cases. These are table counts, not corpus instruction coverage; `lift-coverage.md` records a remeasurement over the 199 PE artifacts available under `target/parity`. Ordinary FMA3 forms, packed alternating FMA3 add/sub, AES-NI, PCLMULQDQ, CRC32, SSE4.2 string compares, RNG, MXCSR, locked memory NEG and software interrupts have explicit semantics. AVX-512 masked operations and default/embedded-rounding FMA forms, plus x87 environment, save/restore and BCD instructions, remain opaque.

## SSE packed conversions and scalar rounding

The lifter models packed `CVTDQ2PD`, `CVTDQ2PS`, `CVTPS2DQ`,
`CVTTPS2DQ`, and `CVTTPD2DQ` operations with vector IR conversions.
`CVTPD2PS` and `CVTTPD2DQ` zero the upper 64 result bits. `CVTDQ2PD`,
`CVTPS2PD`, `ROUNDSD`, and `ROUNDSS` memory operands read only their
architectural width.

Packed float-to-integer conversions return the x86 indefinite value `INT_MIN`
for NaN and out-of-range lanes.

`ROUNDSD` and `ROUNDSS` use `llvm.nearbyint`, `llvm.floor`, `llvm.ceil`, or
`llvm.trunc` according to the instruction's imm8 mode. The MXCSR-selected
mode uses `llvm.nearbyint`, so the JIT does not need x86-specific helper
symbols. The differential cases live in `LiftSemanticsCases.cpp`.

## AVX masked moves and broadcasts

`VMASKMOVPS`, `VMASKMOVPD`, and `VPMASKMOVD` use each mask lane's sign bit.
Masked loads zero inactive lanes. Masked stores touch only active lanes and
use byte alignment because their base address need not be naturally aligned.
`PTEST`/`VPTEST` calculate CF and ZF across the full 128- or 256-bit value.
`VPBROADCASTB/W/D/Q` splat the selected scalar element across the destination.

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
Invokes from the same decoded try-table region carry a shared
`!revenant.eh.region` ID. The Clang frontend groups them into one source
`try` statement with a shared handler set.

For Itanium LSDA call sites, positive action filters retain their RTTI
descriptor and decoded type name. Lifting emits those descriptors as landingpad
catch clauses and consumes matched exceptions with the `__cxa_begin_catch` /
`__cxa_end_catch` sequence; the C++ frontend uses the recovered Itanium type
name when spelling the catch declaration.
