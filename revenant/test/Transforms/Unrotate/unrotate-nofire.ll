; RUN: %revenant-opt -passes=revenant-unrotate -S %s | %FileCheck %s --allow-unused-prefixes

; Guard/latch mismatch: no rewrite, CFG unchanged.
define void @nomatch(i32 %x, i32 %n, i32 %m) {
; CHECK-LABEL: define void @nomatch(
G:
  %c0 = icmp slt i32 %x, %m, !revenant.va !{i64 4096}
  br i1 %c0, label %L, label %X
L:
; CHECK: L:
; CHECK: br i1 %c2, label %L, label %X
  %i = phi i32 [%x, %G], [%inext, %L]
  %c2 = icmp slt i32 %i, %n, !revenant.va !{i64 4100}
  %inext = add i32 %i, 1
  br i1 %c2, label %L, label %X
X:
  ret void
}
