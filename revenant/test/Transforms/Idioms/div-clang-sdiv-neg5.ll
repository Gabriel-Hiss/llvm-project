; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; sdiv-neg5-clang: clang ashr(m,33)+lshr(m,63) wide shape -> sdiv -5.
define i32 @sdivneg5c(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, -1717986919
  %sg = lshr i64 %m, 63
  %a = ashr i64 %m, 33
  %t1 = trunc i64 %a to i32
  %t2 = trunc i64 %sg to i32
  %q = add i32 %t1, %t2
  ret i32 %q
}
; CHECK-LABEL: define i32 @sdivneg5c(
; CHECK: sdiv i32 %x, -5
; CHECK-NOT: -1717986919
