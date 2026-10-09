; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Real c-clang-o2 lift, corpus src/c/main.c:arith, VA 0x140001000.
; The multiply-high and signed correction chain computes (a-b)/7; the
; quotient-times-minus-seven residual computes (a-b)%7.
define i32 @arith(i32 %a, i32 %b) {
entry:
  %diff = sub i32 %a, %b
  %wide = sext i32 %diff to i64
  %product = mul i64 %wide, -1840700269
  %high = lshr i64 %product, 32
  %truncated = trunc i64 %high to i32
  %corrected = add i32 %diff, %truncated
  %sign = lshr i32 %corrected, 31
  %scaled = ashr i32 %corrected, 2
  %quotient = add i32 %scaled, %sign
  %times = mul i32 %quotient, -7
  %remainder = add i32 %diff, %times
  ret i32 %remainder
}
; CHECK-LABEL: define i32 @arith(
; CHECK: srem i32 %diff, 7
; CHECK-NOT: mul i64 %wide, -1840700269

define i32 @div_by_neg3(i32 %a, i32 %b) {
entry:
  %input = or i32 %a, %b
  %wide = sext i32 %input to i64
  %product = mul i64 %wide, 1431655765
  %high = lshr i64 %product, 32
  %truncated = trunc i64 %high to i32
  %adjusted = sub i32 %truncated, %input
  %sign = lshr i32 %adjusted, 31
  %scaled = ashr i32 %adjusted, 1
  %quotient = add i32 %scaled, %sign
  ret i32 %quotient
}
; CHECK-LABEL: define i32 @div_by_neg3(
; CHECK: sdiv i32 %input, -3
; CHECK-NOT: mul i64 %wide, 1431655765
