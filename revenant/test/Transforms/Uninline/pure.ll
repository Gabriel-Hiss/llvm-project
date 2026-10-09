; Pure multi-instruction body: the arithmetic callee below (3 instructions)
; is spliced into the caller with arguments bound; the pass rebuilds the
; call and threads the result to the outer use.
; RUN: %revenant-opt -passes=revenant-uninline -S %s | %FileCheck %s
; CHECK-LABEL: define internal i32 @callee(
define internal i32 @callee(i32 %a, i32 %b) {
  %m = mul i32 %a, 3
  %s = add i32 %m, %b
  %t = xor i32 %s, %a
  ret i32 %t
}

; CHECK-LABEL: define i32 @caller(
; CHECK: %[[V:[0-9]+]] = call i32 @callee(i32 %x, i32 %y)
; CHECK: add i32 %[[V]], 1
define i32 @caller(i32 %x, i32 %y) {
entry:
  %m.i = mul i32 %x, 3, !revenant.va !{i64 6000}
  %s.i = add i32 %m.i, %y, !revenant.va !{i64 6001}
  %t.i = xor i32 %s.i, %x, !revenant.va !{i64 6002}
  %z = add i32 %t.i, 1
  ret i32 %z
}
