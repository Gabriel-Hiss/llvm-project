; Straight-line SLP dot-product (GCC shape): two <2 x i64> loads hold four
; packed i32 elements; shifts/masks split each i64 lane into low/high i32,
; lane-wise multiplies form four products, shuffles gather them into one
; <4 x i32>, and a manual i128 shift/add horizontal fold reduces to lane 0.
; Source (corpus/src/c/main.c dot4): s += a[i]*b[i] over 4 iterations.
; Real lifted IR: deopt-board/work/pre-c-gcc-o2.ll#sub_140001E50 (same shape
; in pre-c-gcc-o3.ll#sub_140002320).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @dot4_fold(ptr %0, ptr %1) {
entry:
  %2 = load <2 x i64>, ptr %0, align 1
  %3 = load <2 x i64>, ptr %1, align 1
  %4 = lshr <2 x i64> %2, splat (i64 32)
  %5 = and <2 x i64> %3, splat (i64 4294967295)
  %6 = and <2 x i64> %2, splat (i64 4294967295)
  %7 = mul nuw <2 x i64> %6, %5
  %8 = lshr <2 x i64> %3, splat (i64 32)
  %9 = mul nuw <2 x i64> %4, %8
  %10 = bitcast <2 x i64> %7 to <4 x i32>
  %11 = shufflevector <4 x i32> %10, <4 x i32> poison, <4 x i32> <i32 0, i32 2, i32 poison, i32 poison>
  %12 = bitcast <2 x i64> %9 to <4 x i32>
  %13 = shufflevector <4 x i32> %12, <4 x i32> poison, <4 x i32> <i32 0, i32 2, i32 poison, i32 poison>
  %14 = shufflevector <4 x i32> %11, <4 x i32> %13, <4 x i32> <i32 0, i32 4, i32 1, i32 5>
  %15 = bitcast <4 x i32> %14 to i128
  %16 = lshr i128 %15, 64
  %17 = bitcast i128 %16 to <4 x i32>
  %18 = add <4 x i32> %14, %17
  %19 = bitcast <4 x i32> %18 to i128
  %20 = lshr i128 %19, 32
  %21 = bitcast i128 %20 to <4 x i32>
  %22 = add <4 x i32> %18, %21
  %23 = extractelement <4 x i32> %22, i64 0
  ret i32 %23
}

; The whole web scalarizes: no vector or i128 types survive.
; CHECK-NOT: <2 x i64>
; CHECK-NOT: <4 x i32>
; CHECK-NOT: i128
; The 64-bit lane products truncate to i32 and feed one scalar add chain.
; CHECK: mul i64
; CHECK: add i32
; CHECK: ret i32

; AVX2 variant (c-gcc-o3avx2 sub_140002560): the loads are already <4 x i32>,
; so no cross-width split is needed; the same i128 fold reduces the products.
define i32 @dot4_fold_avx2(ptr %0, ptr %1) {
entry:
  %a = load <4 x i32>, ptr %0, align 1
  %b = load <4 x i32>, ptr %1, align 1
  %m = mul <4 x i32> %a, %b
  %f = bitcast <4 x i32> %m to i128
  %s = lshr i128 %f, 64
  %g = bitcast i128 %s to <4 x i32>
  %h = add <4 x i32> %m, %g
  %f2 = bitcast <4 x i32> %h to i128
  %s2 = lshr i128 %f2, 32
  %g2 = bitcast i128 %s2 to <4 x i32>
  %r = add <4 x i32> %h, %g2
  %e = extractelement <4 x i32> %r, i64 0
  ret i32 %e
}

; CHECK-LABEL: define i32 @dot4_fold_avx2
; CHECK-NOT: <4 x i32>
; CHECK-NOT: i128
; CHECK: mul i32
; CHECK: add i32
; CHECK: ret i32
