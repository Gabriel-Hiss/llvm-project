# Compiler profile

`revenant/Compiler/Profile.h` exposes producer identity, language, and binary-visible build traits. Call `detectCompiler` when diagnostics need the evidence list; analyses should use `profileFor` and its producer-specific hooks. Unknown producers receive `GenericLlvmProfile`.

Detection intentionally leaves traits unknown when the linked PE cannot establish them. The Rich header identifies MSVC LTCG (`0x108`, `0x109`), MSVC PGO (`0x10b`, `0x0fe`), and rustc (`0x93`). Go uses linker build-id/runtime evidence. GCC uses MinGW/libgcc runtime imports or ELF-style unwind sections. Stack protection and CFG require their emitted runtime markers or guard tables. The detector does not read PDBs.

The profile registry also exposes coarse unwind and cold-chunk conventions. These are producer defaults, not proof that any specific function has exceptions or a cold fragment. Binary analysis must confirm function-level structure before transforming code.
