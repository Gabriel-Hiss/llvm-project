@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
"C:\Program Files\CMake\bin\cmake.exe" -G Ninja -S "%~dp0revenant" -B "%~dp0build-rv" ^
 -DCMAKE_MAKE_PROGRAM=C:/env/ninja.exe ^
 -DLLVM_DIR="%~dp0build-revenant/lib/cmake/llvm" ^
 -DClang_DIR="%~dp0build-revenant/lib/cmake/clang" ^
 -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
 -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
 -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe ^
 -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe ^
 -DLLVM_USE_LINKER=lld %*
