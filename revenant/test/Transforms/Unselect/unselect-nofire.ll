; No-fire cases: selects the pass must leave alone.
; RUN: %revenant-opt -passes=revenant-unselect -S %s | %FileCheck %s --allow-unused-prefixes

declare ptr @opaque_callee(i32)

; A select feeding a same-block loop phi keeps its form.
define i32 @loopphi(i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @loopphi(
entry:
  br label %loop
loop:
; CHECK: loop:
  %p = phi i32 [ 0, %entry ], [ %s, %loop ]
  %c = icmp slt i32 %p, %a
  %s = select i1 %c, i32 %b, i32 %p
; CHECK: %s = select i1 %c, i32 %b, i32 %p
  br label %loop
}

; A select with a call arm is not speculated back: arms must be cheap.
define i32 @callarm(i1 %c, i32 %x) {
; CHECK-LABEL: define i32 @callarm(
entry:
  %v = call ptr @opaque_callee(i32 %x)
  %p = ptrtoint ptr %v to i32
  %s = select i1 %c, i32 %p, i32 %x
; CHECK: %s = select i1 %c, i32 %p, i32 %x
  ret i32 %s
}

; Opaque asm stubs are never touched.
define i32 @opaqueselect(i1 %c, i32 %x) {
; CHECK-LABEL: define i32 @opaqueselect(
entry:
  %s = select i1 %c, i32 1, i32 %x, !revenant.opaque !{}
; CHECK: %s = select i1 %c, i32 1, i32 %x, !revenant.opaque
  ret i32 %s
}
