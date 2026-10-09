# Engine and `revenant-decompile`

`RevenantCAPI` (lib/CAPI) holds the engine that `revenant-decompile` and the
C ABI (`include/revenant-c/revenant.h`) share. A `Session` opens a PE, runs
analysis and producer detection once, then decompiles any set of functions.

## Stages

Every stage runs its real implementation. Binary-level setup and artifact
errors fail the run. A returned per-function asm or lift error records a
failed stage and skips that function while the remaining functions continue.

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

The verifier runs after lift and after each individual cleanup or final
function pass. Invalid pass output restores the function's previous valid
body and records the function, stage and pass. Subsequent passes and source
emission continue. Assertions are pass bugs, not valid output.
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

## Corpus robustness

`tools/revenant-decompile/robustness.py` scans every immediate corpus
directory for PE headers, including files without an executable extension.
It runs final IR and C/C++ emission for every PE with a separate timeout.
It copies the executable and adjacent DLLs before starting, so runs never
lock the integrator's executable. Results resume only for the same
executable SHA-256. A changed executable reruns every target.

```text
python tools/revenant-decompile/robustness.py --exe <revenant-decompile.exe> \
  --corpus <target/parity> --output <scratch/results> \
  --report docs/robustness.md --timeout 180 --workers 6
```

The report includes completed/expected invocation counts, final definition
counts, returned failures by stage and process-failure clusters. `unknown`
counts mean the process stopped before producing final IR. Crash stack
entries identify the current lift VA or function-pass name without logging
every successful function.
