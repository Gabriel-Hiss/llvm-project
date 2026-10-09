; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUniv.dll -passes=revenant-univ,verify -S %s | FileCheck %s

; Shape from lifted c-clang-o2 `sum_vec` scalar tail (VA 0x140001680). The
; tail merges three entries: the direct entry (index 0), the vector-main-loop
; exit (index `%n & ~7`, truncated into the new index in that predecessor),
; and its own latch.
define i32 @sum_vec(ptr %base, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %common.ret, label %pre, !revenant.va !1

pre:
  %wide = zext nneg i32 %n to i64, !revenant.va !2
  %big = icmp ugt i32 %n, 7
  br i1 %big, label %bb_vec, label %tail, !revenant.va !3

bb_vec:                                           ; preds = %pre
  %rounded = and i32 %n, 2147483640, !revenant.va !4
  %vec.start = zext nneg i32 %rounded to i64, !revenant.va !4
  br label %tail, !revenant.va !5

tail:                                             ; preds = %bb_vec, %pre, %tail
  %iv = phi i64 [ 0, %pre ], [ %vec.start, %bb_vec ], [ %next, %tail ]
  %sum = phi i32 [ 0, %pre ], [ 0, %bb_vec ], [ %sum.next, %tail ]
  %addr = getelementptr i32, ptr %base, i64 %iv, !revenant.va !6
  %value = load i32, ptr %addr, align 1, !revenant.va !6
  %sum.next = add i32 %sum, %value, !revenant.va !6
  %next = add i64 %iv, 1, !revenant.va !7
  %done = icmp eq i64 %next, %wide, !revenant.va !8
  br i1 %done, label %common.ret, label %tail, !revenant.va !9

common.ret:                                       ; preds = %tail, %entry
  %result = phi i32 [ 0, %entry ], [ %sum.next, %tail ]
  ret i32 %result, !revenant.va !10
}

; CHECK-LABEL: define i32 @sum_vec(
; CHECK: %iv.start = trunc i64 %vec.start to i32
; CHECK: %iv.idx = phi i32 [ 0, %pre ], [ %iv.start, %bb_vec ], [ %next.idx, %tail ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %base, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %tail, label %common.ret

; Shape from lifted c-o2 (MSVC) `map_call_vec`/`hash_vec`/`copy_vec`: the
; touched GEP indexes a widening of the narrow index, the exact -O0 form.
; Univ folds the widening into a narrow GEP and inverts the exit compare.
define i32 @hash_vec_msvc(ptr %base, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %loop, !revenant.va !1

loop:                                             ; preds = %loop, %entry
  %iv = phi i32 [ 0, %entry ], [ %next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  %iv.wide = zext i32 %iv to i64, !revenant.va !2
  %addr = getelementptr i32, ptr %base, i64 %iv.wide, !revenant.va !3
  %value = load i32, ptr %addr, align 1, !revenant.va !3
  %scaled = mul i32 %sum, 31, !revenant.va !4
  %sum.next = add i32 %scaled, %value, !revenant.va !4
  %next = add i32 %iv, 1, !revenant.va !5
  %done = icmp eq i32 %next, %n, !revenant.va !6
  br i1 %done, label %exit, label %loop, !revenant.va !7

exit:                                             ; preds = %loop, %entry
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result, !revenant.va !8
}

; CHECK-LABEL: define i32 @hash_vec_msvc(
; CHECK: %iv.idx = phi i32 [ 0, %entry ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr %base, i32 %iv.idx, !revenant.va {{.*}}
; CHECK: %value = load i32, ptr %addr.idx
; CHECK: %next.idx = add i32 %iv.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

!1 = !{i64 1600}
!2 = !{i64 1601}
!3 = !{i64 1602}
!4 = !{i64 1603}
!5 = !{i64 1604}
!6 = !{i64 1605}
!7 = !{i64 1606}
!8 = !{i64 1607}
!9 = !{i64 9009}
!10 = !{i64 9010}
