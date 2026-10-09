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
| Transforms | SsaOut, Unrotate, Devectorize, TypeRecovery | core libraries build; `revenant-opt` link fails because Unrotate/SsaOut DLLs are passed as inputs instead of static libraries |
| Frontend clang | RevenantClangCore, revenant-clang | builds after merging ExprBuilder API fix |
| Driver | tools/revenant | builds |

## Tests

| Suite | Result |
|---|---|
| RevenantObjectTests | 5/5 pass |
| RevenantDisasmTests | 4/4 pass |
| RevenantAnalysisTests | 16/16 pass (Padding and Noreturn) |
| RevenantClangTypeTests | 4/4 pass |
| StructuredStmtBuilderTests | 5/7 pass; IfThen label assertion fails, Switch aborts on integer constant width |
| test/ir-contract (revenant-ir-verify) | 10/10 valid accepted, 10/10 invalid rejected |
| test/Structure (RUN lines via CMD) | 5/5 pass |
| test/Transforms/Unrotate (RUN lines via CMD) | 7/7 pass |
| TypeSmoke | aborts on `for.ll` because expected vtable global is missing |
| test/Transforms/Devectorize, TypeRecovery | not runnable through `revenant-opt`; the tool fails to link with Unrotate/SsaOut DLLs as inputs |
| revenant-clang on ir-contract modules | 4/10 emit; clang-cl compilation fails on every emitted file. `for.c` uses undeclared `v0`/`v1` and label `L_bb_140001308`. |

## Latest integration loop

- Merged new commits from MuseLiftSse, MuseNoreturn, MuseClangExpr, and RvSkeleton into `revenant`. The analysis CMake merge retains both Padding and Noreturn sources and tests; unit-test registration retains Compiler and ClangFrontend.
- `rv-int-build.cmd` initially exposed Expr API mismatches and untracked Unswitch code. MuseClangExpr fixed the API mismatch in a follow-up commit. The deopt agent moved Unswitch tests/code out of the integration checkout; no untracked Unswitch source is currently in the build. The full `rv-int-build.cmd` then completed successfully against the existing `build-revenant`.
- `revenant-opt` still fails to link because SsaOut/Unrotate are shared plugin DLLs rather than static libraries. The pass owners were notified. `ctest` has no registered tests; gtests and RUN lines were executed directly.

## Integration fixes applied (on revenant, not slice branches)

- `revenant/CMakeLists.txt`: include AddClang when Clang is found.
- `revenant/lib/Transforms/CMakeLists.txt`: new glue adding each pass dir.
- `revenant/lib/Transforms/SsaOut/CMakeLists.txt`: standalone-only setup
  guarded for superbuild use.
- `revenant/frontend/clang/unittests/CMakeLists.txt`: relative paths
  instead of worktree-absolute; wired via `frontend/clang/CMakeLists.txt`.
- `revenant/unittests/ClangFrontend/CMakeLists.txt`: new; test file adapted
  to `functionDecl(F, TypeBuilder&)` signature.
