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
| Transforms | SsaOut, Unrotate (plugin DLLs), Devectorize, TypeRecovery (static libs) | builds; Devectorize lit test needs the standalone plugin or revenant-opt |
| Frontend clang | RevenantClangCore, revenant-clang | builds |
| Driver | tools/revenant | builds |

## Tests

| Suite | Result |
|---|---|
| RevenantObjectTests (gtest) | 5/5 pass |
| RevenantDisasmTests (gtest) | 3/3 pass |
| RevenantClangTypeTests + TypeSmoke (gtest) | 4/4 pass; TypeSmoke needs an .ll arg (no default) |
| StructuredStmtBuilderTests (gtest) | compile-fixed by integrator; all tests abort at `CreateCondBr` on i32 condition (i1 required) — owner: Swe2ClangStmt/LunaClangStmt |
| test/ir-contract (revenant-ir-verify) | 10/10 valid accepted, 10/10 invalid rejected |
| test/Structure (lit-style) | 5/5 pass |
| test/Transforms/Unrotate + SsaOut (opt plugin) | 12/12 pass |
| test/Transforms/Devectorize, TypeRecovery | not runnable: no revenant-opt / plugin DLL in superbuild — owner: MuseTxPipeline |
| revenant-clang on ir-contract modules | 3/10 emit (for, if-else, while); crashes on call-import, cpp-method, leaf-locals, opaque-asm, rust-result, struct-di, switch — owners: Swe2ClangCore/MuseClangCpp; emitted .c fails clang-cl /c (undeclared v0/v2/v3 temporaries, missing labels) |

## Integration fixes applied (on revenant, not slice branches)

- `revenant/CMakeLists.txt`: include AddClang when Clang is found.
- `revenant/lib/Transforms/CMakeLists.txt`: new glue adding each pass dir.
- `revenant/lib/Transforms/SsaOut/CMakeLists.txt`: standalone-only setup
  guarded for superbuild use.
- `revenant/frontend/clang/unittests/CMakeLists.txt`: relative paths
  instead of worktree-absolute; wired via `frontend/clang/CMakeLists.txt`.
- `revenant/unittests/ClangFrontend/CMakeLists.txt`: new; test file adapted
  to `functionDecl(F, TypeBuilder&)` signature.
