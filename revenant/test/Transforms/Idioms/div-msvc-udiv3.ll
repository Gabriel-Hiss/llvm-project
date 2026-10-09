; RUN: revenant-idioms-test %s -o - | FileCheck %s
; div3: MSVC udiv3 shape (mul by magic, lshr 33, trunc) -> udiv 3.
define i32 @div3(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 2863311531
  %hs = lshr i64 %m, 33
  %t = trunc i64 %hs to i32
  ret i32 %t
}
; CHECK-LABEL: define i32 @div3(
; CHECK: udiv i32 %x, 3
; CHECK-NOT: 2863311531
