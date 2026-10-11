; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; A two-window unroll with !rev.unroll evidence and a fixed trip bound.
; The original step-2 loop covers lanes (0,1) then (2,3). The folded
; step-1 loop covers lanes 0 through 3, preserving exactly the same range.
; Metadata confirms the unroll shape; it does not supply trip-count facts.

define i32 @sum_evidence(ptr %p0) !rev.unroll !0 {
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
  %c = icmp ult i32 %inext, 4
  br i1 %c, label %L, label %X

X:
  %r = phi i32 [ %s2, %L ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @sum_evidence
; CHECK: %s1 = add i32 %s, %l0
; CHECK-NOT: %g1 = getelementptr
; CHECK-NOT: %l1 = load
; CHECK: %inext = add i32 %i, 1
; CHECK: %c = icmp ult i32 %inext, 4
; CHECK: %r = phi i32 [ %s1, %L ]
; CHECK: ret i32 %r
!0 = !{i32 2}
