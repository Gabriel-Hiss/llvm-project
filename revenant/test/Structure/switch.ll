; RUN: %{rv-structure-tool} %s | %{FileCheck} %s

; Switch with shared target (cases 0,2) and fallthrough-ish merge.
define i32 @sw(i32 %a) {
entry:
  switch i32 %a, label %def [
    i32 0, label %c02
    i32 1, label %c1
    i32 2, label %c02
  ]
c02:
  %x = add i32 %a, 10
  br label %merge
c1:
  %y = mul i32 %a, 2
  br label %merge
def:
  %z = or i32 %a, 255
  br label %merge
merge:
  %p = phi i32 [ %x, %c02 ], [ %y, %c1 ], [ %z, %def ]
  ret i32 %p
}
; entry=0 -> switch {merge=4}
; CHECK-LABEL: @sw(
; CHECK: bb_x0:
; CHECK:   switch i32 %a, label %bb_x3 [
; CHECK: ], !revenant.region ![[RS:[0-9]+]]

; A case that falls through to the merge directly (empty arm).
define i32 @sw_fall(i32 %a) {
entry:
  switch i32 %a, label %merge [
    i32 0, label %work
    i32 1, label %merge
  ]
work:
  %x = add i32 %a, 1
  br label %merge
merge:
  %p = phi i32 [ %x, %work ], [ %a, %entry ]
  ret i32 %p
}
; CHECK-LABEL: @sw_fall(
; CHECK:   switch i32 %a, label %bb_x2 [
; CHECK: ], !revenant.region ![[RSF:[0-9]+]]
; CHECK: ![[RS]] = !{!"switch", i32 4}
; CHECK: ![[RSF]] = !{!"switch", i32 2}
