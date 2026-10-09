# revenant/

LLVM-side decompiler pipeline for Revenant: PE load, MC decode, function
discovery/CFG, asm -> IR lift, IR deoptimization, and source emission.
The spec is the Rust implementation under `crates/` in the main
checkout; each area here mirrors one crate.

## Layout

```
include/revenant/<Area>/    public headers
lib/<Area>/                 one CMake library per area
lib/Target/X86/             x86 implementations of TargetHooks
tools/revenant/             driver (info / asm / --export-parity)
tools/revenant-tblgen/      TableGen backends
unittests/<Area>/           gtest per area
frontend/clang/             IR -> Clang AST -> C/C++ text
docs/                       design notes
```

Areas and their libraries: `Object` (RevenantObject, PE32+ loader),
`Disasm` (RevenantDisasm, MCDisassembler decode), `Analysis`, `Lift`,
`Transforms`, `Structure`, `Export`, `CAPI`. `Core` is headers only:
`Image`, `Instr`, `Function`, `BasicBlock`, `Edge`/`EdgeKind`,
`BlockEnd`, `Program`, `Xrefs`, `PaddingRun`, `Va`/`VaRange`.

`Transforms` includes Unsroa, which reverses aggregate scalarization on
LLVM IR. It regroups split allocas, rebuilds field-cluster structs, folds
only evidence-backed scalar copies, restores typed field GEPs, and
reassembles split aggregate values. Inferred fields must come from
constant, nonnegative offsets; byte-dispatch copies and array-like access
runs stay scalar.

## Build

Standalone against the shared build tree:

```
cmake -G Ninja -S <worktree>/revenant -B <worktree>/build-rv \
  -DLLVM_DIR=<llvm-project>/build-revenant/lib/cmake/llvm \
  -DClang_DIR=<llvm-project>/build-revenant/lib/cmake/clang \
  -DCMAKE_C_COMPILER=<VS clang-cl> -DCMAKE_CXX_COMPILER=<VS clang-cl> \
  -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe \
  -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe \
  -DLLVM_USE_LINKER=lld
ninja -C <worktree>/build-rv revenant RevenantUnitTests
```

Clang is optional: only `frontend/clang` needs it.
Never reconfigure or rebuild `build-revenant` itself.

## lit suite

One command runs everything under `revenant/test`:

```
python <llvm-project>/build-revenant/bin/llvm-lit.py -v <worktree>/revenant/test
```

The root `revenant/test/lit.cfg.py` picks lit's internal shell (llvm-lit on
this box resolves `bash` to the System32 WSL stub, so the external-shell
probe fails) and resolves `%opt`, `%FileCheck`, `%revenant-opt` and
`%{rv-structure-tool}` from a build dir: `$REVENANT_BUILD_DIR` (or
`$RV_BUILD_DIR`), else the newest `<worktree>/build-rv*` dir, else the
shared `build-revenant/bin`. Build the tools the suite needs with
`ninja -C <worktree>/build-rv revenant-opt rv-structure-tool`.

RUN lines all call the static `revenant-opt` (every `revenant-<name>` pass
linked in, no `-load-pass-plugin`). Per-pass directories keep their tests in
`revenant/test/Transforms/<Pass>`; `ir-contract` (contract example modules)
and `meta-mode` (corpus measurement scripts) are excluded from the suite.

## Lift tests

`RevenantLiftTests` checks function lifting and cleanup verification. Its corpus
test runs full analysis and CFG lifting for every discovered function in
`c-o0.exe`, `c-o2.exe`, `cpp-o2.exe`, `c-clang-o2.exe`, and `c-gcc-o2.exe`.
It reports verification totals, failure categories, and opaque fallback
mnemonic counts for each binary.

## Adding a slice

Drop a `CMakeLists.txt` into `lib/<Area>` (use `add_llvm_library`),
`tools/<name>` (`add_llvm_tool`), or `unittests/<Area>`
(`revenant_add_unittest(RevenantUnitTests <name> ...)`, from
`cmake/Revenant.cmake`). The top-level file already guards every known
directory; an unknown one needs a one-line `add_subdirectory` in the
`REVENANT_AREA_DIRS` list.

## Clang frontend

The LLVM type controls the emitted C/C++ expression category. Debug info may
refine signedness and record layout, but a conflicting pointer/integer type
must not override the IR type.


## Driver

```
revenant info <pe>                    sections, entry, counts
revenant asm <pe> <va> [n]            linear decode, Intel syntax
revenant --export-parity <pe> <dir>   revenant-items.bin + file-spans.tsv
                                      + meta.json (contract format)
```

`revenant-decompile <pe> [--libdb <file>]` uses the static-library database
for recovered function names and prototypes. Without `--libdb`, it looks for
`libdb.json` beside the executable.

The parity export runs full program analysis without loading a PDB; the
per-binary command and results are tracked in `parity.md`.

`revenant-decompile` runs the whole pipeline (PE to C/C++/Rust source); see
`engine.md`.
