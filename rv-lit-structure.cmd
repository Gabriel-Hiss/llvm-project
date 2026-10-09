@echo off
set TOOL=C:\Users\User\RustroverProjects\Revenant\llvm-project\build-rv\lib\Structure\tool\rv-structure-tool.exe
set FC=C:\Users\User\RustroverProjects\Revenant\llvm-project\build-revenant\bin\FileCheck.exe
for %%F in (C:\Users\User\RustroverProjects\Revenant\llvm-project\revenant\test\Structure\*.ll) do (
  %TOOL% %%F | %FC% %%F >nul 2>&1
  if errorlevel 1 (echo FAIL %%~nxF) else (echo PASS %%~nxF)
)
