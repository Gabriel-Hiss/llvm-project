; RUN: %revenant-opt -passes=revenant-untailrec -verify-each -S %s | %FileCheck %s

; The alternate entry edge enters the latch and then reaches the loop header.
; This is an ordinary loop, not a tail-call-elimination loop.
; CHECK-LABEL: define i8 @ordinary_loop(
; CHECK: entry:
; CHECK: br i1 %entry.cond, label %header, label %latch
; CHECK: header:
; CHECK: %p.iter = phi ptr
; CHECK: %n.iter = phi i64
; CHECK-NOT: call i8 @ordinary_loop
; CHECK: ret i8 0

define i8 @ordinary_loop(ptr %p, i64 %n) {
entry:
  %entry.cond = icmp eq i64 %n, 0
  br i1 %entry.cond, label %header, label %latch
header:
  %p.iter = phi ptr [ %p, %entry ], [ %p.next, %latch ]
  %n.iter = phi i64 [ %n, %entry ], [ %n.next, %latch ]
  %done = icmp ne i64 %n.iter, 0
  br i1 %done, label %exit, label %latch
latch:
  %p.next = getelementptr i8, ptr %p, i64 1
  %n.next = sub i64 %n, 1
  br label %header
exit:
  ret i8 0
}
