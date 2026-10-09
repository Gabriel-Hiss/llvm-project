; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Short range comparisons are ambiguous with source-level interval checks.
; Leave this two-value condition in branch form rather than guessing switch.
; CHECK-LABEL: define i32 @ranged(
; CHECK-NOT: switch
; CHECK: icmp ult i32 %sub, 2
; CHECK: br i1 %rc, label %dest, label %other
define i32 @ranged(i32 %s) {
entry:
  %sub = add i32 %s, -1, !revenant.va !{i64 4096}
  %rc = icmp ult i32 %sub, 2, !revenant.va !{i64 4097}
  br i1 %rc, label %dest, label %other

dest:
  ret i32 10

other:
  ret i32 -1
}
