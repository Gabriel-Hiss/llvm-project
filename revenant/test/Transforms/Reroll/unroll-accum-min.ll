; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s | %FileCheck %s

define i32 @sum2_simple(ptr %a, i32 %n) {
entry:
  %n2 = zext i32 %n to i64
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %acc0 = phi i32 [ 0, %entry ], [ %acc0.next, %loop ]
  %acc1 = phi i32 [ 0, %entry ], [ %acc1.next, %loop ]
  %a0 = getelementptr i32, ptr %a, i64 %i
  %x = load i32, ptr %a0, align 1
  %acc0.next = add i32 %acc0, %x
  %i1 = add i64 %i, 1
  %a1 = getelementptr i32, ptr %a, i64 %i1
  %y = load i32, ptr %a1, align 1
  %acc1.next = add i32 %acc1, %y
  %next = add i64 %i, 2
  %done = icmp eq i64 %next, %n2
  br i1 %done, label %exit, label %loop

exit:
  %s = add i32 %acc0, %acc1
  ret i32 %s
}

; CHECK-LABEL: define i32 @sum2_simple(
; CHECK: %ua_merge = phi i32 [ 0, %entry ], [ %ua_upd1, %loop ]
; CHECK: %next = add i64 %i, 2
; CHECK-NOT: %acc0.next
; CHECK-NOT: %acc1.next
