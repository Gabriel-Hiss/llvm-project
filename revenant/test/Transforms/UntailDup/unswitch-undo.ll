; RUN: %revenant-opt -passes=revenant-untaildup -S %s | %FileCheck %s
; Loop unswitching undo (single-block copies): one guard branch over two
; identical self-loop copies with the same exit. The pass folds them into one
; loop and deletes the guard.
; CHECK-LABEL: define i32 @w(

define i32 @w(i1 %c, i32 %n) {
entry:
  br i1 %c, label %H0, label %H1
H0:
  %i0 = phi i32 [ 0, %entry ], [ %j0, %H0 ]
  %k0 = icmp slt i32 %i0, %n, !revenant.va !{i64 4096}
  %j0 = add i32 %i0, 1, !revenant.va !{i64 4097}
  br i1 %k0, label %H0, label %X
H1:
  %i1 = phi i32 [ 0, %entry ], [ %j1, %H1 ]
  %k1 = icmp slt i32 %i1, %n, !revenant.va !{i64 4112}
  %j1 = add i32 %i1, 1, !revenant.va !{i64 4113}
  br i1 %k1, label %H1, label %X
X:
  ret i32 0
; CHECK: br label %H0
; CHECK-NOT: H1:
; CHECK: ret i32 0
}
