# Compiler profile

`revenant/Compiler/Profile.h` exposes producer identity, language, and binary-visible build traits. Call `detectCompiler` when diagnostics need the evidence list; analyses should use `profileFor` and its producer-specific hooks. Unknown producers receive `GenericLlvmProfile`.

`tools/revenant-detect/` prints the profile for a PE as JSON (`producer`, `language`, `version`, `opt`, `lto`, `pgo`, `stack_protection`, `cfg`, `frame_pointers`, `hot_cold_split`, `evidence`). Unknown or unobservable fields are null. The tool feeds imports plus exports as symbol names; the detector never reads PDBs.

## Validated results (399 corpus binaries, October 2026)

Producer detection is 100%: msvc 130/130, clang 114/114, gcc 120/120, rustc 28/28, go 7/7. Language is exact for C, Rust, and Go; C++ reports `c` unless an import or export carries a mangled name, because linked binaries on this corpus expose no C++-only marker that survives dead-stripping (see limits below).

Detection rules, each confirmed against the corpus:

- Rich header: `0x93` is rustc; `0x101`-`0x105`/`0xfd`/`0x100`/`0x102` are MSVC toolchains; link entries `0x108`/`0x109` mean `/GL` plus LTCG; `0x10d` means LTCG plus PGO. `0x100` alone is a plain link marker, `0xfd` a stock object marker. `0xff` at link build is the rust-cargo self-link marker.
- clang-cl emits no Rich header. Its stable fingerprint is folded builtins (`memcpy_`/`memset_` sections) and CRT guard folding tables (`_RDATA` with `_guard_c`/`_guard_d`). Section matching is case-insensitive: COFF truncates and uppercases long names.
- Go has no Rich header either. It is found through `go1.` toolchain strings and `Go build ID` text in the image, plus the loader's build-id blob, `runtime.` symbols, and Go sections.
- MSVC without any Rich header still resolves through `.fptable` or CRT runtime imports.
- LTCG/PGO imply an optimized build, reported as estimated `O2`.
- CFG is reported only when guard tables survive in the image (`.gfids`/`.giats` bytes or guard imports). The linker often strips these sections even on `/guard:cf` builds, so absence proves nothing.
- Chained unwind entries (`ChainedTo`) report hot/cold split code.

## What the binary cannot show

- clang `-flto` versus plain `-flto`-less clang-cl: byte-identical section layouts, strings, imports, and data directories on all 16 corpus pairs. LTO stays unknown for clang.
- MSVC PGO without the `0x10d` link entry, and LLVM PGO after the final USEPROFILE relink: counter sections (`.lprfc`/`.lprfn`) exist only in instrumented intermediates, not in the shipped binary. Only `cjson-cl-o2pgo` (MSVC) out of the two PGO binaries on disk still carries evidence.
- Optimization level below LTCG/PGO: Rich counts, section layouts, and string markers are identical across `/Od`, `/O1`, `/O2`, `/Ox`, `-O0` through `-O3`, `-Os`, `-Oz`, and `-Og`. The estimate stays unknown.
- C versus C++ without PDB or exports: `?AV`/`?AU` byte hits fire on plain C binaries (format strings), and no cleaner marker survived. The detector reports the language it can prove.
- Stack protection (`/GS`), frame pointers (`/Oy-`), and versions: the corpus carries no surviving marker (no cookie imports, no guard tables, no version strings in the linked image), so these stay unknown rather than guessed.
- The profile registry also exposes coarse unwind and cold-chunk conventions. These are producer defaults, not proof that any specific function has exceptions or a cold fragment. Binary analysis must confirm function-level structure before transforming code.
