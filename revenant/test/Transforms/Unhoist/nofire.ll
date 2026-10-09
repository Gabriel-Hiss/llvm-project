; RUN: %revenant-opt -passes=revenant-unhoist -S %s | %FileCheck %s
;
; Negative tests: shapes the pass must leave alone.
;
; CHECK-LABEL: define i32 @no_loop(
define i32 @no_loop(i32 %a, i32 %b) {
entry:
; A pure computation with no loop around it is not LICM output.
; CHECK: entry:
; CHECK: %s = add i32 %a, %b
; CHECK: ret i32 %s
  %s = add i32 %a, %b
  ret i32 %s
}

; CHECK-LABEL: define i32 @aliased_load(
define i32 @aliased_load(ptr %p, ptr %q, i32 %n) {
entry:
; Hoisted load of %p in the preheader, used in the body. The body
; stores through %q, which may alias %p: the load must stay hoisted.
; CHECK: entry:
; CHECK: %hv = load i32, ptr %p
; CHECK: br label %loop
  %hv = load i32, ptr %p, align 4
  br label %loop

loop:                                             ; preds = %latch, %entry
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit

body:                                             ; preds = %loop
; CHECK: body:
; CHECK: store i32 %i, ptr %q
; CHECK: %s = add i32 %hv, %i
  store i32 %i, ptr %q, align 4
  %s = add i32 %hv, %i
  br label %latch

latch:                                            ; preds = %body
  %i.next = add i32 %i, 1
  br label %loop

exit:                                             ; preds = %loop
  ret i32 0
}

; CHECK-LABEL: define void @may_trap(
define void @may_trap(ptr %p, i32 %n, i32 %d) {
entry:
; A possibly-trapping div hoisted above a loop that may run zero times
; must stay: the header has a second outside predecessor (bypass), so
; the loop is not guaranteed to run and sinking the div into the body
; could execute a fault the preheader version never reaches.
; CHECK: entry:
; CHECK: %q = sdiv i32 %n, %d
  %q = sdiv i32 %n, %d
  br label %header

header:                                           ; preds = %entry, %bypass, %latch
  %i = phi i32 [ 0, %entry ], [ 0, %bypass ], [ %i.next, %latch ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit

body:                                             ; preds = %header
  store i32 %q, ptr %p, align 4
  br label %latch

latch:                                            ; preds = %body
  %i.next = add i32 %i, 1
  br label %header

bypass:                                           ; preds = %entry
  br label %header

exit:                                             ; preds = %header
  ret void
}
