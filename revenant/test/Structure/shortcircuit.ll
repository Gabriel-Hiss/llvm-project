; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Short-circuit `a && b`: entry's true arm is a pure-cond block sharing the
; merge on its false edge -> entry gets "if", the folded block gets "sc".
define i32 @and_sc(i32 %a, i32 %b) {
entry:
  %c1 = icmp sgt i32 %a, 0
  br i1 %c1, label %cond2, label %merge
cond2:
  %c2 = icmp sgt i32 %b, 0
  br i1 %c2, label %then, label %merge
then:
  %x = add i32 %a, %b
  br label %merge
merge:
  %p = phi i32 [ %x, %then ], [ 0, %entry ], [ 0, %cond2 ]
  ret i32 %p
}
; entry=0, cond2=1, then=2, merge=3
; cond2 is folded: no "if" on it, "sc" pointing at partner entry(0).
; CHECK-LABEL: @and_sc(
; CHECK: bb_x0:
; CHECK:   br i1 %{{.*}}, label %bb_x1, label %bb_x3, !revenant.region ![[RIF:[0-9]+]]
; CHECK: bb_x1:
; CHECK:   br i1 %{{.*}}, label %bb_x2, label %bb_x3, !revenant.region ![[RSC:[0-9]+]]

; Short-circuit `a || b`: false arm is the pure-cond block sharing the true
; target.
define i32 @or_sc(i32 %a, i32 %b) {
entry:
  %c1 = icmp sgt i32 %a, 0
  br i1 %c1, label %then, label %cond2
cond2:
  %c2 = icmp sgt i32 %b, 0
  br i1 %c2, label %then, label %merge
then:
  %x = add i32 %a, %b
  br label %merge
merge:
  %p = phi i32 [ %x, %then ], [ %a, %cond2 ]
  ret i32 %p
}
; entry=0, cond2=1, then=2, merge=3
; CHECK-LABEL: @or_sc(
; CHECK: bb_x0:
; CHECK:   br i1 %{{.*}}, label %bb_x2, label %bb_x1, !revenant.region ![[RIF2:[0-9]+]]
; CHECK: bb_x1:
; CHECK:   br i1 %{{.*}}, label %bb_x2, label %bb_x3, !revenant.region ![[RSC2:[0-9]+]]
; CHECK: ![[RIF]] = !{!"if", i32 3}
; CHECK: ![[RSC]] = !{!"sc", i32 0}
