; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; clamp: source-level compare/select shapes remain visible.
define i32 @clamp(i32 %x, i32 %lo, i32 %hi) {
entry:
  %c1 = icmp sgt i32 %x, %lo
  %t1 = select i1 %c1, i32 %x, i32 %lo
  %c2 = icmp slt i32 %t1, %hi
  %t2 = select i1 %c2, i32 %t1, i32 %hi
  ret i32 %t2
}
; CHECK-LABEL: define i32 @clamp(
; CHECK: icmp sgt i32 %x, %lo
; CHECK: select i1 %c1, i32 %x, i32 %lo
; CHECK: icmp slt i32 %t1, %hi
; CHECK: select i1 %c2, i32 %t1, i32 %hi
