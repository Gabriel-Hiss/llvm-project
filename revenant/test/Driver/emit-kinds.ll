; Every documented --emit kind must parse.
; RUN: %revenant-decompile --help 2>&1 | %FileCheck %s
; CHECK: --emit
; CHECK: asm|ir-raw|ir-clean|ir-pre-like|ir-final|source
