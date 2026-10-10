; RUN: %revenant-opt -passes=revenant-univ,verify -S %s | %FileCheck %s

; Reduced pointer-increment byte-loop shape from
; target/parity/c-clang-o2/artifacts/main.post.ll @copy_bytes.
; The unguarded form is left alone: without a proof that `%n` is
; non-negative, rewriting the equality exit into a signed compare would
; change the trip count.
define void @copy_one(ptr %dst, ptr %src, i32 %n) {
entry:
  br label %loop
loop:
  %count = phi i32 [ 0, %entry ], [ %count.next, %loop ]
  %src.cur = phi ptr [ %src, %entry ], [ %src.next, %loop ]
  %dst.cur = phi ptr [ %dst, %entry ], [ %dst.next, %loop ]
  %byte = load i8, ptr %src.cur, align 1
  store i8 %byte, ptr %dst.cur, align 1
  %src.next = getelementptr i8, ptr %src.cur, i32 1
  %dst.next = getelementptr i8, ptr %dst.cur, i32 1
  %count.next = add i32 %count, 1
  %done = icmp eq i32 %count.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @copy_one(
; CHECK-NOT: .idx
; CHECK: %done = icmp eq i32 %count.next, %n

; The same body behind the `%n < 1` guard the lifter emits: the guard proves
; `%n >= 1`, so the signed exit preserves the trip count and the loop does
; transform.
define void @copy_one_guarded(ptr %dst, ptr %src, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %loop

loop:
  %count = phi i32 [ 0, %entry ], [ %count.next, %loop ]
  %src.cur = phi ptr [ %src, %entry ], [ %src.next, %loop ]
  %dst.cur = phi ptr [ %dst, %entry ], [ %dst.next, %loop ]
  %byte = load i8, ptr %src.cur, align 1
  store i8 %byte, ptr %dst.cur, align 1
  %src.next = getelementptr i8, ptr %src.cur, i32 1
  %dst.next = getelementptr i8, ptr %dst.cur, i32 1
  %count.next = add i32 %count, 1
  %done = icmp eq i32 %count.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @copy_one_guarded(
; CHECK: %count.idx = phi i32 [ 0, %entry ], [ %count.next.idx, %loop ]
; CHECK: getelementptr inbounds i8, ptr %src, i32 %count.idx
; CHECK: getelementptr inbounds i8, ptr %dst, i32 %count.idx
; CHECK: %count.next.idx = add i32 %count.idx, 1
; CHECK: %done.idx = icmp slt i32 %count.next.idx, %n
; CHECK: br i1 %done.idx, label %loop, label %exit
