; A memcmp-like loop body with no anchor call must not rewrite: the
; hand-built c::memcmp pattern needs a memcmp/strcmp direct call, and a
; bare loop is not one. Extra code never creates a match.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-patterns -S %s | %FileCheck %s

define i64 @sum_loop(ptr %p, i64 %n) {
entry:
  br label %loop

loop:
  ; CHECK: phi i64
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %elt.ptr = getelementptr i64, ptr %p, i64 %i
  %elt = load i64, ptr %elt.ptr, align 8
  %acc.next = add i64 %acc, %elt
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ; CHECK: ret i64 %acc
  ; CHECK-NOT: __pat_
  ret i64 %acc
}
