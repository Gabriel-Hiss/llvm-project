; RUN: revenant-idioms-test %s -o - | FileCheck %s
; urem7: quotient DAG plus q*-7+x -> udiv 7 and urem 7.
define i32 @urem7(i32 %x) {
entry:
  %e = zext i32 %x to i64
  %m = mul i64 %e, 613566757
  %hs = lshr i64 %m, 32
  %hi = trunc i64 %hs to i32
  %sub = sub i32 %x, %hi
  %s = lshr i32 %sub, 1
  %a = add i32 %s, %hi
  %q = lshr i32 %a, 2
  %mq = mul i32 %q, -7
  %r = add i32 %mq, %x
  ret i32 %r
}
; CHECK-LABEL: define i32 @urem7(
; CHECK: urem i32 %x, 7
