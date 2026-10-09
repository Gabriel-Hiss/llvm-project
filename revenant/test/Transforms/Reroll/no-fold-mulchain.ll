; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Corpus-derived negative: @hash_vec's 4x scalar-unrolled main loop (%29)
; from target/parity/c-clang-o2/artifacts/main.post.ll (clang-cl -O2).
; Each window's add feeds a shared mul-chain, so the windows never tile
; uniformly; the reference reroll also leaves it (reroll x0 on the
; decompiled hash_vec). The pass must preserve the loop bit-for-bit.

define i32 @hash_vec_main(ptr %a, i64 %lim, i64 %trip) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inext, %loop ]
  %s = phi i32 [ 0, %entry ], [ %s4, %loop ]
  %t = phi i64 [ 0, %entry ], [ %tnext, %loop ]
  %m0 = mul i32 %s, 31
  %g0 = getelementptr i32, ptr %a, i64 %i
  %l0 = load i32, ptr %g0, align 4
  %s1 = add i32 %l0, %m0
  %m1 = mul i32 %s1, 31
  %g1 = getelementptr i32, ptr %a, i64 %i
  %g1b = getelementptr i8, ptr %g1, i64 4
  %l1 = load i32, ptr %g1b, align 4
  %s2 = add i32 %l1, %m1
  %m2 = mul i32 %s2, 31
  %g2 = getelementptr i32, ptr %a, i64 %i
  %g2b = getelementptr i8, ptr %g2, i64 8
  %l2 = load i32, ptr %g2b, align 4
  %s3 = add i32 %l2, %m2
  %m3 = mul i32 %s3, 31
  %g3 = getelementptr i32, ptr %a, i64 %i
  %g3b = getelementptr i8, ptr %g3, i64 12
  %l3 = load i32, ptr %g3b, align 4
  %s4 = add i32 %l3, %m3
  %inext = add i64 %i, 4
  %tnext = add i64 %t, 4
  %c = icmp eq i64 %tnext, %trip
  br i1 %c, label %exit, label %loop

exit:
  ret i32 %s4
}

; The unrolled body and both step-4 increments survive untouched.
; CHECK: %s4 = add i32 %l3, %m3
; CHECK: %inext = add i64 %i, 4
; CHECK: %tnext = add i64 %t, 4
; CHECK-NOT: %u0i = phi i64
; CHECK: ret i32 %s4
