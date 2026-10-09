; RUN: %revenant-opt -passes=revenant-univ,verify -S %s | %FileCheck %s

; Shape from lifted c-gcc-o2 `copy_vec`/`add_vec` (VA 0x140001D00/0x140001D60):
; one i64 IV counts bytes, addresses both arrays through `getelementptr i8`,
; steps by 4 (the i32 element size), and exits on `icmp eq %next,
; shl (zext %n), 2` -- the bound is the byte length, divided back out to the
; source `%n`.
define void @copy_vec(ptr %dst, ptr %src, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %pre, !revenant.va !1

pre:
  %wide = zext nneg i32 %n to i64, !revenant.va !2
  %bytes = shl nuw nsw i64 %wide, 2, !revenant.va !3
  br label %loop, !revenant.va !4

loop:                                             ; preds = %loop, %pre
  %iv = phi i64 [ 0, %pre ], [ %next, %loop ]
  %src.addr = getelementptr i8, ptr %src, i64 %iv, !revenant.va !5
  %value = load i32, ptr %src.addr, align 1, !revenant.va !5
  %dst.addr = getelementptr i8, ptr %dst, i64 %iv, !revenant.va !6
  store i32 %value, ptr %dst.addr, align 1, !revenant.va !6
  %next = add i64 %iv, 4, !revenant.va !7
  %done = icmp eq i64 %next, %bytes, !revenant.va !8
  br i1 %done, label %exit, label %loop, !revenant.va !9

exit:                                             ; preds = %loop, %entry
  ret void, !revenant.va !10
}

; CHECK-LABEL: define void @copy_vec(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %src.addr.idx = getelementptr inbounds i32, ptr %src, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %dst.addr.idx = getelementptr inbounds i32, ptr %dst, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

; Shape from lifted c-gcc-o2 `sum_vec`/`hash_vec` (VA 0x140001CD0/0x140001E10):
; the loop threads a `ptr` PHI stepped by a 4-byte GEP plus an `i64` mirror
; PHI of the same stride, and exits on `icmp eq %mirror-step, %end` where
; `%end = %start + %n * 4`. The bound is the element count behind the end
; address.
define i64 @sum_vec(ptr %base, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %common.ret, label %pre, !revenant.va !1

pre:
  %base.bits = ptrtoint ptr %base to i64, !revenant.va !2
  %wide = zext nneg i32 %n to i64, !revenant.va !3
  %bytes = shl nuw nsw i64 %wide, 2, !revenant.va !4
  %end = add i64 %bytes, %base.bits, !revenant.va !5
  br label %loop, !revenant.va !6

loop:                                             ; preds = %loop, %pre
  %ptr = phi ptr [ %base, %pre ], [ %ptr.next, %loop ]
  %mirror = phi i64 [ %base.bits, %pre ], [ %mirror.next, %loop ]
  %sum = phi i32 [ 0, %pre ], [ %sum.next, %loop ]
  %value = load i32, ptr %ptr, align 1, !revenant.va !7
  %sum.next = add i32 %sum, %value, !revenant.va !7
  %mirror.next = add i64 %mirror, 4, !revenant.va !8
  %done = icmp eq i64 %mirror.next, %end, !revenant.va !9
  %ptr.next = getelementptr i8, ptr %ptr, i64 4, !revenant.va !8
  br i1 %done, label %bb_exit, label %loop, !revenant.va !10

bb_exit:                                          ; preds = %loop
  %sum.wide = zext i32 %sum.next to i64
  br label %common.ret, !revenant.va !11

common.ret:                                       ; preds = %bb_exit, %entry
  %result = phi i64 [ 0, %entry ], [ %sum.wide, %bb_exit ]
  ret i64 %result, !revenant.va !12
}

; CHECK-LABEL: define i64 @sum_vec(
; CHECK: %mirror.idx = phi i32 [ 0, %pre ], [ %mirror.next.idx, %loop ]
; CHECK: %ptr.idx = getelementptr inbounds i32, ptr %base, i32 %mirror.idx
; CHECK: %value = load i32, ptr %ptr.idx, align 1, !revenant.va {{.*}}
; CHECK: %mirror.next.idx = add i32 %mirror.idx, 1
; CHECK: %done.idx = icmp slt i32 %mirror.next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %bb_exit

; Shape from lifted c-gcc-o2 `copy_bytes` (VA 0x140001C60): genuinely byte
; elements, so the IV steps by 1 and the touch GEPs stay `i8`.
define i32 @copy_bytes(ptr %dst, ptr %src, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %common.ret, label %pre, !revenant.va !1

pre:
  %wide = zext nneg i32 %n to i64, !revenant.va !2
  br label %loop, !revenant.va !3

loop:                                             ; preds = %loop, %pre
  %iv = phi i64 [ 0, %pre ], [ %next, %loop ]
  %src.addr = getelementptr i8, ptr %src, i64 %iv, !revenant.va !4
  %value = load i8, ptr %src.addr, align 1, !revenant.va !4
  %dst.addr = getelementptr i8, ptr %dst, i64 %iv, !revenant.va !5
  store i8 %value, ptr %dst.addr, align 1, !revenant.va !5
  %next = add i64 %iv, 1, !revenant.va !6
  %done = icmp eq i64 %next, %wide, !revenant.va !7
  br i1 %done, label %common.ret, label %loop, !revenant.va !8

common.ret:                                       ; preds = %loop, %entry
  %result = phi i32 [ %n, %loop ], [ 0, %entry ]
  ret i32 %result, !revenant.va !9
}

; CHECK-LABEL: define i32 @copy_bytes(
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %src.addr.idx = getelementptr inbounds i8, ptr %src, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %dst.addr.idx = getelementptr inbounds i8, ptr %dst, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %common.ret

!1 = !{i64 1700}
!2 = !{i64 1701}
!3 = !{i64 1702}
!4 = !{i64 1703}
!5 = !{i64 1704}
!6 = !{i64 1705}
!7 = !{i64 1706}
!8 = !{i64 1707}
!9 = !{i64 1708}
!10 = !{i64 9010}
!11 = !{i64 9011}
!12 = !{i64 9012}
