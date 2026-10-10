; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; while(i < n) { sum += i; i += 1 } — canonical IV, expects "for".
define i32 @loop_for(i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
  %sum = phi i32 [ 0, %entry ], [ %s2, %latch ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %s2 = add i32 %sum, %i
  br label %latch
latch:
  %inc = add i32 %i, 1
  br label %header
exit:
  ret i32 %sum
}
; header=1, latch=3, exit=4
; CHECK-LABEL: @loop_for(
; CHECK: bb_x1:
; CHECK:   br i1 %{{.*}}, label %bb_x2, label %bb_x4, !revenant.region ![[RF:[0-9]+]]

; while with a non-IV condition: expects "while".
define i32 @loop_while(i32 %n) {
entry:
  br label %header
header:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %body, label %exit
body:
  %n2 = sub i32 %n, 1
  br label %header
exit:
  ret i32 %n
}
; header=1, latch=2 (body -> header), exit=3
; CHECK-LABEL: @loop_while(
; CHECK: bb_x1:
; CHECK:   br i1 %{{.*}}, label %bb_x2, label %bb_x3, !revenant.region ![[RW:[0-9]+]]

; do { body } while (cond): header is a plain br; the latch tests.
define i32 @loop_dowhile(i32 %n) {
entry:
  br label %header
header:
  br label %body
body:
  %n2 = sub i32 %n, 1
  %c = icmp sgt i32 %n2, 0
  br i1 %c, label %header, label %exit
exit:
  ret i32 %n2
}
; header=1, latch=2 (body), exit=3
; CHECK-LABEL: @loop_dowhile(
; CHECK: bb_x1:
; CHECK:   br label %bb_x2, !revenant.region ![[RD:[0-9]+]]

; while(1) with break/continue inside: header cbr stays inside -> "loop".
define i32 @loop_bc(i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %inc, %cont ]
  %c1 = icmp eq i32 %i, 5
  br i1 %c1, label %brk, label %step
step:
  %c2 = icmp slt i32 %i, %n
  br i1 %c2, label %cont, label %brk
cont:
  %inc = add i32 %i, 1
  br label %header
brk:
  %c3 = icmp sgt i32 %i, 0
  br i1 %c3, label %exit, label %cont
exit:
  ret i32 %i
}
; CHECK-LABEL: @loop_bc(
; CHECK: bb_x1:
; CHECK:   br i1 %{{.*}}, label %bb_x4, label %bb_x2, !revenant.region ![[RL:[0-9]+]]

; Nested loops: inner header annotated with its own latch/exit.
define i32 @nested(i32 %n) {
entry:
  br label %outhdr
outhdr:
  %i = phi i32 [ 0, %entry ], [ %i2, %outlatch ]
  %co = icmp slt i32 %i, %n
  br i1 %co, label %inhdr, label %exit
inhdr:
  %j = phi i32 [ 0, %outhdr ], [ %j2, %inbody ]
  %ci = icmp slt i32 %j, %n
  br i1 %ci, label %inbody, label %outlatch
inbody:
  %j2 = add i32 %j, 1
  br label %inhdr
outlatch:
  %i2 = add i32 %i, 1
  br label %outhdr
exit:
  ret i32 %i
}
; outer: header=1 latch=4 exit=5 ; inner: header=2 latch=3 exit=4
; CHECK-LABEL: @nested(
; CHECK: bb_x1:
; CHECK:   br i1 %{{.*}}, label %bb_x2, label %bb_x5, !revenant.region ![[RO:[0-9]+]]
; CHECK: bb_x2:
; CHECK:   br i1 %{{.*}}, label %bb_x3, label %bb_x4, !revenant.region ![[RI:[0-9]+]]

; Break and continue paths inside a natural loop must not erase its region.
define i32 @loop_break_continue(i32 %n) {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
  %in.range = icmp slt i32 %i, %n
  br i1 %in.range, label %check.continue, label %exit
check.continue:
  %rem = urem i32 %i, 3
  %skip = icmp eq i32 %rem, 0
  br i1 %skip, label %latch, label %check.break
check.break:
  %past.limit = icmp sgt i32 %i, 20
  br i1 %past.limit, label %exit, label %body
body:
  %sum.body = add i32 %sum, %i
  br label %latch
latch:
  %sum.next = phi i32 [ %sum, %check.continue ], [ %sum.body, %body ]
  %i.next = add i32 %i, 1
  br label %head
exit:
  ret i32 %sum
}
; header=1, latch=5, exit=6
; CHECK-LABEL: @loop_break_continue(
; CHECK: bb_x1:
; CHECK: br i1 %{{.*}}, label %bb_x2, label %bb_x6, !revenant.region ![[RBC:[0-9]+]]
; CHECK: ![[RF]] = !{!"for", i32 1, i32 3, i32 4}
; CHECK: ![[RW]] = !{!"while", i32 1, i32 2, i32 3}
; CHECK: ![[RD]] = !{!"dowhile", i32 1, i32 2, i32 3}
; CHECK: ![[RL]] = !{![[RLLOOP:[0-9]+]], ![[RLIF:[0-9]+]]}
; CHECK: ![[RLLOOP]] = !{!"loop", i32 1, i32 3, i32 5}
; CHECK: ![[RO]] = !{!"for", i32 1, i32 4, i32 5}
; CHECK: ![[RI]] = !{!"for", i32 2, i32 3, i32 4}
; CHECK: ![[RBC]] = !{!"for", i32 1, i32 5, i32 6}
