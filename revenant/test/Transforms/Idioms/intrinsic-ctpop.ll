; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; ctpop: popcount spelling normalizes to the intrinsic.
define i32 @usectpop(i8 %x) {
entry:
  %p = call i8 @llvm.ctpop.i8(i8 %x)
  %e = zext i8 %p to i32
  ret i32 %e
}
declare i8 @llvm.ctpop.i8(i8)
; CHECK-LABEL: define i32 @usectpop(
; CHECK: call i8 @llvm.ctpop.i8(i8 %x)
