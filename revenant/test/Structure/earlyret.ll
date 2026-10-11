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
; CHECK: ![[EARLY]] = !{!"if", i32 -1}

