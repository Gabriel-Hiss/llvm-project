; RUN: revenant-idioms-test %s -o - | FileCheck %s
; minmax: icmp+select forms -> llvm.smin/smax, select abs -> llvm.abs.
define i32 @minmax(i32 %a, i32 %b) {
entry:
  %c1 = icmp slt i32 %a, %b
  %lo = select i1 %c1, i32 %a, i32 %b
  %c2 = icmp sgt i32 %a, %b
  %hi = select i1 %c2, i32 %a, i32 %b
  %c3 = icmp slt i32 %a, 0
  %n = sub i32 0, %a
  %mag = select i1 %c3, i32 %n, i32 %a
  %s1 = add i32 %lo, %hi
  %s2 = add i32 %s1, %mag
  ret i32 %s2
}
; CHECK-LABEL: define i32 @minmax(
; CHECK: call i32 @llvm.smin.i32(i32 %a, i32 %b)
; CHECK: call i32 @llvm.smax.i32(i32 %a, i32 %b)
; CHECK: call i32 @llvm.abs.i32(i32 %a, i1 false)
