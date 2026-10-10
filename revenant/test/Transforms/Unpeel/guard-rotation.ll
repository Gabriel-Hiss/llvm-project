; RUN: %revenant-opt -passes=revenant-unpeel -S %s | %FileCheck %s
; Guard + do-while rotation: block %G guards the loop condition and
; branches to %H (enter) or %X (exit). The loop's latch tests the same
; condition. After the fold, G is deleted and the entry goes straight to H.
; CHECK-LABEL: define i32 @gr(

define i32 @gr(i32 %n) {
Entry:
  br label %G
G:
  %c = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c, label %H, label %X
H:
  %i = phi i32 [ 0, %G ], [ %in, %L ]
  %s = phi i32 [ 0, %G ], [ %sa, %L ]
  %sa = add i32 %s, 1, !revenant.va !{i64 4112}
  %in = add i32 %i, 1, !revenant.va !{i64 4113}
  %c2 = icmp slt i32 %in, %n, !revenant.va !{i64 4114}
  br i1 %c2, label %L, label %X
L:
  br label %H
; CHECK-NOT: G:
; CHECK: br label %H
X:
  %r = phi i32 [ 0, %G ], [ %sa, %H ]
  ret i32 %r
}
