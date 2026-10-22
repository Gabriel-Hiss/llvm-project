; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; A latch-tested D=2 loop with a trip count of one cannot fold. A folded
; loop would execute twice and drop one of the two original loads.

define i32 @sum_short(ptr %p0) {
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
  %c = icmp ult i32 %inext, 2
  br i1 %c, label %L, label %X

X:
  %r = phi i32 [ %s2, %L ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @sum_short
; CHECK: %g1 = getelementptr i32, ptr %g0, i32 1
; CHECK: %l1 = load i32, ptr %g1, align 4
; CHECK: %inext = add i32 %i, 2
; CHECK: %c = icmp ult i32 %inext, 2
; CHECK: br i1 %c, label %L, label %X
; CHECK: %r = phi i32 [ %s2, %L ]
