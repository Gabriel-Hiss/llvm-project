; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Guards on the negated-divisor fold: a positive divisor is already the
; source shape, INT_MIN has no positive negation, udiv is never touched,
; and sdiv by -1 keeps its (trapping) form.
define i32 @addpos3(i32 %a, i32 %x) {
entry:
  %q = sdiv i32 %a, 3
  %r = add i32 %x, %q
  ret i32 %r
}
; CHECK-LABEL: define i32 @addpos3(
; CHECK: sdiv i32 %a, 3
; CHECK: add i32 %x,

define i32 @addmin(i32 %a, i32 %x) {
entry:
  %q = sdiv i32 %a, -2147483648
  %r = add i32 %x, %q
  ret i32 %r
}
; CHECK-LABEL: define i32 @addmin(
; CHECK: sdiv i32 %a, -2147483648
; CHECK: add i32 %x,

define i32 @addudiv3(i32 %a, i32 %x) {
entry:
  %q = udiv i32 %a, 3
  %r = add i32 %x, %q
  ret i32 %r
}
; CHECK-LABEL: define i32 @addudiv3(
; CHECK: udiv i32 %a, 3
; CHECK: add i32 %x,

; `sdiv` by -1 folds the same way (`sub x, sdiv(y,1)`); the INT_MIN/-1
; trap stays a trap in both spellings.
define i32 @addneg1(i32 %a, i32 %x) {
entry:
  %q = sdiv i32 %a, -1
  %r = add i32 %x, %q
  ret i32 %r
}
; CHECK-LABEL: define i32 @addneg1(
; CHECK: sdiv i32 %a, 1
; CHECK: sub i32 %x,
