; Nonzero IV start off the old-step lattice: the loop strides 32 from 5 against a constant bound of 32, so narrowing the step would drop stores. The pass must leave the loop untouched.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define void @write_offset(ptr %dst, i64 %n) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 5, %entry ], [ %iv.next, %loop ]
  %p = phi ptr [ %dst, %entry ], [ %p.next, %loop ]
  store i32 42, ptr %p, align 4
  %p1 = getelementptr i32, ptr %p, i64 1
  store i32 42, ptr %p1, align 4
  %iv.next = add i64 %iv, 32
  %p.next = getelementptr i8, ptr %p, i64 32
  %more = icmp ult i64 %iv.next, 32
  br i1 %more, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: define void @write_offset
; CHECK: loop:
; CHECK: store i32 42, ptr %p
; CHECK: store i32 42
; CHECK: %iv.next = add i64 %iv, 32
; CHECK: %p.next = getelementptr i8, ptr %p, i64 32
; CHECK: }
