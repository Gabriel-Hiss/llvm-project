; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Irreducible two-entry cycle: a -> b -> a with neither dominating the
; other, plus entry edges. LoopInfo sees no natural loop, the region walk
; recurses until the budget guard, and the function falls back to flat
; output: every annotation cleared (the "goto" contract marker path is a
; function-level flat fallback).
define i32 @irred(i32 %a) {
entry:
  %c = icmp eq i32 %a, 0
  br i1 %c, label %a_blk, label %b_blk
a_blk:
  %x = add i32 %a, 1
  br label %b_blk
b_blk:
  %c2 = icmp sgt i32 %a, 10
  br i1 %c2, label %a_blk, label %exit
exit:
  ret i32 %a
}
; The walk must not hang or annotate a coherent structure that isn't one:
; expect no !revenant.region terminator descriptors left.
; CHECK-LABEL: @irred(
; CHECK-NOT: !{!"while"
; CHECK-NOT: !{!"dowhile"
; CHECK-NOT: !{!"for"
; CHECK-NOT: !{!"if"
; CHECK-NOT: !{!"switch"
; CHECK: ret i32
