; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUniv.dll -passes=revenant-univ,verify -S %s | FileCheck %s

; Shape from lifted c-clang-o2 `break_continue` (VA 0x140001150). Three
; indvars artifacts at once: the exit compares the already-narrow `%n`
; against the step, the loop has no memory touch, and the counter feeds the
; in-body magic division for `i % 3` (`zext` + `mul` + `icmp` uses of the
; index). The counter is already `i32`, so the value uses stay attached to
; the new index through the RAUW.
define i32 @break_continue(i32 %n) {
entry:
  %o = icmp slt i32 %n, 1
  br i1 %o, label %exit, label %loop, !revenant.va !1

loop:                                             ; preds = %body, %entry
  %i = phi i32 [ %next, %body ], [ 0, %entry ]
  %acc = phi i64 [ %acc1, %body ], [ 0, %entry ]
  %i.wide = zext i32 %i to i64
  %magic = mul nuw i64 %i.wide, 2863311531
  %shifted = lshr i64 %magic, 33
  %i.div3 = trunc nuw nsw i64 %shifted to i32
  %i.mod3 = mul i32 %i.div3, 3
  %cont = icmp eq i32 %i.mod3, %i, !revenant.va !2
  br i1 %cont, label %body, label %bb_break, !revenant.va !2

bb_break:                                         ; preds = %loop
  %big = icmp ugt i32 %i, 20, !revenant.va !3
  br i1 %big, label %exit, label %body, !revenant.va !4

body:                                             ; preds = %bb_break, %loop
  %acc.next = phi i64 [ %acc, %loop ], [ %acc, %bb_break ]
  %acc.i32 = trunc nuw i64 %acc.next to i32
  %sum = add i32 %i, %acc.i32, !revenant.va !5
  %acc1 = zext i32 %sum to i64, !revenant.va !5
  %next = add i32 %i, 1, !revenant.va !6
  %done = icmp eq i32 %n, %next, !revenant.va !2
  br i1 %done, label %exit, label %loop, !revenant.va !7

exit:                                             ; preds = %body, %bb_break, %entry
  %result = phi i64 [ 0, %entry ], [ %acc1, %body ], [ %acc, %bb_break ]
  %out = trunc i64 %result to i32
  ret i32 %out, !revenant.va !8
}

; CHECK-LABEL: define i32 @break_continue(
; CHECK: %i.idx = phi i32 [ 0, %entry ], [ %next.idx, %body ]
; CHECK: %i.wide = zext i32 %i.idx to i64
; CHECK: %cont = icmp eq i32 %i.mod3, %i.idx
; CHECK: %next.idx = add i32 %i.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

; Same shape with a widened (i64) counter bound by `zext`: the bound peels
; back to the i32 source value.
define i32 @break_continue_wide(i32 %n) {
entry:
  %o = icmp slt i32 %n, 1
  br i1 %o, label %exit, label %pre, !revenant.va !1

pre:
  %wide = zext nneg i32 %n to i64
  br label %loop, !revenant.va !2

loop:                                             ; preds = %loop, %pre
  %i = phi i64 [ 0, %pre ], [ %next, %loop ]
  %sum = phi i32 [ 0, %pre ], [ %sum.next, %loop ]
  %addr = getelementptr i32, ptr @g_array, i64 %i, !revenant.va !3
  %value = load i32, ptr %addr, align 4
  %sum.next = add i32 %sum, %value
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, %wide, !revenant.va !4
  br i1 %done, label %exit, label %loop, !revenant.va !5

exit:                                             ; preds = %loop, %entry
  %result = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  ret i32 %result, !revenant.va !6
}

; CHECK-LABEL: define i32 @break_continue_wide(
; CHECK: %i.idx = phi i32 [ 0, %pre ], [ %next.idx, %loop ]
; CHECK: %addr.idx = getelementptr inbounds i32, ptr @g_array, i32 %i.idx, !revenant.va {{.*}}
; CHECK: %next.idx = add i32 %i.idx, 1
; CHECK: %done.idx = icmp slt i32 %next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit

@g_array = external global [8 x i32]

!1 = !{i64 1200}
!2 = !{i64 1201}
!3 = !{i64 1202}
!4 = !{i64 1203}
!5 = !{i64 1204}
!6 = !{i64 1205}
!7 = !{i64 9007}
!8 = !{i64 9008}
