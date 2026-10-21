; RUN: %revenant-opt -passes=revenant-untailrec -verify-each -S %s | %FileCheck %s

; Self-loop header that splitSelfLoop splits, but the rewrite vetoes: the
; exit path stores the header's pointer phi and the call returns an i32,
; so the recursion result cannot replace that use. The pass must keep the
; (now split) loop instead of corrupting the exit. -verify-each guards
; that the split leaves valid IR when the rewrite declines.
;
; CHECK-LABEL: define i32 @selfloop_ptr_exit(
; CHECK-NOT: call i32 @selfloop_ptr_exit
; CHECK: store ptr
; CHECK: ret i32
define i32 @selfloop_ptr_exit(ptr %p, i32 %n) {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %exit, label %loop
loop:
  %pp = phi ptr [ %p, %entry ], [ %pp.next, %loop ]
  %pn = phi i32 [ %n, %entry ], [ %pn.next, %loop ]
  %done = icmp eq i32 %pn, 0
  %pp.next = getelementptr i8, ptr %pp, i64 1
  %pn.next = sub i32 %pn, 1
  br i1 %done, label %exit2, label %loop
exit:
  ret i32 0
exit2:
  store ptr %pp, ptr @slot
  ret i32 0
}

@slot = global ptr null
