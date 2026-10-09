; RUN: revenant-idioms-test %s -o - | FileCheck %s
; neg: xor(x,-1)+1 -> sub(0,x), the abs building block.
define i32 @negxor(i32 %x) {
entry:
  %n = xor i32 %x, -1
  %r = add i32 %n, 1
  ret i32 %r
}
; CHECK-LABEL: define i32 @negxor(
; CHECK: sub i32 0, %x
