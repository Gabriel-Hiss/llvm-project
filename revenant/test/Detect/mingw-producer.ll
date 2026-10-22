; Producer detection: stripped static-MinGW PEs (gcc -static-libgcc -s, no
; mingw/libgcc import DLLs) must detect as mingw-gcc, and MSVC / clang-cl
; images must not. Needs the parity corpus: set REVENANT_CORPUS to its root
; (the test skips when unset) and revenant-decompile in the build dir.
; REQUIRES: revenant-corpus
; RUN: %revenant-decompile --no-artifacts --emit asm %revenant-corpus/c-gcc-os/c-gcc-os.exe 2>&1 | %FileCheck %s
; RUN: %revenant-decompile --no-artifacts --emit asm %revenant-corpus/c-gcc-o0/c-gcc-o0.exe 2>&1 | %FileCheck %s
; RUN: %revenant-decompile --no-artifacts --emit asm %revenant-corpus/c-o2/c-o2.exe 2>&1 | %FileCheck %s --check-prefix=NOTMINGW
; RUN: %revenant-decompile --no-artifacts --emit asm %revenant-corpus/json-cl-o2/json-cl-o2.exe 2>&1 | %FileCheck %s --check-prefix=NOTMINGW

; CHECK: detect: c via mingw-gcc
; CHECK: MinGW (
; NOTMINGW-NOT: mingw-gcc
; NOTMINGW-NOT: MinGW (
; NOTMINGW: detect:
