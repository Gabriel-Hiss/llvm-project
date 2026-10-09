; RUN: %revenant-opt -passes=revenant-univ,verify -S %s | %FileCheck %s

; Shape from lifted c-clang-o2 `cmp_vec` (VA 0x1400019F0). The early-exit
; block returns the index, so the IV escapes as `trunc i64 %iv to i32` in a
; block outside the loop; the rewrite drops the trunc and hands the new
; narrow index to the result PHI directly.
define i32 @cmp_vec(ptr %a, ptr %b, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %common.ret, label %pre, !revenant.va !1

pre:
  %wide = zext nneg i32 %n to i64, !revenant.va !2
  br label %loop, !revenant.va !3

loop:                                             ; preds = %latch, %pre
  %iv = phi i64 [ 0, %pre ], [ %next, %latch ]
  %a.addr = getelementptr i32, ptr %a, i64 %iv, !revenant.va !4
  %a.value = load i32, ptr %a.addr, align 1, !revenant.va !4
  %b.addr = getelementptr i32, ptr %b, i64 %iv, !revenant.va !5
  %b.value = load i32, ptr %b.addr, align 1, !revenant.va !5
  %same = icmp eq i32 %a.value, %b.value, !revenant.va !6
  br i1 %same, label %latch, label %bb_found, !revenant.va !6

latch:                                            ; preds = %loop
  %next = add i64 %iv, 1, !revenant.va !7
  %done = icmp eq i64 %next, %wide, !revenant.va !6
  br i1 %done, label %common.ret, label %loop, !revenant.va !8

bb_found:                                         ; preds = %loop
  %index = trunc i64 %iv to i32, !revenant.va !9
  br label %common.ret, !revenant.va !10

common.ret:                                       ; preds = %bb_found, %latch, %entry
  %result = phi i32 [ %index, %bb_found ], [ -1, %latch ], [ -1, %entry ]
  ret i32 %result, !revenant.va !11
}

; CHECK-LABEL: define i32 @cmp_vec(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %latch ]
; CHECK: %a.addr.idx = getelementptr inbounds i32, ptr %a, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %b.addr.idx = getelementptr inbounds i32, ptr %b, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %common.ret
; The trunc escape is gone: the result PHI takes the index directly.
; CHECK: bb_found:
; CHECK-NOT: trunc i64
; CHECK: %result = phi i32 [ %iv.idx, %bb_found ], [ -1, %latch ], [ -1, %entry ]

; Shape from lifted c-clang-o2 `hash_vec` scalar tail (VA 0x140001A20): the
; base pointer is materialized inside the loop from invariant operands
; (`getelementptr i8, %base, %scale`), and the bound peels to an i32.
define i32 @hash_vec(ptr %base, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %common.ret, label %pre, !revenant.va !1

pre:
  %rem = and i32 %n, 3, !revenant.va !2
  %wide = zext nneg i32 %rem to i64, !revenant.va !2
  %big = icmp ugt i32 %n, 3
  br i1 %big, label %bb_main, label %bb_tail, !revenant.va !3

bb_main:                                          ; preds = %pre
  br label %bb_tail, !revenant.va !4

bb_tail:                                          ; preds = %bb_main, %pre
  %start.i32 = phi i32 [ 0, %pre ], [ 0, %bb_main ]
  %acc.in = phi i32 [ 0, %pre ], [ 0, %bb_main ]
  %start = zext nneg i32 %start.i32 to i64
  %scale = shl nuw nsw i64 %start, 2, !revenant.va !5
  br label %loop, !revenant.va !6

loop:                                             ; preds = %loop, %bb_tail
  %iv = phi i64 [ 0, %bb_tail ], [ %next, %loop ]
  %sum = phi i32 [ %acc.in, %bb_tail ], [ %sum.next, %loop ]
  %scaled = mul i32 %sum, 31
  %address = getelementptr i8, ptr %base, i64 %scale, !revenant.va !7
  %addr = getelementptr i32, ptr %address, i64 %iv, !revenant.va !8
  %value = load i32, ptr %addr, align 1, !revenant.va !8
  %sum.next = add i32 %scaled, %value, !revenant.va !8
  %next = add i64 %iv, 1, !revenant.va !9
  %done = icmp eq i64 %next, %wide, !revenant.va !10
  br i1 %done, label %common.ret, label %loop, !revenant.va !11

common.ret:                                       ; preds = %loop, %entry
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result, !revenant.va !12
}

; CHECK-LABEL: define i32 @hash_vec(
; CHECK: %iv.idx = phi i32 [ 0, %bb_tail ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %address, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %rem
; CHECK: br i1 %done.idx, label %loop, label %common.ret

!1 = !{i64 1500}
!2 = !{i64 1501}
!3 = !{i64 1502}
!4 = !{i64 1503}
!5 = !{i64 1504}
!6 = !{i64 1505}
!7 = !{i64 1506}
!8 = !{i64 1507}
!9 = !{i64 1508}
!10 = !{i64 1509}
!11 = !{i64 1510}
!12 = !{i64 1511}
