; Phi locals keep every value a later read needs: phis alive at once do
; not share a local, an assignment is stored early only when nothing
; reads the old value afterwards, and the copies on one edge read old
; values before they are overwritten.
; RUN: %revenant-opt -passes=revenant-locals-to-slots -S %s | %FileCheck %s

; x lags y by one iteration; y's only use is x's phi, but both are alive
; in the loop, so they keep two locals.
; CHECK-LABEL: define i32 @lag(
; CHECK-DAG: %retval = alloca i32
; CHECK-DAG: %v_y = alloca i32
; CHECK: loop.loop_crit_edge:
; CHECK-NEXT: %[[OLDY:[0-9]+]] = load i32, ptr %v_y
; CHECK-NEXT: store i32 %[[OLDY]], ptr %retval
; CHECK-NEXT: store i32 %i1, ptr %v_y
; CHECK-NEXT: br label %loop

define i32 @lag(i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %x = phi i32 [ 0, %entry ], [ %y, %loop ]
  %y = phi i32 [ 1, %entry ], [ %i1, %loop ]
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i1, %n
  br i1 %c, label %loop, label %exit

exit:
  ret i32 %x
}

; The exit reads the old i, so i1 is not stored to i's local in the loop
; body: the store moves to the back edge.
; CHECK-LABEL: define i32 @oldvalue(
; CHECK: loop:
; CHECK: %i1 = add i32
; CHECK-NOT: store i32 %i1
; CHECK: br i1 %c
; CHECK: loop.loop_crit_edge:
; CHECK-NEXT: store i32 %i1, ptr %retval
; CHECK-NEXT: br label %loop

define i32 @oldvalue(i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i1, %n
  br i1 %c, label %loop, label %exit

exit:
  ret i32 %i
}

; A swap is a copy cycle: one old value goes through the spill local.
; CHECK-LABEL: define i32 @swap(
; CHECK: loop.loop_crit_edge:
; CHECK-NEXT: %[[OLDX:[0-9]+]] = load i32, ptr %v_x
; CHECK-NEXT: store i32 %[[OLDX]], ptr %v_spill
; CHECK-NEXT: %[[OLDY:[0-9]+]] = load i32, ptr %v_y
; CHECK-NEXT: store i32 %[[OLDY]], ptr %v_x
; CHECK-NEXT: %[[SAVED:[0-9]+]] = load i32, ptr %v_spill
; CHECK-NEXT: store i32 %[[SAVED]], ptr %v_y
; CHECK-NEXT: br label %loop

define i32 @swap(i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %x = phi i32 [ 1, %entry ], [ %y, %loop ]
  %y = phi i32 [ 2, %entry ], [ %x, %loop ]
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i1, %n
  br i1 %c, label %loop, label %exit

exit:
  %r = sub i32 %x, %y
  ret i32 %r
}
