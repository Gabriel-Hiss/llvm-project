; RUN: %revenant-opt -passes=revenant-devectorize,revenant-reroll,verify -S %s | %FileCheck %s
; Real rust-o3 lifted IR: corpus Rust `sum_slice_i32`, VA 0x140001370.
; The 16-element loop is vectorized and unrolled to four <4 x i32> loads,
; lane-wise adds, and a horizontal reduction. Devectorize first, then reroll
; the scalar reduction tree as one source-style loop.
;
; CHECK-LABEL: define i32 @sub_140001370(ptr %0)
; CHECK-NOT: <4 x i32>
; CHECK: unroll:
; CHECK: %u0i = phi i64 [ 0, %bb_140001370 ], [ %u0inext, %unroll ]
; CHECK: %u0acc = phi i32 [ 0, %bb_140001370 ], [ %u0next, %unroll ]
; CHECK: %u0addr = getelementptr i32, ptr %0, i64 %u0i
; CHECK: %u0v = load i32, ptr %u0addr, align 1
; CHECK: %u0next = add i32 %u0acc, %u0v
; CHECK-NOT: <4 x i32>
; CHECK: %u0inext = add i64 %u0i, 1
; CHECK: %u0c = icmp ult i64 %u0inext, 16
; CHECK: br i1 %u0c, label %unroll, label %unroll.exit
; CHECK: unroll.exit:
; CHECK: ret i32 %u0next

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@values = global [16 x i32] [i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15, i32 16], align 4
@overflow = global [16 x i32] [i32 2147483647, i32 1, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0], align 4

define i32 @sub_140001370(ptr %0) {
bb_140001370:
  %1 = load <4 x i32>, ptr %0, align 1
  %element1 = getelementptr <4 x i32>, ptr %0, i64 1
  %2 = load <4 x i32>, ptr %element1, align 1
  %element2 = getelementptr <4 x i32>, ptr %0, i64 2
  %3 = load <4 x i32>, ptr %element2, align 1
  %4 = add <4 x i32> %3, %1, !revenant.va !0
  %element = getelementptr <4 x i32>, ptr %0, i64 3
  %5 = load <4 x i32>, ptr %element, align 1
  %6 = add <4 x i32> %5, %2, !revenant.va !1
  %7 = add <4 x i32> %6, %4, !revenant.va !2
  %8 = shufflevector <4 x i32> %7, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 2, i32 3>, !revenant.va !3
  %9 = add <4 x i32> %8, %7, !revenant.va !4
  %10 = shufflevector <4 x i32> %9, <4 x i32> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>, !revenant.va !5
  %11 = add <4 x i32> %10, %9, !revenant.va !6
  %12 = extractelement <4 x i32> %11, i64 0
  ret i32 %12, !revenant.va !7
}

define i32 @main() {
entry:
  %sum = call i32 @sub_140001370(ptr @values)
  %ok = icmp eq i32 %sum, 136
  %wrapped = call i32 @sub_140001370(ptr @overflow)
  %wrap.ok = icmp eq i32 %wrapped, -2147483648
  %both = and i1 %ok, %wrap.ok
  %fail = xor i1 %both, true
  %ret = zext i1 %fail to i32
  ret i32 %ret
}

!0 = !{i64 5368714110}
!1 = !{i64 5368714119}
!2 = !{i64 5368714123}
!3 = !{i64 5368714127}
!4 = !{i64 5368714132}
!5 = !{i64 5368714136}
!6 = !{i64 5368714141}
!7 = !{i64 5368714149}
