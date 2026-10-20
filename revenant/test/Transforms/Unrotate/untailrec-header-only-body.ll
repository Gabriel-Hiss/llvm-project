; RUN: %revenant-opt -passes=revenant-untailrec -verify-each -S %s | %FileCheck %s

; Ordinary loop: the header holds only the loop-carried phis and branches to
; a body block that does the work, and the latch computes the next values
; (spdlog-gcc-o0 @sub_1400417D0). TailCallElim keeps the whole loop body in
; the header and branches from there to the latch, so this is not the shape
; the pass reverses. Rewriting it would drop the body and replace the
; header phis with the recursion result, which is not even type-correct:
; the body loads through the pointer phis.
;
; CHECK-LABEL: define i32 @header_only_body(
; CHECK-NOT: call i32 @header_only_body
; CHECK: ret i32
define i32 @header_only_body(ptr %a, ptr %b, i64 %n) {
entry:
  %c = icmp eq i64 %n, 0
  br i1 %c, label %pre, label %header
pre:
  %r = load i8, ptr %b
  %z = zext i8 %r to i32
  br label %exit
header:
  %pa = phi ptr [ %a, %entry ], [ %pa.next, %latch ]
  %pb = phi ptr [ %b, %entry ], [ %pb.next, %latch ]
  %pn = phi i64 [ %n, %entry ], [ %pn.next, %latch ]
  %done = icmp eq i64 %pn, 0
  br i1 %done, label %exit, label %body
body:
  %x = load i8, ptr %pb
  %y = load i8, ptr %pa
  %cmp = icmp slt i8 %x, %y
  br i1 %cmp, label %exit, label %latch
latch:
  %pa.next = getelementptr i8, ptr %pa, i64 1
  %pb.next = getelementptr i8, ptr %pb, i64 1
  %pn.next = add i64 %pn, -1
  br label %header
exit:
  %res = phi i32 [ %z, %pre ], [ 0, %header ], [ 1, %body ]
  ret i32 %res
}
