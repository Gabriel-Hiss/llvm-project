; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Provenance survives the fold: !revenant.va on the kept instructions is
; preserved, and the rewritten step keeps the original VA node.

define i32 @sum_va(ptr %p0, i32 %n) !revenant.va !0 {
entry:
  ; Never reached with n == 0: with zero windows the unrolled body would run
  ; once and read two elements while the folded body reads one.
  %empty = icmp slt i32 %n, 1
  ; Exit on the step value against a bound that is a multiple of the
  ; 2-window step, so folding the step to 1 keeps the trip count.
  %scaled = shl i32 %n, 1
  br i1 %empty, label %X, label %L

L:
  %i = phi i32 [ 0, %entry ], [ %inext, %L ], !revenant.va !1
  %s = phi i32 [ 0, %entry ], [ %s2, %L ], !revenant.va !2
  %g0 = getelementptr i32, ptr %p0, i32 %i, !revenant.va !3
  %l0 = load i32, ptr %g0, align 4, !revenant.va !4
  %s1 = add i32 %s, %l0, !revenant.va !5
  %g1 = getelementptr i32, ptr %g0, i32 1, !revenant.va !6
  %l1 = load i32, ptr %g1, align 4, !revenant.va !7
  %s2 = add i32 %s1, %l1, !revenant.va !8
  %inext = add i32 %i, 2, !revenant.va !9
  %c = icmp ult i32 %inext, %scaled, !revenant.va !10
  br i1 %c, label %L, label %X

X:
  %r = phi i32 [ 0, %entry ], [ %s2, %L ]
  ret i32 %r
}


; A pre-check excludes n == 0 but does not prove n >= D (2). For n == 1
; the original executes once and reads two values; folding would execute
; twice and drop one load. Preserve the original loop and its provenance.
; CHECK: %g1 = getelementptr i32, ptr %g0, i32 1, !revenant.va !{{[0-9]+}}
; CHECK: %l1 = load i32, ptr %g1, align 4, !revenant.va !{{[0-9]+}}
; CHECK: %inext = add i32 %i, 2, !revenant.va !{{[0-9]+}}
; CHECK: %c = icmp ult i32 %inext, %scaled, !revenant.va !{{[0-9]+}}
; CHECK: %r = phi i32 [ 0, %entry ], [ %s2, %L ]
; CHECK: ret i32 %r
!0 = !{i64 4096}
!1 = !{i64 4100}
!2 = !{i64 4104}
!3 = !{i64 4108}
!4 = !{i64 4112}
!5 = !{i64 4116}
!6 = !{i64 4120}
!7 = !{i64 4124}
!8 = !{i64 4128}
!9 = !{i64 4132}
!10 = !{i64 4136}
