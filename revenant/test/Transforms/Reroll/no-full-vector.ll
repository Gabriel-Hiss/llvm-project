; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Negative: vectorized reduction (MSVC AVX2 dot4, pre-c-o2avx2.ll
; sub_1400013C0 shape) is devectorizer territory, never a full unroll.
; Vector-typed windows must not synthesize a loop.

define i32 @dot4_vec(ptr %0, ptr %1) {
entry:
  %2 = load <4 x i32>, ptr %0, align 1, !revenant.va !0
  %3 = load <4 x i32>, ptr %1, align 1, !revenant.va !1
  %4 = mul <4 x i32> %2, %3, !revenant.va !2
  %5 = bitcast <4 x i32> %4 to i128, !revenant.va !3
  %6 = lshr i128 %5, 64, !revenant.va !4
  %7 = bitcast i128 %6 to <4 x i32>, !revenant.va !5
  %8 = add <4 x i32> %4, %7, !revenant.va !6
  %9 = bitcast <4 x i32> %8 to i128, !revenant.va !7
  %10 = lshr i128 %9, 32, !revenant.va !8
  %11 = bitcast i128 %10 to <4 x i32>, !revenant.va !9
  %12 = add <4 x i32> %8, %11, !revenant.va !10
  %13 = extractelement <4 x i32> %12, i64 0, !revenant.va !11
  ret i32 %13, !revenant.va !12
}

; CHECK: define i32 @dot4_vec
; CHECK-NOT: unroll:
; CHECK-NOT: phi i64
; CHECK: ret i32 %13

!0 = !{i64 100}
!1 = !{i64 107}
!2 = !{i64 114}
!3 = !{i64 121}
!4 = !{i64 128}
!5 = !{i64 135}
!6 = !{i64 142}
!7 = !{i64 149}
!8 = !{i64 156}
!9 = !{i64 163}
!10 = !{i64 170}
!11 = !{i64 177}
!12 = !{i64 184}
