; RUN: %revenant-opt -passes=revenant-reroll -S %s | %FileCheck %s
; Negative: vectorized fixed-trip init (GCC O3 AVX2 arrays, sub_140001C40
; shape: splat/mul by <0..7> plus one vector stack store). No scalar
; isomorphic windows exist, so no loop may be synthesized.

define i32 @arrays_splat(i32 %0) {
entry:
  %frame = alloca [344 x i8], align 1, !revenant.va !0
  %.splatinsert = insertelement <8 x i32> poison, i32 %0, i64 0, !revenant.va !1
  %.splat = shufflevector <8 x i32> %.splatinsert, <8 x i32> poison, <8 x i32> zeroinitializer, !revenant.va !1
  %4 = mul <8 x i32> %.splat, <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7>, !revenant.va !2
  %fp41 = getelementptr inbounds nuw i8, ptr %frame, i64 8
  store <8 x i32> %4, ptr %fp41, align 1, !revenant.va !3
  %7 = load i32, ptr %fp41, align 1, !revenant.va !4
  ret i32 %7, !revenant.va !5
}

; CHECK: define i32 @arrays_splat
; CHECK-NOT: unroll:
; CHECK-NOT: phi i64
; CHECK: ret i32

!0 = !{i64 200}
!1 = !{i64 207}
!2 = !{i64 214}
!3 = !{i64 221}
!4 = !{i64 228}
!5 = !{i64 235}
