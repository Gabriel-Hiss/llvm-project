; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; Unsupported opcodes inside the DAG (here freeze on the multiply-high
; chain) never fold: the evaluator bails instead of proving a quotient.
define i32 @freezesurvive(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 2863311531
  %f = freeze i64 %m
  %hs = lshr i64 %f, 33
  %t = trunc i64 %hs to i32
  ret i32 %t
}
; CHECK-LABEL: define i32 @freezesurvive(
; CHECK-NOT: udiv i32 %x, 3
; CHECK: freeze i64 %m
; CHECK: ret i32 %t
