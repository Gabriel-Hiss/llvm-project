; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-untaildup -S %s | %FileCheck %s
; Loop peeling undo (pure shape): block %P holds the peeled first iteration
; of a pure loop body (same computation with the IV replaced by its initial
; value 0, so `add 0, 1` is the peeled IV step). The pass sinks %P's phi
; feed into %G, routes %G straight to the header, and drops %P.
; CHECK-LABEL: define i32 @pl(

define i32 @pl(i32 %n) {
G:
  %c0 = icmp sgt i32 %n, 0, !revenant.va !{i64 4096}
  br i1 %c0, label %P, label %X
P:
  %a0 = add i32 0, 1, !revenant.va !{i64 4112}
  %s0 = mul i32 %a0, 3, !revenant.va !{i64 4113}
  br label %H
H:
  %i = phi i32 [ %s0, %P ], [ %s, %H ]
  %a = add i32 %i, 1, !revenant.va !{i64 4128}
  %s = mul i32 %a, 3, !revenant.va !{i64 4129}
  %c = icmp slt i32 %s, %n, !revenant.va !{i64 4130}
  br i1 %c, label %H, label %X
; CHECK-NOT: P:
; CHECK: br i1 %c0, label %H, label %X
X:
  ret i32 0
}
