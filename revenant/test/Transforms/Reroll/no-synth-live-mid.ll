; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Negative: an intermediate window value read after the run has no single
; loop value, so the run must stay straight-line. Ports
; reroll_intermediate_live_out_blocks_synth.

define i32 @mid_live(ptr %p0, i32 %s0) {
entry:
  %g0 = getelementptr i32, ptr %p0, i32 0
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s0, %l0
  %g1 = getelementptr i32, ptr %p0, i32 1
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %g2 = getelementptr i32, ptr %p0, i32 2
  %l2 = load i32, ptr %g2, align 4
  %s3 = add i32 %s2, %l2
  %g3 = getelementptr i32, ptr %p0, i32 3
  %l3 = load i32, ptr %g3, align 4
  %s4 = add i32 %s3, %l3
  %t = add i32 %s2, %s4
  ret i32 %t
}

; CHECK: define i32 @mid_live
; CHECK-NOT: unroll:
; CHECK-NOT: phi i64
; CHECK: %t = add i32 %s2, %s4
; CHECK: ret i32 %t
