; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; mul-odd: mul by 31 (from lea/syntax shapes) stays a mul.
define i32 @mulodd(i32 %x) {
entry:
  %m = mul i32 %x, 31
  ret i32 %m
}
; CHECK-LABEL: define i32 @mulodd(
; CHECK: mul i32 %x, 31
