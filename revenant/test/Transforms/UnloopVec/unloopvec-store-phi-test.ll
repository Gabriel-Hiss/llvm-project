; IV tested instead of IV.next: the latch compares the raw IV against the bound, so the proven IV.next orientation does not hold. The pass must leave the loop untouched.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define void @write_iv_test(ptr %dst, i64 %n) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %p = phi ptr [ %dst, %entry ], [ %p.next, %loop ]
  store i32 42, ptr %p, align 4
  %p1 = getelementptr i32, ptr %p, i64 1
  store i32 42, ptr %p1, align 4
  %iv.next = add i64 %iv, 8
  %p.next = getelementptr i8, ptr %p, i64 8
  %more = icmp ult i64 %iv, 32
  br i1 %more, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: define void @write_iv_test
; CHECK: loop:
; CHECK: store i32 42, ptr %p
; CHECK: store i32 42
; CHECK: %iv.next = add i64 %iv, 8
; CHECK: %p.next = getelementptr i8, ptr %p, i64 8
; CHECK: }
