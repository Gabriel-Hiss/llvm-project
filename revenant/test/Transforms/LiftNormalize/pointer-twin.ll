; A register that walks memory in a loop gets a pointer phi for its address
; uses. When the integer phi is left feeding only its own step and the
; end-pointer compare, the compare takes the pointers and the integer web
; goes: one loop variable, as in the source (NodeGraph::hasConnection).
; RUN: %revenant-opt -passes=revenant-lift-normalize -S %s | %FileCheck %s

; CHECK-LABEL: define i64 @count(
; CHECK: loop:
; CHECK-NEXT: %rax.0.ptr = phi ptr [ %begin.ptr, %entry ], [ %[[STEP:[a-z0-9]+]], %loop ]
; CHECK-NOT: phi i64 [ %begin
; CHECK: load i32, ptr %rax.0.ptr
; CHECK: %[[STEP]] = getelementptr i8, ptr %rax.0.ptr, i64 8
; CHECK-NEXT: %[[END:[a-z0-9]+]] = getelementptr i8, ptr %begin.ptr, i64 %bytes
; CHECK-NEXT: %done = icmp eq ptr %[[STEP]], %[[END]]
; CHECK-NEXT: br i1 %done
; CHECK: ret i64

define i64 @count(ptr %begin.ptr, i64 %n) {
entry:
  %begin = ptrtoint ptr %begin.ptr to i64
  %bytes = shl i64 %n, 3
  %end = add i64 %begin, %bytes
  br label %loop

loop:
  %rax.0 = phi i64 [ %begin, %entry ], [ %next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc1, %loop ]
  %p = inttoptr i64 %rax.0 to ptr
  %v = load i32, ptr %p, align 4
  %w = zext i32 %v to i64
  %acc1 = add i64 %acc, %w
  %next = add i64 %rax.0, 8
  %done = icmp eq i64 %next, %end
  br i1 %done, label %exit, label %loop

exit:
  ret i64 %acc1
}

; A twin whose integer value is also stored keeps both webs.
; CHECK-LABEL: define void @escape(
; CHECK-DAG: %rax.0 = phi i64
; CHECK-DAG: %rax.0.ptr = phi ptr
; CHECK: icmp eq i64 %next, %end

define void @escape(ptr %begin.ptr, i64 %end, ptr %out) {
entry:
  %begin = ptrtoint ptr %begin.ptr to i64
  br label %loop

loop:
  %rax.0 = phi i64 [ %begin, %entry ], [ %next, %loop ]
  %p = inttoptr i64 %rax.0 to ptr
  store i32 0, ptr %p, align 4
  store i64 %rax.0, ptr %out, align 8
  %next = add i64 %rax.0, 4
  %done = icmp eq i64 %next, %end
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; Twins an earlier run left, across two phis: the integer web (both phis
; and both steps) goes, and the end test compares pointers.
; CHECK-LABEL: define i64 @chain(
; CHECK-NOT: phi i64
; CHECK: %r.1.ptr = phi ptr
; CHECK-NEXT: %[[END:[a-z0-9.]+]] = inttoptr i64 %end to ptr
; CHECK-NEXT: %done = icmp uge ptr %r.1.ptr, %[[END]]
; CHECK-NOT: add i64 %r

define i64 @chain(ptr %b.ptr, i64 %end, i1 %f) {
entry:
  %b = ptrtoint ptr %b.ptr to i64
  br label %head

head:
  %r.0 = phi i64 [ %b, %entry ], [ %r.1, %latch ]
  %r.0.ptr = phi ptr [ %b.ptr, %entry ], [ %r.1.ptr, %latch ]
  %v = load i8, ptr %r.0.ptr, align 1
  %s = add i64 %r.0, 1
  %s.ptr = getelementptr i8, ptr %r.0.ptr, i64 1
  br i1 %f, label %skip, label %latch

skip:
  %t = add i64 %r.0, 2
  %t.ptr = getelementptr i8, ptr %r.0.ptr, i64 2
  br label %latch

latch:
  %r.1 = phi i64 [ %s, %head ], [ %t, %skip ]
  %r.1.ptr = phi ptr [ %s.ptr, %head ], [ %t.ptr, %skip ]
  %done = icmp uge i64 %r.1, %end
  br i1 %done, label %exit, label %head

exit:
  ret i64 0
}
