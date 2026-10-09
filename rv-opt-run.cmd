@echo off
rem %1 = plugin dll, %2 = pass name, %3 = test file
set OPT=C:\Users\User\RustroverProjects\Revenant\llvm-project\build-revenant\bin\opt.exe
set FC=C:\Users\User\RustroverProjects\Revenant\llvm-project\build-revenant\bin\FileCheck.exe
%OPT% -load-pass-plugin=%1 -passes=%2 -S %3 | %FC% %3 --allow-unused-prefixes >nul 2>&1
echo EXIT=%ERRORLEVEL%
