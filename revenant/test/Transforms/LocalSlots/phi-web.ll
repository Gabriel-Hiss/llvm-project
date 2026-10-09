; RUN: %revenant-opt -passes=revenant-locals-to-slots -S %s | %FileCheck %s
;
; A loop-carried local with its backedge value defined in the loop body needs
; no critical-edge block when the web stores at definitions.

define i32 @count(i32 %limit) {
; CHECK-LABEL: define i32 @count(
; CHECK: %limit.addr = alloca i32
; CHECK: %v_i = alloca i32
; CHECK: %v_next = alloca i32
; CHECK: store i32 0, ptr %v_i
; CHECK: [[LOOP:loop]]:
; CHECK-NOT: phi i32
; CHECK: %[[I:[0-9]+]] = load i32, ptr %v_i
; CHECK: %next = add i32 %[[I]], 1
; CHECK: store i32 %next, ptr %v_next
; CHECK: %[[N:[0-9]+]] = load i32, ptr %v_next
; CHECK: store i32 %[[N]], ptr %v_i
; CHECK: %[[N2:[0-9]+]] = load i32, ptr %v_next
; CHECK: %[[L:[0-9]+]] = load i32, ptr %limit.addr
; CHECK: %cond = icmp slt i32 %[[N2]], %[[L]]
; CHECK: br i1 {{.*}}, label %[[LOOP]], label %exit
; CHECK: %[[N3:[0-9]+]] = load i32, ptr %v_next
; CHECK: ret i32 %[[N3]]
; CHECK-NOT: crit_edge
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %next = add i32 %i, 1
  %cond = icmp slt i32 %next, %limit
  br i1 %cond, label %loop, label %exit

exit:
  ret i32 %next
}
