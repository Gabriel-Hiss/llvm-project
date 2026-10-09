; Fixture: the shape c-o2 `dot4` (VA 0x140001420) has AFTER revenant-reroll
; folds its 2x-unrolled body: one element per iteration, the i64 index
; stepping 1, the accumulators merged, and the countdown trip counter that
; MSVC keeps (start = the element count). Univ inverts the countdown on top,
; so the two passes compose to the source `for (i = 0; i < 4; i++)`.
define i32 @dot4(ptr %a, ptr %b) {
entry:
  br label %loop

loop:                                             ; preds = %loop, %entry
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %trip = phi i32 [ 4, %entry ], [ %trip.next, %loop ]
  %a.addr = getelementptr i32, ptr %a, i64 %i, !revenant.va !1
  %x = load i32, ptr %a.addr, align 1, !revenant.va !1
  %b.addr = getelementptr i32, ptr %b, i64 %i, !revenant.va !2
  %y = load i32, ptr %b.addr, align 1, !revenant.va !2
  %prod = mul i32 %x, %y, !revenant.va !3
  %acc.next = add i32 %acc, %prod, !revenant.va !4
  %next = add i64 %i, 1, !revenant.va !5
  %trip.next = add i32 %trip, -1
  %done = icmp eq i32 %trip.next, 0, !revenant.va !6
  br i1 %done, label %exit, label %loop, !revenant.va !7

exit:                                             ; preds = %loop
  ret i32 %acc.next, !revenant.va !8
}

; CHECK-LABEL: define i32 @dot4(
; CHECK: %i.idx = phi i32 [ 0, %entry ], [ %next.idx, %loop ]
; CHECK: %a.addr.idx = getelementptr inbounds i32, ptr %a, i32 %i.idx, !revenant.va !{{.*}}
; CHECK: %b.addr.idx = getelementptr inbounds i32, ptr %b, i32 %i.idx, !revenant.va !{{.*}}
; CHECK: %next.idx = add i32 %i.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, 4
; CHECK: br i1 %done.idx, label %loop, label %exit

; Same composition for the GCC `fill_vec` shape after un-peeling and folding:
; a 1-stride `ptr` loop whose trip counter is the countdown.
define void @fill_vec(ptr %dst, i32 %v, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %loop

loop:                                             ; preds = %loop, %entry
  %p = phi ptr [ %dst, %entry ], [ %p.next, %loop ]
  %trip = phi i32 [ %n, %entry ], [ %trip.next, %loop ]
  store i32 %v, ptr %p, align 1, !revenant.va !9
  %trip.next = add i32 %trip, -1
  %done = icmp eq i32 %trip.next, 0, !revenant.va !10
  %p.next = getelementptr i8, ptr %p, i64 4, !revenant.va !11
  br i1 %done, label %exit, label %loop, !revenant.va !12

exit:                                             ; preds = %loop, %entry
  ret void
}

; CHECK-LABEL: define void @fill_vec(
; CHECK: %trip.idx = phi i32 [ 0, %entry ], [ %trip.next.idx, %loop ]
; CHECK: %p.idx = getelementptr inbounds i32, ptr %dst, i32 %trip.idx
; CHECK: store i32 %v, ptr %p.idx, align 1
; CHECK: %trip.next.idx = add i32 %trip.idx, 1
; CHECK: %done.idx = icmp slt i32 %trip.next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

!1 = !{i64 2300}
!2 = !{i64 2301}
!3 = !{i64 2302}
!4 = !{i64 2303}
!5 = !{i64 2304}
!6 = !{i64 2305}
!7 = !{i64 2306}
!8 = !{i64 2307}
!9 = !{i64 2308}
!10 = !{i64 2309}
!11 = !{i64 2310}
!12 = !{i64 2311}
!13 = !{i64 2312}
