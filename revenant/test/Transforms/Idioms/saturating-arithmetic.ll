; RUN: revenant-idioms-test %s -o - | FileCheck %s
; Unsigned saturation shapes from the compiler's compare/select lowering.
; Keep these adjacent to the arith and minmax corpus idioms: the rewrite must
; preserve modular arithmetic on the non-saturated path.
define i8 @usub_sat(i8 %x, i8 %y) {
entry:
  %underflow = icmp ult i8 %x, %y
  %difference = sub i8 %x, %y
  %result = select i1 %underflow, i8 0, i8 %difference
  ret i8 %result
}
; CHECK-LABEL: define i8 @usub_sat(
; CHECK: call i8 @llvm.usub.sat.i8(i8 %x, i8 %y)

define i32 @uadd_sat(i32 %x, i32 %y) {
entry:
  %sum = add i32 %x, %y
  %overflow = icmp ult i32 %sum, %x
  %result = select i1 %overflow, i32 -1, i32 %sum
  ret i32 %result
}
; CHECK-LABEL: define i32 @uadd_sat(
; CHECK: call i32 @llvm.uadd.sat.i32(i32 %x, i32 %y)
