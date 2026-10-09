; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Epilogue shape (-O3 unroll-by-N with remainder): a 4x main loop over
; sum_vec plus a scalar remainder loop must fold the main loop to step 1
; and leave the remainder loop alone.

define i32 @sum_epilogue(ptr %a, i32 %n) {
entry:
  %lim = and i32 %n, -4
  br label %main

main:
  %i = phi i32 [ 0, %entry ], [ %inext, %main ]
  %s = phi i32 [ 0, %entry ], [ %s4, %main ]
  %g0 = getelementptr i32, ptr %a, i32 %i
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %s, %l0
  %g1 = getelementptr i32, ptr %g0, i32 1
  %l1 = load i32, ptr %g1, align 4
  %s2 = add i32 %s1, %l1
  %g2 = getelementptr i32, ptr %g0, i32 2
  %l2 = load i32, ptr %g2, align 4
  %s3 = add i32 %s2, %l2
  %g3 = getelementptr i32, ptr %g0, i32 3
  %l3 = load i32, ptr %g3, align 4
  %s4 = add i32 %s3, %l3
  %inext = add i32 %i, 4
  %c = icmp ult i32 %inext, %lim
  br i1 %c, label %main, label %rem

rem:
  %j = phi i32 [ %lim, %main ], [ %jnext, %rem ]
  %t = phi i32 [ %s4, %main ], [ %t2, %rem ]
  %gj = getelementptr i32, ptr %a, i32 %j
  %lj = load i32, ptr %gj, align 4
  %t2 = add i32 %t, %lj
  %jnext = add i32 %j, 1
  %d = icmp ult i32 %jnext, %n
  br i1 %d, label %rem, label %X

X:
  %r = phi i32 [ %t2, %rem ]
  ret i32 %r
}

; CHECK: define i32 @sum_epilogue
; Main loop folds to step 1.
; CHECK: %inext = add i32 %i, 1
; Remainder loop keeps its own step of 1 and still exits to X.
; CHECK: %jnext = add i32 %j, 1
; CHECK: br i1 %d, label %rem, label %X
