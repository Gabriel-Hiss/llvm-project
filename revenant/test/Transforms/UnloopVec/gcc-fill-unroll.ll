; Lifted source shape: c-gcc-o3 fill_vec in corpus/src/c/main.c:205-207.
; The x86 lifter expands an eight-element store group into scalar stores.
; Revenant may narrow the group stride only when the trip bound is a
; proven multiple of the old step (constant 32 here); a symbolic bound
; keeps the loop untouched (see uniform-store-nodiv.ll).
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define void @fill_unrolled(ptr %dst, i32 %value) {
entry:
  %empty = icmp eq i64 32, 0
  br i1 %empty, label %exit, label %loop

loop:
  %p = phi ptr [ %dst, %entry ], [ %p.next, %loop ]
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  store i32 %value, ptr %p, align 1
  %p1 = getelementptr i32, ptr %p, i64 1
  store i32 %value, ptr %p1, align 1
  %p2 = getelementptr i32, ptr %p, i64 2
  store i32 %value, ptr %p2, align 1
  %p3 = getelementptr i32, ptr %p, i64 3
  store i32 %value, ptr %p3, align 1
  %p4 = getelementptr i32, ptr %p, i64 4
  store i32 %value, ptr %p4, align 1
  %p5 = getelementptr i32, ptr %p, i64 5
  store i32 %value, ptr %p5, align 1
  %p6 = getelementptr i32, ptr %p, i64 6
  store i32 %value, ptr %p6, align 1
  %p7 = getelementptr i32, ptr %p, i64 7
  store i32 %value, ptr %p7, align 1
  %iv.next = add i64 %iv, 32
  %p.next = getelementptr i8, ptr %p, i64 32
  %done = icmp eq i64 %iv.next, 32
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; CHECK-LABEL: define void @fill_unrolled
; CHECK: loop:
; CHECK: store i32 %value, ptr %p
; CHECK-NOT: store i32 %value
; CHECK: %iv.next = add i64 %iv, 4
; CHECK: %p.next = getelementptr i8, ptr %p, i64 4
; CHECK: %done = icmp eq i64 %iv.next, 32
; CHECK: }
