; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Negative: genuinely different statements with matching opcodes must never
; re-roll. The stored values (mul by unrelated constants with no uniform
; step, leaves from different factors) are not one affine over one leaf.

define i32 @different_stmts(ptr %frame, i32 %a, i32 %b) {
entry:
  %g0 = getelementptr inbounds nuw i32, ptr %frame, i64 10, !revenant.va !0
  %m0 = mul i32 %a, 3, !revenant.va !0
  store i32 %m0, ptr %g0, align 1, !revenant.va !0
  %g1 = getelementptr inbounds nuw i32, ptr %frame, i64 11, !revenant.va !1
  %m1 = mul i32 %b, 7, !revenant.va !1
  store i32 %m1, ptr %g1, align 1, !revenant.va !1
  %g2 = getelementptr inbounds nuw i32, ptr %frame, i64 12, !revenant.va !2
  %m2 = mul i32 %a, 11, !revenant.va !2
  store i32 %m2, ptr %g2, align 1, !revenant.va !2
  %r = load i32, ptr %g2, align 1, !revenant.va !3
  ret i32 %r
}

; CHECK: define i32 @different_stmts
; CHECK-NOT: unroll:
; CHECK-NOT: phi i64
; CHECK: ret i32 %r

!0 = !{i64 300}
!1 = !{i64 307}
!2 = !{i64 314}
!3 = !{i64 321}
