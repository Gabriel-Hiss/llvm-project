@echo off
rem Configure the Revenant LLVM fork build (LLVM only, X86 target, sccache).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

"C:\Program Files\CMake\bin\cmake.exe" ^
  -G Ninja -S "%~dp0llvm" -B "%~dp0build-revenant" ^
  -DCMAKE_MAKE_PROGRAM=C:/env/ninja.exe ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DLLVM_ENABLE_ASSERTIONS=ON ^
  -DLLVM_TARGETS_TO_BUILD=X86 ^
  -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DLLVM_USE_LINKER=lld ^
  -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe ^
  -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe ^
  -DLLVM_INCLUDE_BENCHMARKS=OFF ^
  -DLLVM_INCLUDE_EXAMPLES=OFF ^
  -DLLVM_ENABLE_ZSTD=OFF ^
  -DLLVM_ENABLE_LIBXML2=OFF
