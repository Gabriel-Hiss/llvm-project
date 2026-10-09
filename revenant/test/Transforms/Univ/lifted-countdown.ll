; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUniv.dll -passes=revenant-univ,verify -S %s | FileCheck %s

; Shape from lifted c-o2 `copy_bytes` (VA 0x140001320): two `ptr` PHIs stepped
; by 1-element byte GEPs, an i64 counter mirror that only feeds itself and the
; returned byte count, and a countdown `i32` trip PHI stepped by -1 against 0.
; The bound is the countdown's start value, and the mirror's exit value is
; materialized as `%start + %new_step` on the exit edge.
define i32 @copy_bytes(ptr %dst, ptr %src, i32 %n) {
entry:
  %dst.bits = ptrtoint ptr %dst to i64, !revenant.va !1
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %common.ret, label %pre, !revenant.va !2

pre:
  br label %loop, !revenant.va !3

loop:                                             ; preds = %loop, %pre
  %remaining = phi i32 [ %n, %pre ], [ %remaining.next, %loop ]
  %src.ptr = phi ptr [ %src, %pre ], [ %src.next, %loop ]
  %dst.ptr = phi ptr [ %dst, %pre ], [ %dst.next, %loop ]
  %dst.mirror = phi i64 [ %dst.bits, %pre ], [ %dst.mirror.next, %loop ]
  %value = load i8, ptr %src.ptr, align 1, !revenant.va !4
  store i8 %value, ptr %dst.ptr, align 1, !revenant.va !5
  %dst.mirror.next = add i64 %dst.mirror, 1, !revenant.va !6
  %remaining.next = add i32 %remaining, -1
  %done = icmp eq i32 %remaining.next, 0, !revenant.va !7
  %src.next = getelementptr i8, ptr %src.ptr, i64 1, !revenant.va !8
  %dst.next = getelementptr i8, ptr %dst.ptr, i64 1, !revenant.va !6
  br i1 %done, label %common.ret, label %loop, !revenant.va !9

common.ret:                                       ; preds = %loop, %entry
  %ret = phi i64 [ %dst.bits, %entry ], [ %dst.mirror.next, %loop ]
  %diff = sub i64 %ret, %dst.bits
  %out = trunc i64 %diff to i32
  ret i32 %out, !revenant.va !10
}

; CHECK-LABEL: define i32 @copy_bytes(
; CHECK: %remaining.idx = phi i32 [ 0, %pre ], [ %remaining.next.idx, %loop ]
; CHECK: %src.ptr.idx = getelementptr inbounds i8, ptr %src, i32 %remaining.idx
; CHECK: %dst.ptr.idx = getelementptr inbounds i8, ptr %dst, i32 %remaining.idx
; CHECK: %remaining.next.idx = add i32 %remaining.idx, 1
; CHECK: %done.idx = icmp slt i32 %remaining.next.idx, %n
; The countdown and both pointer PHIs are gone; only the escape value stays.
; CHECK-NOT: phi ptr
; CHECK-NOT: add i32 %remaining, -1
; CHECK: %escape.end = add i64 %dst.bits, %escape.idx
; CHECK: br i1 %done.idx, label %loop, label %common.ret

!1 = !{i64 2200}
!2 = !{i64 2201}
!3 = !{i64 2202}
!4 = !{i64 2203}
!5 = !{i64 2204}
!6 = !{i64 2205}
!7 = !{i64 2206}
!8 = !{i64 2207}
!9 = !{i64 2208}
!10 = !{i64 2209}
!11 = !{i64 2210}
