# Revenant frontend IR contract ("decompiler form")

The pipeline's final output per binary is one LLVM IR module in textual `.ll`
form. This document is the normative contract for that module: what producers
(lifter, transforms, structuring, type recovery) must emit and what the
Clang/rustc frontends may rely on. `revenant-ir-verify` enforces every MUST
below; everything else a frontend must tolerate.

RFC 2119 wording applies: MUST/REQUIRED is a hard error, SHOULD is a
recommendation frontends cannot assume, MAY is free choice.

## Compatibility

- Interchange format is **textual** LLVM IR.
- The producer is this fork, LLVM **22.1.3** (`llvm-project/build-revenant`).
- The strictest reader is the LLVM that ships with rustc: **CI LLVM 23.1.3**
  (`23.1.3-rust-1.101.0-nightly`, from
  `rust/build/x86_64-pc-windows-msvc/ci-llvm`; the rust fork's
  `src/llvm-project` submodule sits at `aab11ee0ea5e`). Rule: emit only
  constructs parseable by LLVM 23.1. Since 22.1 < 23.1 everything the fork
  writes is forward compatible; hand-written or generated text MUST NOT use
  syntax newer than LLVM 23 (e.g. `ptradd`, `usub.sat` forms added after 23,
  new attribute spellings).
- Debug info is emitted as **debug records** (`#dbg_declare`, `#dbg_value`),
  the modern format. The `call void @llvm.dbg.declare(...)` intrinsic spelling
  is still accepted by LLVM 22/23 and auto-upgraded on parse, so readers must
  look at `DbgVariableRecord`s after parsing, not raw dbg intrinsics.
- Opaque pointers only; no typed-pointer syntax anywhere.

## Module

REQUIRED:

- `target triple` reflecting the source binary (e.g.
  `x86_64-pc-windows-msvc`) and `target datalayout` matching it. Frontends
  read pointer size and ABI from here.
- Module flag `revenant.lang`, behavior **Override** (`i32 4`), value an
  `MDString`: one of `c`, `cpp`, `rust` — the detected producer language:

  ```llvm
  !llvm.module.flags = !{!0, ...}
  !0 = !{i32 4, !"revenant.lang", !"rust"}
  ```

- If the module carries any debug info (`llvm.dbg.cu`, `!dbg` attachments,
  dbg records), it MUST carry `"Debug Info Version" = i32 3`
  (`!{i32 2, !"Debug Info Version", i32 3}`) and `llvm.dbg.cu` MUST exist
  with at least one operand, each a `!DICompileUnit`. All producers share
  ONE compile unit created via `revenant::ir::getOrCreateCompileUnit`:
  `producer: "revenant"`, `emissionKind: FullDebug`, `isOptimized: true`,
  `language` matching `revenant.lang` (`DW_LANG_C` for c,
  `DW_LANG_C_plus_plus` for cpp, `DW_LANG_Rust` for rust).

- No other module flag or named metadata may use the `revenant.` prefix;
  unknown `revenant.*` module flags, named metadata, or metadata kinds on
  functions/globals/instructions are violations.

SHOULD: `source_filename` set to the original binary's file name.

## Functions

- One `define` per recovered function; `declare` for imports.
- Naming: a recovered symbol (export/import/RTTI/demangled or PDB-free
  name) is used verbatim, quoted if needed (`@"?Method@Klass@@QEAAHXZ"`,
  `@"_ZN4core6result…"`). Without a name the function is `sub_<hex VA>`
  (lowercase hex, no leading zeros, e.g. `@sub_140001234`). If the name is
  `sub_<hex>` it MUST equal the VA attribute below.
- Every `define` MUST carry the string attribute
  `"revenant.va"="0x<hex VA>"` of the function's entry address. `declare`s
  MAY carry it; there it denotes the IAT slot VA. Format inside the string:
  `0x` followed by lowercase hex.
- The IR signature is the recovered signature: integer, pointer, floating
  point, vector or aggregate types, `void` when no return is recovered.
  Calling convention SHOULD be the recovered one (`win64cc` for typical
  x64 PE code); `ccc` is the fallback.
- Standard LLVM attributes (`noreturn`, `nounwind`, `readonly`, …) MAY be
  used where recovered; frontends may ignore them. Attributes not known to
  LLVM 23 are forbidden.
- Function-level `!dbg` attaching a `DISubprogram` is REQUIRED when DI is
  present (see Debug info).

## Basic blocks

Every basic block in a `define` MUST be named. Names are stable across
passes:

- `bb_<hex VA>` — VA of the block's first lifted asm instruction.
  Canonical VA spelling in metadata operands is `i64 u0x<hex>` (bare `0x…`
  would lex as a float constant; readers also accept decimal `i64`).
- `bb_x<decimal index>` — blocks synthesized by IR passes with no asm
  provenance (critical-edge splits, phi-resolution blocks, …).


## Provenance: `!revenant.va`

Every instruction MUST carry provenance to the originating asm instruction:

```llvm
%x = add i64 %a, %b, !revenant.va !7
!7 = !{i64 u0x140001234}
```

Shape: a metadata node with exactly one operand, an `i64` constant (the VA).
Exempt: debug intrinsics/records (`llvm.dbg.*` calls and `#dbg_*` records
carry no provenance). `revenant.asm.*` opaque calls DO carry `!revenant.va`.

Global variables MAY carry the same `!{i64 VA}` attachment; for named
`g_`/`str_` globals the name already encodes it.

## Control flow: `!revenant.region`

