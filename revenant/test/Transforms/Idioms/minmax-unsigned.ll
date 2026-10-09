; RUN: revenant-idioms-test %s -o - | FileCheck %s
; umin/umax: unsigned icmp+select keeps unsigned intrinsics.
define i32 @uminmax(i32 %a, i32 %b) {
entry:
  %c1 = icmp ult i32 %a, %b
  %lo = select i1 %c1, i32 %a, i32 %b
  %c2 = icmp ugt i32 %a, %b
  %hi = select i1 %c2, i32 %a, i32 %b
  %s = add i32 %lo, %hi
  ret i32 %s
}
; CHECK-LABEL: define i32 @uminmax(
; CHECK: call i32 @llvm.umin.i32(i32 %a, i32 %b)
; CHECK: call i32 @llvm.umax.i32(i32 %a, i32 %b)
