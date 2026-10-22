; Clang SLP dot-product (c-clang-o2 shape): two i128 loads hold four packed
; i32 elements each; i128 bitcasts reinterpret them as <4 x i32> and
; <2 x i64> lanes, lane-wise masks/multiplies form products, shuffles
; gather them, and a pure-shuffle/add horizontal fold reduces to lane 0.
; Source (corpus/src/c/main.c dot4): s += a[i]*b[i] over 4 iterations.
; Real lifted IR: deopt-board/work/rec-c-clang-o2.c.ir-clean.ll#sub_140001AC0.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @dot4_clang(ptr %0, ptr %1) {
entry:
  %2 = load i128, ptr %0, align 1
  %3 = load i128, ptr %1, align 1
  %4 = bitcast i128 %3 to <4 x i32>
  %5 = shufflevector <4 x i32> %4, <4 x i32> poison, <4 x i32> <i32 1, i32 1, i32 3, i32 3>
  %6 = bitcast i128 %3 to <2 x i64>
  %7 = bitcast i128 %2 to <2 x i64>
  %8 = and <2 x i64> %7, splat (i64 4294967295)
  %9 = and <2 x i64> %6, splat (i64 4294967295)
  %10 = mul nuw <2 x i64> %9, %8
  %11 = bitcast <2 x i64> %10 to <4 x i32>
  %12 = shufflevector <4 x i32> %11, <4 x i32> poison, <4 x i32> <i32 0, i32 2, i32 poison, i32 poison>
  %13 = bitcast i128 %2 to <4 x i32>
  %14 = shufflevector <4 x i32> %13, <4 x i32> poison, <4 x i32> <i32 1, i32 1, i32 3, i32 3>
  %15 = bitcast <4 x i32> %5 to <2 x i64>
  %16 = bitcast <4 x i32> %14 to <2 x i64>
  %17 = and <2 x i64> %16, splat (i64 4294967295)
  %18 = and <2 x i64> %15, splat (i64 4294967295)
  %19 = mul nuw <2 x i64> %18, %17
  %20 = bitcast <2 x i64> %19 to <4 x i32>
  %21 = shufflevector <4 x i32> %20, <4 x i32> poison, <4 x i32> <i32 0, i32 2, i32 poison, i32 poison>
  %22 = shufflevector <4 x i32> %12, <4 x i32> %21, <4 x i32> <i32 0, i32 4, i32 1, i32 5>
  %23 = shufflevector <4 x i32> %22, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 2, i32 3>
  %24 = add <4 x i32> %23, %22
  %25 = shufflevector <4 x i32> %24, <4 x i32> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>
  %26 = add <4 x i32> %25, %24
  %27 = extractelement <4 x i32> %26, i64 0
  ret i32 %27
}

; The whole web scalarizes: no vector or i128 types survive.
; CHECK-NOT: <2 x i64>
; CHECK-NOT: <4 x i32>
; CHECK-NOT: load i128
; CHECK-NOT: bitcast i128
; Scalar products feed one scalar reduction chain.
; CHECK: mul i64
; CHECK: add i32
; CHECK: ret i32
