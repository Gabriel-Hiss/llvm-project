; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Remainder signs: gcc emits identical code for `x%3` and `x%-3`
; (the remainders always agree), so both recover to `srem 3`.
; gcc srem3/srem_neg3: sign-corrected q, then x - q*3.
define i32 @gcc_srem3(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %sg = ashr i32 %x, 31
  %m = mul i64 %e, 1431655766
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %q = sub i32 %t, %sg
  %qd = mul i32 %q, 3
  %r = sub i32 %x, %qd
  ret i32 %r
}
; CHECK-LABEL: define i32 @gcc_srem3(
; CHECK: srem i32 %x, 3

define i32 @gcc_srem_neg3(i32 %x) {
entry:
  %e = sext i32 %x to i64
  %sg = ashr i32 %x, 31
  %m = mul i64 %e, 1431655766
  %h = lshr i64 %m, 32
  %t = trunc i64 %h to i32
  %q = sub i32 %t, %sg
  %qd = mul i32 %q, 3
  %r = sub i32 %x, %qd
  ret i32 %r
}
; CHECK-LABEL: define i32 @gcc_srem_neg3(
; CHECK: srem i32 %x, 3
