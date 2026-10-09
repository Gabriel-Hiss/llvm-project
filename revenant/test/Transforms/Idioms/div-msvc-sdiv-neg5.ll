; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; sdiv-neg5: MSVC smulhi + sign-correction shape -> sdiv -5.
define i32 @sdivneg5(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %m = mul i64 %e, -1717986919
  %hs = lshr i64 %m, 32
  %hi = trunc i64 %hs to i32
  %s = ashr i32 %hi, 1
  %c = lshr i32 %s, 31
  %q = add i32 %c, %s
  ret i32 %q
}
; CHECK-LABEL: define i32 @sdivneg5(
; CHECK: sdiv i32 %x, -5
; CHECK-NOT: -1717986919
