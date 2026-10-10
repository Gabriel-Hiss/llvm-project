; RUN: %revenant-opt -passes=revenant-unpeel -S %s | %FileCheck %s
; Pure peel shape: block %P holds the peeled first iteration of the loop
; body with the IV replaced by its initial value 0. The loop accumulates
; s = s + (i * 2); the peeled copy computes the i=0 step (0*2=0 added to
; the init 10) and the IV step 0+1. The pass sinks %P's phi feed into %G,
; routes %G straight to the header, and drops %P.
; CHECK-LABEL: define i32 @pl(

define i32 @pl(i32 %n) {
G:
  %c0 = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c0, label %P, label %X
P:
  %m0 = mul i32 0, 2, !revenant.va !{i64 4112}
  %s0 = add i32 10, %m0, !revenant.va !{i64 4113}
  %i0 = add i32 0, 1, !revenant.va !{i64 4114}
  br label %H
H:
  %i = phi i32 [ %i0, %P ], [ %in, %H ]
  %s = phi i32 [ %s0, %P ], [ %sa, %H ]
  %m = mul i32 %i, 2, !revenant.va !{i64 4128}
  %sa = add i32 %s, %m, !revenant.va !{i64 4129}
  %in = add i32 %i, 1, !revenant.va !{i64 4130}
  %c = icmp slt i32 %in, %n, !revenant.va !{i64 4131}
  br i1 %c, label %H, label %X
; CHECK-NOT: P:
; CHECK: br i1 %c0, label %H, label %X
X:
  %r = phi i32 [ 10, %G ], [ %sa, %H ]
  ret i32 %r
}