Produced by RevenantStructure on region-header terminators. One attachment
per terminator; its value is either a single descriptor node, or a list node
of descriptors when one block heads several regions:

```llvm
br i1 %c, label %bb_140002010, label %bb_140002020, !revenant.region !4
!4 = !{!"if", i32 3}                          ; single descriptor
!5 = !{!{!"if", i32 6}, !{!"goto", i32 9}}    ; list form
```

Block operands are `i32` **indices into the function's basic block list**
(textual order, 0-based). `-1` means "none" where noted.

| kind | operands | attaches to | meaning |
|------|----------|-------------|---------|
| `"if"` | `follow` | conditional `br` | acyclic region; `follow` = post-dom join block (may be a real merge or the else-target when there is no else) |
| `"sc"` | `partner` | conditional `br` | short-circuit condition folded into the branch at `partner`; frontend should not emit a separate statement/block for this one |
| `"while"` | `header, latch, exit` | header terminator | pre-test natural loop |
| `"dowhile"` | `header, latch, exit` | header terminator | condition evaluated in `latch` |
| `"for"` | `header, latch, exit` | header terminator | while-shape with canonical induction variable; init/step recoverable from the header phis |
| `"loop"` | `header, latch, exit` | header terminator | natural loop that could not be shaped; `exit` may be `-1` |
| `"switch"` | `follow` | `switch` | jump-table region |
| `"goto"` | `target` | any terminator | the edge to `target` is irreducible; emit a real `goto` |

For loop kinds `header` MUST be the index of the annotated block and `latch`
MUST be a back-edge source to it. For every back edge `u -> h` in the CFG,
`h`'s terminator MUST carry a loop-kind descriptor. `"if"`/`"sc"` only on
conditional `br`, `"switch"` only on `switch`. Presence of any `"goto"`
descriptor in a function is the irreducible marker — frontends that see one
fall back to labels + `goto` for that region (or the whole function). A
frontend that ignores annotations entirely MUST still emit correct code
(labels + `goto`), because annotations are advisory except where a `"goto"`
or `"loop"` descriptor makes plain structuring impossible.

## Opaque instructions: `!revenant.opaque`

Instructions with no lifted IR semantics are calls to per-mnemonic stubs:

```llvm
declare i64 @revenant.asm.cpuid(i64, i64)

%r = call i64 @revenant.asm.cpuid(i64 %rax, i64 %rcx), !revenant.opaque !12
!12 = !{}          ; presence is the marker
```

Rules:

- Callee name: `revenant.asm.<lowercase mnemonic>`, always a `declare`
  (never `define`), any signature the lifter needs.
- The marker is an empty-tuple metadata attachment; presence is what counts.
- A `call`/`invoke` to a `revenant.asm.*` callee MUST carry the marker; the
  marker MUST NOT appear on calls to anything else.
- Opaque calls carry `!revenant.va` like every other instruction.

## Globals and imports

- Data globals: `@g_<hex VA>` (or a recovered name); string literals:
  `@str_<hex VA>`, a constant `[N x i8]` with the bytes. `str_` globals MUST
  be `[N x i8]` constants. Both MAY carry `!revenant.va`.
- Imports are `declare`s with their real symbol names; the optional
  `"revenant.va"` attribute gives the IAT slot.
- Vtables are ordinary `@g_`/`@vt_` globals of pointer arrays; the owning
  class relationship is expressed through DI (below).

## Debug info (recovered names and types)

Frontends read names and types exclusively from DI, exactly as they read
real debug info — the IR-level names/types are secondary.

- One shared `DICompileUnit` per module (see Module).
- Each `define` gets a `DISubprogram` (`spFlags: DISPFlagDefinition |
  DISPFlagOptimized`, `unit:` the CU, `type: DISubroutineType` mirroring the
  IR signature, `line:` ≥ 1 when attached to a `DILocation`), attached as
  `!dbg` on the function. `name:`/`linkageName:` carry the recovered symbol.
- Locals and parameters: `DILocalVariable` + `#dbg_declare` (or
  `#dbg_value` after promotion). `arg: n > 0` marks parameter n. The
  variable's scope chain MUST reach the function's `DISubprogram`.
- Types: `DIBasicType` (name + `sizeInBits` + DWARF `encoding`),
  `DIDerivedType` (`DW_TAG_pointer_type`/`reference_type`/`typedef`/
  `member`), `DICompositeType` (`DW_TAG_structure_type`/`class_type`/
  `union_type`/`enumeration_type`/`array_type`), `DIEnumerator` for enum
  members, `DISubroutineType` for function signatures.
- C++ classes with vtables: the class is a `DICompositeType` whose first
  member is named `_vptr`, a `DIDerivedType(DW_TAG_pointer_type)` whose
  base type is a pointer to the vftable's `DISubroutineType` array shape.
  Frontends treat `_vptr` as the vptr and synthesize the vtable global's
  `DIGlobalVariable` type accordingly.
- Rust `Result`-like enums: `DICompositeType DW_TAG_enumeration_type` with
  `DIEnumerator` elements for the variants; payload shape expressed via an
  associated structure member or the enum's `elements`.

## Verification

`revenant-ir-verify <file.ll>` runs the stock LLVM verifier first, then
every contract rule above. Diagnostics are `file: error: <context> —
<what>`. Exit codes: 0 clean, 1 contract violations, 2 parse/IO error.
Reference modules live in `test/ir-contract/` (valid) and
`test/ir-contract/invalid/` (each violating one rule).
