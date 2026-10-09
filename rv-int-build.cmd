@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
C:\env\ninja.exe -j8 -C "%~dp0build-rv" %*
