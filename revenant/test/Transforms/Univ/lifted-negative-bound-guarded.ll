; RUN: %revenant-opt -passes=revenant-univ,verify -S %s | %FileCheck %s
; A widened equality exit may only be rewritten into a signed `slt` when
; the narrowed bound is provably non-negative. Without that proof the two
; forms disagree on the trip count for a negative `%n`: the widened form
; runs `zext %n - %start` iterations (a count near 2^32) while the signed
; form exits immediately.

; No guard, no `nneg`, no positive constant: nothing proves `%n >= 0`, so the
; loop keeps its widened exit.
define i32 @unguarded(ptr %values, i32 %n, i64 %start) {
entry:
  %wide = zext i32 %n to i64
  br label %loop

loop:                                             ; preds = %loop, %entry
  %iv = phi i64 [ %start, %entry ], [ %next, %loop ]
  %addr = getelementptr i32, ptr %values, i64 %iv
  %v = load i32, ptr %addr, align 4
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop

exit:                                             ; preds = %loop
  %r = phi i32 [ %v, %loop ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @unguarded(
; CHECK-NOT: .idx
; CHECK: %done = icmp eq i64 %next, %wide
; CHECK-NOT: .idx
; CHECK: br i1 %done, label %exit, label %loop

; The `%n < 1` preheader guard proves `%n >= 1`, so the rewrite is licensed
; and the loop comes back in source index form.
define i32 @guarded(ptr %values, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %pre

pre:
  %wide = zext i32 %n to i64
  br label %loop

loop:                                             ; preds = %loop, %pre
  %iv = phi i64 [ 0, %pre ], [ %next, %loop ]
  %addr = getelementptr i32, ptr %values, i64 %iv
  %v = load i32, ptr %addr, align 4
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop

exit:                                             ; preds = %loop, %entry
  %r = phi i32 [ 0, %entry ], [ %v, %loop ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @guarded(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %values, i32 %iv.idx
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit
; CHECK-NOT: icmp eq i64 %next, %wide

; `zext nneg` carries the same fact on the value itself: the bound peels
; back to the i32 source value through the widening, and the widening's
; `nneg` is what licenses the rewrite.
define i32 @nneg_bound(ptr %values, i32 %n) {
entry:
  br label %pre

pre:
  %wide = zext nneg i32 %n to i64
  br label %loop

loop:                                             ; preds = %loop, %pre
  %iv = phi i64 [ 0, %pre ], [ %next, %loop ]
  %addr = getelementptr i32, ptr %values, i64 %iv
  %v = load i32, ptr %addr, align 4
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop

exit:                                             ; preds = %loop
  %r = phi i32 [ %v, %loop ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @nneg_bound(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %values, i32 %iv.idx
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

; The proof has to cover every path into the header. This loop is also
; reachable through `%other`, which bypasses the `%n < 1` test entirely, so
; no dominance proof exists and the widened exit stays.
define i32 @guard_not_dominating(ptr %values, i32 %n, i64 %other.start,
                                 i32 %flag) {
entry:
  %wide = zext i32 %n to i64
  %choose = icmp eq i32 %flag, 0
  br i1 %choose, label %other, label %check

check:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %pre

pre:
  br label %loop

other:                                            ; unguarded path in
  br label %loop

loop:                                             ; preds = %loop, %pre, %other
  %iv = phi i64 [ 0, %pre ], [ %other.start, %other ], [ %next, %loop ]
  %addr = getelementptr i32, ptr %values, i64 %iv
  %v = load i32, ptr %addr, align 4
  %next = add i64 %iv, 1
  %done = icmp eq i64 %next, %wide
  br i1 %done, label %exit, label %loop

exit:                                             ; preds = %loop, %check
  %r = phi i32 [ 0, %check ], [ %v, %loop ]
  ret i32 %r
}

; CHECK-LABEL: define i32 @guard_not_dominating(
; The `%other` edge reaches the header without the guard, so the proof is
; unavailable and the exit keeps its widened form.
; CHECK-NOT: .idx
; CHECK: %done = icmp eq i64 %next, %wide
; CHECK: br i1 %done, label %exit, label %loop
