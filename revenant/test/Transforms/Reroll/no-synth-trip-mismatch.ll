; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Synth-side twin of the fold-side trip-mismatch pair (StepReroll).
; Forged !rev.unroll !{i32 4} on uneven lanes. Two locks:
;
; 1. `prefix_exact`: lanes 0,3 form a genuinely exact K=2 run, so the
;    evidence hook correctly synthesizes 2 trips; lanes 7,20 survive with
;    exact indices. The old truncating scan built K=3 with lane 6 (wrong
;    code, board 218). Trip count 2 plus intact 7/20 pins the fix.
; 2. `conflict`: per-lane deltas disagree inside window 1 (3 vs 4), so no
;    run is proved (K=1) and even forged evidence cannot enable synthesis.
;    Evidence lowers the count threshold only; it never relaxes equality.
; Pairs with the exit legality proof in tryFoldLoop/tryFoldPtrLoop; review
; both together.
define i32 @prefix_exact(ptr %p0, i32 %s0) !rev.unroll !0 {
entry:
  %g0 = getelementptr i32, ptr %p0, i32 0
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s0, %l0
  %g1 = getelementptr i32, ptr %p0, i32 3
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %g2 = getelementptr i32, ptr %p0, i32 7
  %l2 = load i32, ptr %g2, align 4
  %s3 = add i32 %s2, %l2
  %g3 = getelementptr i32, ptr %p0, i32 20
  %l3 = load i32, ptr %g3, align 4
  %s4 = add i32 %s3, %l3
  ret i32 %s4
}

; CHECK-LABEL: define i32 @prefix_exact
; CHECK: %u0c = icmp ult i64 %u0inext, 2
; CHECK: %g2 = getelementptr i32, ptr %p0, i32 7
; CHECK: %g3 = getelementptr i32, ptr %p0, i32 20
; CHECK: ret i32 %s4

define i32 @conflict(ptr %p0, i32 %s0) !rev.unroll !0 {
entry:
  %g0 = getelementptr i32, ptr %p0, i32 0
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s0, %l0
  %g1 = getelementptr i32, ptr %p0, i32 1
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %h0 = getelementptr i32, ptr %p0, i32 3
  %m0 = load i32, ptr %h0, align 4
  %t1 = add i32 %s2, %m0
  %h1 = getelementptr i32, ptr %p0, i32 5
  %m1 = load i32, ptr %h1, align 4
  %t2 = add i32 %t1, %m1
  %k0 = getelementptr i32, ptr %p0, i32 6
  %n0 = load i32, ptr %k0, align 4
  %u1 = add i32 %t2, %n0
  %k1 = getelementptr i32, ptr %p0, i32 7
  %n1 = load i32, ptr %k1, align 4
  %u2 = add i32 %u1, %n1
  %j0 = getelementptr i32, ptr %p0, i32 9
  %o0 = load i32, ptr %j0, align 4
  %v1 = add i32 %u2, %o0
  %j1 = getelementptr i32, ptr %p0, i32 10
  %o1 = load i32, ptr %j1, align 4
  %v2 = add i32 %v1, %o1
  ret i32 %v2
}

; CHECK-LABEL: define i32 @conflict
; CHECK: %h1 = getelementptr i32, ptr %p0, i32 5
; CHECK: ret i32 %v2
!0 = !{i32 4}
