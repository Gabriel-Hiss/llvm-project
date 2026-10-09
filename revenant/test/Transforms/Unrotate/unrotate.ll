; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-unrotate -S %s | FileCheck %s

; Mirrors unopt's unrotate_do_while: if (x < n) { do { i++ } while (i < n) }.
define void @t(i32 %x, i32 %n) {
; CHECK-LABEL: define void @t(
G:
  %c0 = icmp slt i32 %x, %n, !revenant.va !{i64 4096}
  br i1 %c0, label %L, label %X
L:
; CHECK: L:
; The latch keeps the cond chain and branches to the new body block.
; CHECK: %c2 = icmp slt i32 %i, %n
; CHECK: br i1 %c2, label %L.cond, label %X
  %i = phi i32 [%x, %G], [%inext, %L]
  %c2 = icmp slt i32 %i, %n, !revenant.va !{i64 4100}
  %inext = add i32 %i, 1
  br i1 %c2, label %L, label %X
; CHECK: L.cond:
; CHECK: br label %L
X:
  ret void
}
