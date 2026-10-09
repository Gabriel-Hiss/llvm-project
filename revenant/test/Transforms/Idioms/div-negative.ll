; RUN: revenant-idioms-test %s -o - | FileCheck %s
; negative: genuine multiply (wrong shift) stays a multiply.
define i32 @mulsurvive(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 613566757
  %hs = lshr i64 %m, 30
  %t = trunc i64 %hs to i32
  %o = add i32 %t, 4
  ret i32 %o
}
; CHECK-LABEL: define i32 @mulsurvive(
; CHECK-NOT: udiv i32 %x, 7
; CHECK: ret i32 %o
