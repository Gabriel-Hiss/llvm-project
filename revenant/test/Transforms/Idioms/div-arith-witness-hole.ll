; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; An input-dependent compare folded through integer arithmetic (x == 42
; becomes zext+add, no SelectInst) is still a data-dependent branch, not
; a quotient DAG: the pass must not fold the root to udiv x, 3.
define i32 @division_with_arithmetic_exception(i32 %x) {
entry:
  %wide = zext i32 %x to i64
  %product = mul i64 %wide, 2863311531
  %shifted = lshr i64 %product, 33
  %quotient = trunc i64 %shifted to i32
  %special = icmp eq i32 %x, 42
  %flag = zext i1 %special to i32
  %result = add i32 %quotient, %flag
  ret i32 %result
}
; CHECK-LABEL: define i32 @division_with_arithmetic_exception(
; CHECK-NOT: udiv i32 %x, 3
; CHECK: ret i32 %result
