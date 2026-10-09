; A scalar float accumulator kept in the low lane of an xmm register:
; a <2 x double> phi web where every use reads lane 0 (extractelement 0)
; or forwards the vector (insertelement base, vector bitcast) scalarizes
; to a double phi. Ports crates/unopt lane0_scalar_acc.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @lane0_acc(ptr %p0, double %fl, i1 %c) {
entry:
  br label %L

L:
  %xv = phi <2 x double> [ zeroinitializer, %entry ], [ %xv, %L ]
  %e = extractelement <2 x double> %xv, i32 0
  %s = fadd double %e, %fl
  br i1 %c, label %L, label %X

X:
  ret void
}

; The vector phi is gone, replaced by a double phi keeping the name.
; CHECK: %xv.l0 = phi double [ undef, %entry ], [ %xv.l0, %L ]
; CHECK: %s = fadd double %xv.l0, %fl
; No vector ops survive in the loop.
; CHECK-NOT: <2 x double>
; CHECK: br i1 %c, label %L, label %X
