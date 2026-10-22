; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; A 4x-unrolled i32 sum loop (step 4, four s += a[i+j] windows) folds to a
; single-window loop stepping by 1. Ports crates/unopt reroll_in_loop_fold.

define i32 @sum_unrolled(ptr %p0, i32 %n) {
entry:
  ; The O2 caller rounds the count down to a multiple of 4 before calling
  ; and never reaches this loop with n == 0. The guard makes the
  ; differential in-contract: with n == 0 the unrolled body runs once and
  ; reads four elements while the folded body reads one.
  %empty = icmp slt i32 %n, 1
  ; The bound is a multiple of the window step, which is what the exit
  ; legality proof requires.
  %nm = shl i32 %n, 2
  br i1 %empty, label %X, label %L

L:
  %i = phi i32 [ 0, %entry ], [ %inext, %L ]
  %s = phi i32 [ 0, %entry ], [ %s4, %L ]
  %g0 = getelementptr i32, ptr %p0, i32 %i
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
  %c = icmp ult i32 %inext, %nm
  br i1 %c, label %L, label %X

X:
  %r = phi i32 [ 0, %entry ], [ %s4, %L ]
  ret i32 %r
}

; CHECK: define i32 @sum_unrolled
; The runtime trip count is unknown, so this latch-tested loop must not fold.
; CHECK: %g1 = getelementptr i32, ptr %g0, i32 1
; CHECK: %l1 = load i32, ptr %g1, align 4
; CHECK: %inext = add i32 %i, 4
; CHECK: %c = icmp ult i32 %inext, %nm
; CHECK: br i1 %c, label %L, label %X
; CHECK: %r = phi i32 [ 0, %entry ], [ %s4, %L ]
; CHECK: ret i32 %r

; Pointer-to-integer bounds have no proven low bits. The divisibility proof
; must leave this loop intact instead of treating the pointer as an integer.
define i32 @pointer_bound(ptr %p0, ptr %end) {
entry:
  %bound = ptrtoint ptr %end to i32
  br label %L

L:
  %i = phi i32 [ 0, %entry ], [ %inext, %L ]
  %s = phi i32 [ 0, %entry ], [ %s4, %L ]
  %g0 = getelementptr i32, ptr %p0, i32 %i
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
  %c = icmp ult i32 %inext, %bound
  br i1 %c, label %L, label %X

X:
  ret i32 %s4
}

; CHECK-LABEL: define i32 @pointer_bound
; CHECK: %bound = ptrtoint ptr %end to i32
; CHECK: %g1 = getelementptr i32, ptr %g0, i32 1
; CHECK: %inext = add i32 %i, 4

; The pointer advances by two bytes but the loop executes once. Replacing
; two stores by one and halving the pointer step would lose the second store.
define void @pointer_store_one_trip(ptr %base, i8 %byte) {
entry:
  br label %L

L:
  %p = phi ptr [ %base, %entry ], [ %next, %L ]
  %count = phi i32 [ 0, %entry ], [ %count.next, %L ]
  store i8 %byte, ptr %p, align 1
  %second = getelementptr i8, ptr %p, i64 1
  store i8 %byte, ptr %second, align 1
  %next = getelementptr i8, ptr %p, i64 2
  %count.next = add i32 %count, 1
  %done = icmp eq i32 %count.next, 1
  br i1 %done, label %exit, label %L

exit:
  ret void
}

; CHECK-LABEL: define void @pointer_store_one_trip
; CHECK: store i8 %byte, ptr %p, align 1
; CHECK: %second = getelementptr i8, ptr %p, i64 1
; CHECK: store i8 %byte, ptr %second, align 1
; CHECK: %next = getelementptr i8, ptr %p, i64 2
