; RUN: %revenant-opt -passes=revenant-locals-to-slots -S %s | %FileCheck %s
;
; A loop-carried PHI makes its source variable memory-backed. A constant on a
; critical incoming edge must store on the split edge, not on both branches.

define i32 @choose(i1 %cond) {
; CHECK-LABEL: define i32 @choose(
; CHECK-NOT: phi i32
; CHECK: entry.header_crit_edge:
; CHECK: store i32 1, ptr %retval
; CHECK: br label %header
entry:
  br i1 %cond, label %left, label %header

left:
  br label %header

header:
  %value = phi i32 [ 1, %entry ], [ 2, %left ], [ %next, %latch ]
  %again = icmp slt i32 %value, 3
  br i1 %again, label %latch, label %exit

latch:
  %next = add i32 %value, 1
  br label %header

exit:
  ret i32 %value
}
