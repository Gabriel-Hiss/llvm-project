; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Synth-side twin of the fold-side trip-divisible pair (StepReroll):
; 4 shape-matching windows with exact uniform delta 3 (offsets 0,3,6,9)
; synthesize the counted loop. Pairs with the exit legality proof in
; tryFoldLoop/tryFoldPtrLoop; review both together.
define i32 @even(ptr %p0, i32 %s0) {
entry:
  %g0 = getelementptr i32, ptr %p0, i32 0
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s0, %l0
  %g1 = getelementptr i32, ptr %p0, i32 3
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %g2 = getelementptr i32, ptr %p0, i32 6
  %l2 = load i32, ptr %g2, align 4
  %s3 = add i32 %s2, %l2
  %g3 = getelementptr i32, ptr %p0, i32 9
  %l3 = load i32, ptr %g3, align 4
  %s4 = add i32 %s3, %l3
  ret i32 %s4
}

; CHECK: define i32 @even
; CHECK: unroll:
; CHECK: %u0i = phi i64 [ 0, %entry ], [ %u0inext, %unroll ]
; CHECK: %u0c = icmp ult i64 %u0inext, 4
; CHECK: br i1 %u0c, label %unroll, label %unroll.exit
; CHECK: unroll.exit:
; CHECK: ret i32
