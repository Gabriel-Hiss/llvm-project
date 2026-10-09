; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unhotcold -S %s | %FileCheck %s

; PGO branch inversion: cjson-cl-o2pgo shape. Weights 1:99 say the false
; edge dominates, but layout has the true successor (cold) first. After
; the pass the condition is inverted and the hot block is the true
; successor and the layout fallthrough.
;
; Source order (-O0 lowering, cf. main.pre.ll `cmp_signed`: true edge =
; then-block): `if (x) hot(); else cold();` with hot() inlined as the
; fallthrough.

define void @pgo_branch(i1 %x) {
; CHECK-LABEL: define void @pgo_branch(
entry:
  br i1 %x, label %cold, label %hot, !prof !{!"branch_weights", i32 1, i32 99}, !revenant.va !{i64 4096}
; CHECK: br i1 {{.*}}, label %hot, label %cold

cold:
  call void @cold_work(), !revenant.va !{i64 8388608}
  br label %exit, !revenant.va !{i64 8388612}

hot:
  call void @hot_work(), !revenant.va !{i64 4100}
  br label %exit, !revenant.va !{i64 4104}

exit:
  ret void
}

declare void @hot_work()
declare void @cold_work()
