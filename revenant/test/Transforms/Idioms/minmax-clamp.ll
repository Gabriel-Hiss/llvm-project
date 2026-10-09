; RUN: revenant-idioms-test %s -o - | FileCheck %s
; clamp: min(max(x,lo),hi) shape is already canonical; select form folds.
define i32 @clamp(i32 %x, i32 %lo, i32 %hi) {
entry:
  %c1 = icmp sgt i32 %x, %lo
  %t1 = select i1 %c1, i32 %x, i32 %lo
  %c2 = icmp slt i32 %t1, %hi
  %t2 = select i1 %c2, i32 %t1, i32 %hi
  ret i32 %t2
}
; CHECK-LABEL: define i32 @clamp(
; CHECK: call i32 @llvm.smax.i32(i32 %x, i32 %lo)
; CHECK: call i32 @llvm.smin.i32(
