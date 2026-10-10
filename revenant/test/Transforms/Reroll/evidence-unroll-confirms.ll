; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Evidence confirmation: a 2x-unrolled loop carrying !rev.unroll !{i32 2}
; folds exactly like the bare shape. The factor only confirms the window
; count the slot arithmetic already proved; without metadata the same
; fold fires, so --meta=none output stays valid.

define i32 @sum_evidence(ptr %p0, i32 %n) !rev.unroll !0 {
entry:
  br label %L

L:
  %i = phi i32 [ 0, %entry ], [ %inext, %L ]
  %s = phi i32 [ 0, %entry ], [ %s2, %L ]
  %g0 = getelementptr i32, ptr %p0, i32 %i
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s, %l0
  %g1 = getelementptr i32, ptr %g0, i32 1
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %inext = add i32 %i, 2
  %c = icmp ult i32 %inext, %n
  br i1 %c, label %L, label %X

X:
  ret i32 %s2
}

; CHECK: define i32 @sum_evidence
; CHECK: %s1 = add i32 %s, %l0
; CHECK: %inext = add i32 %i, 1
; CHECK: ret i32 %s1
!0 = !{i32 2}
