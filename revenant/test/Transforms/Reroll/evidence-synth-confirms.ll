; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Evidence-confirmed short synthesis: 4 shape-matching gep/load/add
; windows, but only the first 2 share a uniform delta (window 2 jumps by
; 10, window 3 returns to the stride). The bare shape (K=2 < 4) never
; synthesizes; with !rev.unroll confirming unrolled copies in the
; function, the pass builds the 2-trip counted loop over windows 0-1.
; Without the metadata the same shape stays put.
define i32 @sum_short(ptr %p0, i32 %s0) !rev.unroll !0 {
entry:
  %g0 = getelementptr i32, ptr %p0, i32 0
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s0, %l0
  %g1 = getelementptr i32, ptr %p0, i32 1
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %g2 = getelementptr i32, ptr %p0, i32 10
  %l2 = load i32, ptr %g2, align 4
  %s3 = add i32 %s2, %l2
  %g3 = getelementptr i32, ptr %p0, i32 11
  %l3 = load i32, ptr %g3, align 4
  %s4 = add i32 %s3, %l3
  ret i32 %s4
}

; CHECK: define i32 @sum_short
; CHECK: unroll:
; CHECK: %u0i = phi i64 [ 0, %entry ], [ %u0inext, %unroll ]
; CHECK: %u0c = icmp ult i64 %u0inext, 2
; CHECK: br i1 %u0c, label %unroll, label %unroll.exit
; CHECK: unroll.exit:
; CHECK: ret i32
!0 = !{i32 2}
