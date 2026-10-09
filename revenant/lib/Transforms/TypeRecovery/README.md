# RevenantTransformTypeRecovery

Implements the `revenant-types` pass
(`revenant/include/revenant/Transforms/TypeRecovery/TypeRecovery.h`).

Build (standalone against the shared build tree, from a `vcvars64` shell;
never reconfigure or rebuild `build-revenant` itself):

```cmd
cmake -G Ninja -S revenant -B build-rv ^
  -DLLVM_DIR=<build-revenant>\lib\cmake\llvm ^
  -DClang_DIR=<build-revenant>\lib\cmake\clang ^
  -DCMAKE_C_COMPILER=<VS clang-cl> -DCMAKE_CXX_COMPILER=<VS clang-cl> ^
  -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe ^
  -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe ^
  -DLLVM_USE_LINKER=lld
ninja -C build-rv -j4 RevenantTransformTypeRecovery
```

Requires RevenantIR (`revenant/IR/Contract.h`, RvIRContract): the build
fails with `#error` until that header lands. No other revenant
dependencies.
