; InstCombine canonicalization undo: `samesign` markers and `zext nneg`
; churn go back to the plain -O0 spelling. The icmp keeps its predicate,
; the zext loses nneg. Other instructions are untouched.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unselect -S %s | %FileCheck %s

define i32 @t(i32 %a, i32 %n) {
; CHECK-LABEL: define i32 @t(
entry:
  %c = icmp samesign ult i32 %a, %n
  %z = zext nneg i1 %c to i32
  ret i32 %z
; CHECK: %c = icmp ult i32 %a, %n
; CHECK: %z = zext i1 %c to i32
; CHECK: ret i32 %z
}
