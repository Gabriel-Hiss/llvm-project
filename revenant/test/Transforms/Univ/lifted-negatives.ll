; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUniv.dll -passes=revenant-univ,verify -S %s | FileCheck %s

; Shapes Univ deliberately leaves alone: un-unrolling and countdown elimination
; belong to the unroll pass, and a negative step is a countdown, not an index.

; Lifted c-clang-o2 `copy_vec` scalar tail (VA 0x140001710): the index steps
; +1 while a coupled trip counter steps -1 to zero.
define void @copy_vec_tail(ptr %dst, ptr %src, i32 %n) {
entry:
  %z = zext nneg i32 %n to i64
  %and = and i64 %z, 3
  %start = add i64 0, 0
  br label %loop

loop:                                             ; preds = %loop, %entry
  %trip = phi i64 [ %trip.next, %loop ], [ %and, %entry ]
  %index = phi i64 [ %index.next, %loop ], [ %start, %entry ]
  %src.addr = getelementptr i32, ptr %src, i64 %index, !revenant.va !1
  %value = load i32, ptr %src.addr, align 1
  %dst.addr = getelementptr i32, ptr %dst, i64 %index, !revenant.va !2
  store i32 %value, ptr %dst.addr, align 1
  %index.next = add i64 %index, 1
  %trip.next = add i64 %trip, -1
  %done = icmp eq i64 %trip.next, 0, !revenant.va !3
  br i1 %done, label %exit, label %loop, !revenant.va !4

exit:                                             ; preds = %loop
  ret void
}

; CHECK-LABEL: define void @copy_vec_tail(
; CHECK: %trip = phi i64 [ %trip.next, %loop ], [ %and, %entry ]
; CHECK: %trip.next = add i64 %trip, -1
; CHECK: %done = icmp eq i64 %trip.next, 0
; CHECK-NOT: .idx

; Lifted c-gcc-o2 `__do_global_ctors` region: the IV counts down to zero and
; the compare tests the PHI itself, so the trip count is not an index.
define void @dtors(ptr %table, i32 %count) {
entry:
  %start = add i32 %count, -1
  br label %loop

loop:                                             ; preds = %loop, %entry
  %index = phi i32 [ %start, %entry ], [ %index.next, %loop ]
  %zext = zext i32 %index to i64
  %slot = getelementptr i64, ptr %table, i64 %zext, !revenant.va !5
  %fn.raw = load i64, ptr %slot, align 8
  %fn = inttoptr i64 %fn.raw to ptr
  call void %fn()
  %index.next = add i32 %index, -1
  %done = icmp eq i32 %index, 0, !revenant.va !6
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; CHECK-LABEL: define void @dtors(
; CHECK: %index = phi i32 [ %start, %entry ], [ %index.next, %loop ]
; CHECK: %index.next = add i32 %index, -1
; CHECK: %done = icmp eq i32 %index, 0
; CHECK-NOT: .idx

; Lifted c-clang-o2 `hash_vec` main loop (VA 0x140001A20): 4x-unrolled, so the
; IV steps by 4 and feeds `shl` address math; the unroll pass owns this shape.
define i32 @hash_vec_main(ptr %base, i32 %n) {
entry:
  %wide = zext nneg i32 %n to i64
  br label %loop

loop:                                             ; preds = %loop, %entry
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %addr = getelementptr i32, ptr %base, i64 %iv, !revenant.va !7
  %value = load i32, ptr %addr, align 1
  %scaled = shl i64 %iv, 2
  %addr2 = getelementptr i8, ptr %base, i64 %scaled, !revenant.va !8
  %addr3 = getelementptr i32, ptr %addr2, i64 1
  %value2 = load i32, ptr %addr3, align 1
  %next = add i64 %iv, 4, !revenant.va !9
  %done = icmp eq i64 %next, %wide, !revenant.va !10
  br i1 %done, label %exit, label %loop

exit:                                             ; preds = %loop
  %sum = add i32 %value, %value2
  ret i32 %sum
}

; CHECK-LABEL: define i32 @hash_vec_main(
; CHECK: %next = add i64 %iv, 4
; CHECK: %done = icmp eq i64 %next, %wide
; CHECK-NOT: .idx

!1 = !{i64 2100}
!2 = !{i64 2101}
!3 = !{i64 2102}
!4 = !{i64 2103}
!5 = !{i64 2104}
!6 = !{i64 2105}
!7 = !{i64 2106}
!8 = !{i64 2107}
!9 = !{i64 2108}
!10 = !{i64 2109}
