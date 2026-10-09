; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Simple if-else: entry(0) cbr -> then(1)/else(2), merge(3) postdominates.
define i32 @ifelse(i32 %a) {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %then, label %else
then:
  %x = add i32 %a, 1
  br label %merge
else:
  %y = sub i32 %a, 1
  br label %merge
merge:
  %p = phi i32 [ %x, %then ], [ %y, %else ]
  ret i32 %p
}
; CHECK-LABEL: @ifelse(
; CHECK: bb_x0:
; CHECK:   br i1 %{{.*}}, label %bb_x1, label %bb_x2, !revenant.region ![[R0:[0-9]+]]
; CHECK: bb_x1:
; CHECK: bb_x2:
; CHECK: bb_x3:
; CHECK-LABEL: @ifonly(
; CHECK: bb_x0:
; CHECK:   br i1 %{{.*}}, label %bb_x1, label %bb_x2, !revenant.region ![[R1:[0-9]+]]
; CHECK: ![[R0]] = !{!"if", i32 3}
; CHECK: ![[R1]] = !{!"if", i32 2}

; If without else: false arm is the merge itself.
define i32 @ifonly(i32 %a) {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %then, label %merge
then:
  %x = add i32 %a, 1
  br label %merge
merge:
  %p = phi i32 [ %x, %then ], [ %a, %entry ]
  ret i32 %p
}
