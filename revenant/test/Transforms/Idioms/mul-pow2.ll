; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Plain C multiplication stays multiplication in decompiler-form IR.
define i32 @mulpow2(i32 %x) {
entry:
  %m = mul i32 %x, 8
  ret i32 %m
}
; CHECK-LABEL: define i32 @mulpow2(
; CHECK: mul i32 %x, 8
