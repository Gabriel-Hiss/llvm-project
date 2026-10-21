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
| ir-final | `revenant-uncxx` (C++ output), unopt passes, `revenant-unhotcold` (module pass), `revenant-structure`, `revenant-types`, `revenant-ssaout` (and `revenant-patterns` with `--patterns`), then the verifier |
| source | C/C++: `RevenantClangCore` + `printTranslationUnit`; Rust: the rustc fork |

The verifier runs after lift and after each individual cleanup or final
function pass. Invalid pass output restores the function's previous valid
body and records the function, stage and pass. Subsequent passes and source
emission continue. Assertions are pass bugs, not valid output.
Mixed pipelines also support module passes. Their cross-function changes
are verified together and, on verifier failure, the entire module pass is
rolled back. Function-pass adaptors still retain per-function isolation.
`--emit asm|ir-raw|ir-clean|ir-final|source` stops after the requested stage.

## Driver

```text
revenant-decompile <pe> [--func <va|name>]... [--lang c|cpp|rust]
                        [--meta none|embedded|pdb]
revenant-decompile <pe> [--func <va|name>]... [--func-file <path>]
                        [--lang c|cpp|rust]
                        [--emit asm|ir-raw|ir-clean|ir-final|source]
                        [-o <base>] [--patterns] [--list [--classify]]
                        [--libdb <path>] [--time]
```


`--meta` defaults to `embedded`. `none` keeps imported symbols for linking
and uses generic names for functions and data. `pdb` is accepted but reports
`not implemented yet`.
`embedded` keeps exports, retained COFF symbols and RTTI names. It attaches
source locations, panic messages, EH catch types, assertion/file/function
strings, and RTTI class/vtable data to IR as `!rev.source`, `!rev.panic`,
`!rev.eh.types`, `!rev.class`, `!rev.strings`, and `!rev.types`. `none` drops
embedded names and metadata, but retains data bytes and import names.

`--list --classify --libdb=<path>` prints `user-entry<TAB><va>` followed by
one row per function with its name and `user`, `library`, or `startup`
classification. It traces the PE entry through CRT startup to the user entry
(including MinGW's startup call convention), follows direct calls and
address-taken function pointers from user code, and stops at library matches.
When the user's first call reaches an unmatched MinGW constructor-list
dispatcher, that hook and code reachable only through it are treated as
runtime. An adjacent `libdb.json` is optional by default; an explicitly
supplied missing database is an error. `--func-file=<path>` reads one selector
per line.

`-o <base>` writes `<base>.asm.txt`, `<base>.ir-raw.ll`, `<base>.ir-clean.ll`,
`<base>.ir-final.ll`, `<base>.source.<c|cpp|rs>` and `<base>.manifest.txt`
(only the stages that ran). `--time` prints the elapsed time of each stage.
`--no-artifacts` prints the requested stage on stdout without writing any
artifact file. The ir-final artifact text is printed on a background thread
while the source stage reads the same module.
`REVENANT_SERIAL_ARTIFACT_PRINT=1` keeps that print on the main thread, and
`REVENANT_ARTIFACT_TIMING=1` reports what the artifact path costs.

`--list --classify` uses `Session::classifyFunctions`. It traces the user
entry and reachability from the PE entry even without a library database;
without a database, functions outside those reachable sets are reported as
`unclassified`. With a database, matched functions are reported as `library`
and their names are used in the listing. The IR pipeline adds
`"revenant.user"="true"` to user-reachable definitions and
`"revenant.lib"="<name>"` to matched library definitions, independent of
metadata mode. C++ deoptimization uses these attributes to keep runtime and
startup functions out of user-code transformations.

For MinGW executables, startup reaches `main` through the CRT after loading
`argc`, `argv`, and `envp` into the Windows argument registers. The classifier
uses that call setup to find `main` instead of selecting the largest reachable
tail target. It also keeps the first `main` call that reaches a constructor-table
dispatcher in the runtime set, rather than marking the dispatcher as user code.
Without `--func` every discovered function is decompiled; a Windows command
line holds a few hundred `--func` arguments, so chunk large sets or use
`--func-file`.

`test/meta-mode/invariant.py` samples 300 functions per release binary with
seed 211009, normalizes identifiers, type names and block labels, then
compares final IR for `none` and `embedded`. Names and types may differ
between the modes; control flow, statements and call argument counts may
not. A function that crashes in both modes is counted separately and
excluded; a one-mode crash or differing IR fails the check.
For a full-binary run without retaining intermediate `.ll` dumps, add
`--whole`; this also passes `--no-artifacts` to the driver.
`test/meta-mode/recovery-report.py` compares sampled real names with oracle
PDB or map symbols. Both scripts accept `--all-release`.

```text
python test/meta-mode/invariant.py --exe <revenant-decompile.exe> --corpus <target/parity> --all-release --sample 300 --seed 211009
python test/meta-mode/recovery-report.py --exe <revenant-decompile.exe> --corpus <target/parity> --pdbutil <llvm-pdbutil.exe> --all-release --sample 300 --seed 211009
python test/meta-mode/invariant.py --exe <revenant-decompile.exe> --corpus <target/parity> --targets c-o2 cpp-o2 rust-o2 cpp-gcc-o2 --whole
```

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
