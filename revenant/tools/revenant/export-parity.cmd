@echo off
rem compare.py invokes "<exporter> <binary> <outdir>"; adapt that to
rem "revenant --export-parity <binary> <outdir>". Copied next to
rem revenant.exe by CMake, so %~dp0 always finds the driver.
"%~dp0revenant.exe" --export-parity %*
