; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; An input-dependent select (x == 42 adds 1) is not a pure quotient DAG:
; the pass must not fold the root to udiv x, 3.
define i32 @division_with_unwitnessed_exception(i32 %x) {
entry:
  %wide = zext i32 %x to i64
  %product = mul i64 %wide, 2863311531
  %shifted = lshr i64 %product, 33
  %quotient = trunc i64 %shifted to i32
  %special = icmp eq i32 %x, 42
  %changed = add i32 %quotient, 1
  %result = select i1 %special, i32 %changed, i32 %quotient
  ret i32 %result
}
; CHECK-LABEL: define i32 @division_with_unwitnessed_exception(
; CHECK-NOT: udiv i32 %x, 3
; CHECK: ret i32 %result
