; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Early-return diamond: the return arm is consumed by an if with no merge.
define i32 @earlyret(i32 %a) {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %retblock, label %follow
retblock:
  ret i32 7
follow:
  %x = add i32 %a, 1
  ret i32 %x
}
; CHECK-LABEL: @earlyret(
; CHECK: br i1 %{{.*}}, label %bb_x1, label %bb_x2, !revenant.region ![[EARLY:[0-9]+]]


; Both arms return; the two terminating blocks remain in the early-return if.
define i32 @earlyret_both(i32 %a) {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %r1, label %r2
r1:
  ret i32 1
r2:
  ret i32 2
}
; CHECK-LABEL: @earlyret_both(
; CHECK: br i1 %{{.*}}, label %bb_x1, label %bb_x2, !revenant.region ![[EARLY]]

; A conditional nested in the live arm gets a normal shared follow.
define i32 @earlyret_nested(i32 %a) {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %mid, label %follow
mid:
  %c2 = icmp slt i32 %a, 5
  br i1 %c2, label %retblock, label %follow
retblock:
  ret i32 7
follow:
  %x = add i32 %a, 1
  ret i32 %x
}
; The entry arm folds the pure-cond mid block; its nested if has follow and
; carries an sc relation back to the entry header.
; CHECK-LABEL: @earlyret_nested(
; CHECK: bb_x0:
; CHECK: br i1 %{{.*}}, label %bb_x1, label %bb_x3, !revenant.region ![[EARLY]]
; CHECK: bb_x1:
; CHECK: br i1 %{{.*}}, label %bb_x2, label %bb_x3, !revenant.region ![[NESTED2:[0-9]+]]
; Metadata definitions are emitted after the module body.
; CHECK-DAG: ![[EARLY]] = !{!"if", i32 -1}
; CHECK-DAG: ![[NESTED2]] = !{!"sc", i32 0}
