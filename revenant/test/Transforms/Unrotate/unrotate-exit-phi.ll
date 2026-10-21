; RUN: %revenant-opt -passes=revenant-unrotate -S %s | %FileCheck %s

; A guarded do-while whose exit block has a phi: rewriting G to jump
; straight into the loop would leave the exit phi with an entry for G,
; which is no longer a predecessor (invalid IR). The loop stays as is.
define i32 @live_out(i32 %x, i32 %n) {
; CHECK-LABEL: define i32 @live_out(
; CHECK: G:
; CHECK-NEXT: %c0 = icmp slt i32 %x, %n
; CHECK-NEXT: br i1 %c0, label %L, label %X
; CHECK-NOT: L.cond:
; CHECK: X:
; CHECK-NEXT: %r = phi i32 [ %x, %G ], [ %inext, %L ]
G:
  %c0 = icmp slt i32 %x, %n, !revenant.va !{i64 4096}
  br i1 %c0, label %L, label %X
L:
  %i = phi i32 [%x, %G], [%inext, %L]
  %c2 = icmp slt i32 %i, %n, !revenant.va !{i64 4100}
  %inext = add i32 %i, 1
  br i1 %c2, label %L, label %X
X:
  %r = phi i32 [%x, %G], [%inext, %L]
  ret i32 %r
}
