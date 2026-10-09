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

## Lift tests

`RevenantLiftTests` checks function lifting and cleanup verification. Its corpus
sample test lifts the first 12 decodable `.pdata` functions from `c-o0.exe`,
`c-o2.exe`, and `cpp-o2.exe` and verifies each resulting LLVM module.

## Adding a slice

Drop a `CMakeLists.txt` into `lib/<Area>` (use `add_llvm_library`),
`tools/<name>` (`add_llvm_tool`), or `unittests/<Area>`
(`revenant_add_unittest(RevenantUnitTests <name> ...)`, from
`cmake/Revenant.cmake`). The top-level file already guards every known
directory; an unknown one needs a one-line `add_subdirectory` in the
`REVENANT_AREA_DIRS` list.

## Driver

```
revenant info <pe>                    sections, entry, counts
revenant asm <pe> <va> [n]            linear decode, Intel syntax
revenant --export-parity <pe> <dir>   revenant-items.bin + file-spans.tsv
                                      + meta.json (contract format)
```

Until analysis lands, the export lists no functions: the byte map still
partitions the file (headers/sections/unknown/certificate/overlay).
