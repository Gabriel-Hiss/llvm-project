; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Cleanup canonicalizes `sub x, sdiv(y,3)` into `add x, sdiv(y,-3)`
; (c-o0 `arith`, VA 0x140001000). The negated divisor folds back to the
; source shape: `sub x, sdiv(y,3)`.
define i32 @addneg3(i32 %a, i32 %b, i32 %x) {
entry:
  %o = or i32 %a, %b
  %q.neg = sdiv i32 %o, -3
  %r = add i32 %q.neg, %x
  ret i32 %r
}
; CHECK-LABEL: define i32 @addneg3(
; CHECK: sdiv i32 {{.*}}, 3
; CHECK: sub i32 %x,
; CHECK-NOT: -3

; Commuted operand order folds the same way.
define i32 @addneg3_comm(i32 %a, i32 %b, i32 %x) {
entry:
  %o = or i32 %a, %b
  %q.neg = sdiv i32 %o, -3
  %r = add i32 %x, %q.neg
  ret i32 %r
}
; CHECK-LABEL: define i32 @addneg3_comm(
; CHECK: sdiv i32 {{.*}}, 3
; CHECK: sub i32 %x,
; CHECK-NOT: -3
