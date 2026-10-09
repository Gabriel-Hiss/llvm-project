; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Switch-of-select (SimplifyCFG simplifySwitchOnSelect) back to a branch:
; `switch (select c, 10, 20)` with one case per arm becomes `br c`.

define i32 @swselect(i1 %c, i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @swselect(
; CHECK-NOT: switch
; CHECK: br i1 %c, label %bb1, label %bb2
entry:
  %s = select i1 %c, i32 10, i32 20, !revenant.va !{i64 4096}
  switch i32 %s, label %def [
    i32 10, label %bb1
    i32 20, label %bb2
  ]

bb1:
  ret i32 %a

bb2:
  ret i32 %b

def:
  ret i32 0
}
