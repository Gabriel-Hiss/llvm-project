; RUN: %revenant-opt -passes=revenant-untailrec -verify-each -S %s | %FileCheck %s

; TailCallElim shape, but the loop exit stores a header pointer phi. The
; recursion result would replace that use, and the call returns an i32, so
; the store would take an integer where it needs a pointer. The pass must
; leave the function alone.
;
; CHECK-LABEL: define i32 @ptr_phi_exit(
; CHECK-NOT: call i32 @ptr_phi_exit
; CHECK: ret i32
define i32 @ptr_phi_exit(ptr %p, i32 %n) {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %exit, label %header
header:
  %pp = phi ptr [ %p, %entry ], [ %pp.next, %latch ]
  %pn = phi i32 [ %n, %entry ], [ %pn.next, %latch ]
  %done = icmp eq i32 %pn, 0
  br i1 %done, label %exit2, label %latch
latch:
  %pp.next = getelementptr i8, ptr %pp, i64 1
  %pn.next = sub i32 %pn, 1
  br label %header
exit:
  ret i32 0
exit2:
  store ptr %pp, ptr @slot
  ret i32 0
}

@slot = global ptr null
