; RUN: %revenant-opt -passes=revenant-untailrec -verify-each -S %s | %FileCheck %s

; A header whose only edge is the latch is an infinite loop, not the
; TailCallElim shape: the documented shape needs an edge that leaves the
; loop. The unconditional form and the both-edges-to-latch form must both
; be left alone.
;
; CHECK-LABEL: define i32 @no_exit(
; CHECK-NOT: call i32 @no_exit
; CHECK: ret i32
define i32 @no_exit(i32 %n) {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %exit, label %header
header:
  %pn = phi i32 [ %n, %entry ], [ %pn.next, %latch ]
  br label %latch
latch:
  %pn.next = sub i32 %pn, 1
  br label %header
exit:
  ret i32 0
}

; CHECK-LABEL: define i32 @both_to_latch(
; CHECK-NOT: call i32 @both_to_latch
; CHECK: ret i32
define i32 @both_to_latch(i32 %n) {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %exit, label %header
header:
  %pn = phi i32 [ %n, %entry ], [ %pn.next, %latch ]
  %done = icmp eq i32 %pn, 0
  br i1 %done, label %latch, label %latch
latch:
  %pn.next = sub i32 %pn, 1
  br label %header
exit:
  ret i32 0
}
