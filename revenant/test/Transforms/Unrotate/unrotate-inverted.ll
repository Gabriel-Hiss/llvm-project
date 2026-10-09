; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnrotate.dll -passes=revenant-unrotate -S %s | FileCheck %s

; Inverted guard: `if (x >= n) goto X; do { i++ } while (i < n)` still
; de-rotates (guard is the predicate inverse of the latch at init).
define void @inv(i32 %x, i32 %n) {
; CHECK-LABEL: define void @inv(
G:
  %c0 = icmp sge i32 %x, %n, !revenant.va !{i64 4096}
  br i1 %c0, label %X, label %L
L:
; CHECK: L:
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
