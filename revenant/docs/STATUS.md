# Integration status

Branch `revenant`, integrated build `llvm-project/build-rv`
(configured against `build-revenant`, `ninja -j8`). Updated by the
integrator after each merge loop.

## Builds

| Area | Targets | Status |
|---|---|---|
| Object | RevenantObject | builds |
| Disasm | RevenantDisasm | builds |
| IR contract | RevenantIR, revenant-ir-verify | builds |
| Structure | RevenantStructure, rv-structure-tool | builds |
| Target/X86 | RevenantTargetX86 | builds |
| Transforms | SsaOut, Unrotate, Devectorize, TypeRecovery, and newly merged deopt passes | integrated build pending; SsaOut and Unrotate now select STATIC libraries for the superbuild |
| Frontend clang | RevenantClangCore, revenant-clang | merged ClangAccept fixes, integrated rebuild pending |
| Driver | tools/revenant | builds |

## Tests

| Suite | Result |
|---|---|
| RevenantObjectTests | 5/5 pass |
| RevenantDisasmTests | 4/4 pass |
| RevenantAnalysisTests | last built set 16/16 pass; new Candidates, Overlaps, StackPtr, Signatures, and Orphan coverage not rebuilt |
| RevenantClangTypeTests | 4/4 pass before latest ClangAccept merge |
| StructuredStmtBuilderTests | pre-fix binary 5/7 pass; latest ClangAccept branch reports IfThen and Switch fixes, pending integrated rerun |
| test/ir-contract (revenant-ir-verify) | last run 10/10 valid accepted, 10/10 invalid rejected |
| test/Structure (RUN lines via CMD) | last run 5/5 pass |
| test/Transforms/Unrotate (RUN lines via CMD) | last run 7/7 pass; Unrotate owner reports 8/8 on standalone plugin |
| TypeSmoke | aborts on `for.ll` because expected vtable global is missing |
| test/Transforms/Devectorize, TypeRecovery | not run after merging newer transforms; `revenant-opt` superbuild target still needs a successful link |
| revenant-clang on ir-contract modules | prior integrated run emitted 4/10 and clang-cl rejected every emitted file; ClangAccept reports 10/10 references and 5 structure shapes now compile, pending integrated rerun |

## Latest integration loop

- Merged new commits from MuseXrefs, Discovery, CompilerProfile, DeoptAbi/Licm/Noreturn/StackPtr/Select/HotCold/Inline/TailDup/EH, LiftSystem/LiftFunction/LiftGen2, TxDevec/TxPipeline/TxUnrotate, JumpTables, Sigs, and ClangAccept. Resolved Analysis, X86, and TableGen CMake conflicts by retaining all distinct sources, tests, and target links.
- Registered new Candidates, Overlaps, StackPtr, and signature sources/tests. Added `UnrotatePlugin.cpp` to its static superbuild source list and changed SsaOut to build STATIC in the superbuild while retaining the standalone plugin mode.
- Full `rv-int-build.cmd` now stops compiling Candidates.cpp and Overlaps.cpp because `AnalysisImpl.h` is still untracked in LunaDiscovery's worktree. LunaDiscovery confirmed the header is shared by those sources and plans to commit it. The earlier CMake scan errors for unregistered sources are fixed.
- `ctest --test-dir build-rv` has no registered tests. Direct gtests and RUN lines above describe the last fully built integration state; the latest merged changes still need a clean integrated build and test run.

## Integration fixes applied (on revenant, not slice branches)

- `revenant/CMakeLists.txt`: include AddClang when Clang is found.
- `revenant/lib/Transforms/CMakeLists.txt`: new glue adding each pass dir.
- `revenant/lib/Transforms/SsaOut/CMakeLists.txt`: standalone-only setup
  guarded for superbuild use.
- `revenant/frontend/clang/unittests/CMakeLists.txt`: relative paths
  instead of worktree-absolute; wired via `frontend/clang/CMakeLists.txt`.
- `revenant/unittests/ClangFrontend/CMakeLists.txt`: new; test file adapted
  to `functionDecl(F, TypeBuilder&)` signature.
