@echo off
rem Configure the revenant/frontend/clang slice standalone.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
"C:\Program Files\CMake\bin\cmake.exe" ^
  -G Ninja -S "%~dp0revenant\frontend\clang" -B "%~dp0build-clang" ^
  -DCMAKE_MAKE_PROGRAM=C:/env/ninja.exe ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DLLVM_DIR=C:/Users/User/RustroverProjects/Revenant/llvm-project/build-revenant/lib/cmake/llvm ^
  -DClang_DIR=C:/Users/User/RustroverProjects/Revenant/llvm-project/build-revenant/lib/cmake/clang ^
  -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DLLVM_USE_LINKER=lld ^
  -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe ^
  -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe
