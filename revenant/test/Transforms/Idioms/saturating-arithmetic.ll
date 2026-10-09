; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Reconstruct plain C compare/select arithmetic from optimizer intrinsics.
define i8 @usub_sat(i8 %x, i8 %y) {
entry:
  %result = call i8 @llvm.usub.sat.i8(i8 %x, i8 %y)
  ret i8 %result
}
; CHECK-LABEL: define i8 @usub_sat(
; CHECK: icmp ult i8 %x, %y
; CHECK: sub i8 %x, %y
; CHECK: select i1 {{.*}}, i8 0, i8 {{.*}}
; CHECK-NOT: call i8 @llvm.usub.sat

define i32 @uadd_sat(i32 %x, i32 %y) {
entry:
  %result = call i32 @llvm.uadd.sat.i32(i32 %x, i32 %y)
  ret i32 %result
}
; CHECK-LABEL: define i32 @uadd_sat(
; CHECK: add i32 %x, %y
; CHECK: icmp ult i32 {{.*}}, %x
; CHECK: select i1 {{.*}}, i32 -1, i32 {{.*}}
; CHECK-NOT: call i32 @llvm.uadd.sat
