@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL || exit /b 1
if "%~1"=="build" (
  C:\env\ninja.exe -C "%~dp0build-rv" %2 %3 %4 %5 %6
  exit /b %ERRORLEVEL%
)
"C:\Program Files\CMake\bin\cmake.exe" -G Ninja -S "%~dp0revenant" -B "%~dp0build-rv" -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=C:/Users/User/RustroverProjects/Revenant/llvm-project/build-revenant/lib/cmake/llvm -DClang_DIR=C:/Users/User/RustroverProjects/Revenant/llvm-project/build-revenant/lib/cmake/clang -DCMAKE_MAKE_PROGRAM=C:/env/ninja.exe -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe -DLLVM_USE_LINKER=lld
