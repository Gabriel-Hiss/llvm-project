# Engine and `revenant-decompile`

`RevenantCAPI` (lib/CAPI) holds the engine that `revenant-decompile` and the
C ABI (`include/revenant-c/revenant.h`) share. A `Session` opens a PE, runs
analysis and producer detection once, then decompiles any set of functions.

## Stages

Every stage is the real implementation. A stage that cannot run fails the
run with its error; nothing falls back to a stub.

| stage | implementation |
|---|---|
| load | `PeFile::load` |
| analysis | `revenant::analyze` with the x86 `TargetHooks` |
| detect | `detectProducer` (language of the output when `--lang` is absent) |
| asm | `Disasm` listing of each function |
| sigs | `analyzeSignatures` over the whole program, cached per session, converted to the lifter's `lift::Signatures` |
| lift (ir-raw) | `lift::liftFunction`, then the module verifier |
| ir-clean | `CleanupPipeline` + `revenant-idioms`, then the verifier |
| ir-final | unopt passes, `revenant-structure`, `revenant-types`, `revenant-ssaout` (and `revenant-patterns` with `--patterns`), then the verifier |
| source | C/C++: `RevenantClangCore` + `printTranslationUnit`; Rust: the rustc fork |

The verifier runs after lift, ir-clean and ir-final, so a pass that emits
malformed IR is named in the error instead of crashing a frontend.
`--emit asm|ir-raw|ir-clean|ir-final|source` stops after the requested stage.

## Driver

```text
revenant-decompile <pe> [--func <va|name>]... [--lang c|cpp|rust]
                        [--emit asm|ir-raw|ir-clean|ir-final|source]
                        [-o <base>] [--patterns] [--list] [--time]
```

`-o <base>` writes `<base>.asm.txt`, `<base>.ir-raw.ll`, `<base>.ir-clean.ll`,
`<base>.ir-final.ll`, `<base>.source.<c|cpp|rs>` and `<base>.manifest.txt`
(only the stages that ran). `--time` prints the elapsed time of each stage.
Without `--func` every discovered function is decompiled; a Windows command
line holds a few hundred `--func` arguments, so chunk large sets.

## Rust path

The `source` stage runs `rustc -Zrevenant-decompile=<file.ll> -` from the
fork's stage-1 build (`REVENANT_RUSTC` overrides the path). rustc requires an
input name, the fork never reads it, and the generated crate comes back on
stdout.

## Checking a binary

```text
revenant-decompile target\parity\c-o0\c-o0.exe --lang c --time -o out\c-o0 \
    --func 0x140001000 --func 0x140001080 ...
clang-cl /c out\c-o0.source.c
```

On `c-o0`, `cpp-o2` and `rust-o0` the first 20 functions take 0.2 to 0.7 s per
run; the analysis stage dominates.
