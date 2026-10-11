; RUN: %revenant-opt -passes=revenant-devectorize,verify -S %s | %FileCheck %s --check-prefix=DEVEC
; Real lifted Rust rust-o3avx2 `sum_slice_i32`, VA 0x140001370, after the
; return-channel fix. The 256-bit reduction is split through `<2 x i128>`,
; then one i128 half is extracted and rebuilt as `<4 x i32>`.
; Promote the exact half round-trip before StraightDevec scalarizes the web.
;
; DEVEC-LABEL: define i32 @sub_140001370(ptr %0)
; DEVEC-NOT: load <8 x i32>
; DEVEC-NOT: add <8 x i32>
; DEVEC-NOT: shufflevector
; DEVEC-NOT: extractelement <2 x i128>
; DEVEC-NOT: bitcast i128
; DEVEC-NOT: <2 x i128>
; DEVEC: load i32
; DEVEC-NOT: load <8 x i32>
; DEVEC-NOT: add <8 x i32>
; DEVEC-NOT: shufflevector
; DEVEC-NOT: extractelement <2 x i128>
; DEVEC-NOT: bitcast i128
; DEVEC-NOT: <2 x i128>
; DEVEC: add i32
; DEVEC-NOT: load <8 x i32>
; DEVEC-NOT: add <8 x i32>
; DEVEC-NOT: shufflevector
; DEVEC-NOT: extractelement <2 x i128>
; DEVEC-NOT: bitcast i128
; DEVEC-NOT: <2 x i128>
; DEVEC: ret i32

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@values = global [16 x i32] [i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15, i32 16], align 4
@overflow = global [16 x i32] [i32 2147483647, i32 1, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0], align 4

define i32 @sub_140001370(ptr %0) {
bb_140001370:
  %1 = load <8 x i32>, ptr %0, align 1
  %element = getelementptr <8 x i32>, ptr %0, i64 1
  %2 = load <8 x i32>, ptr %element, align 1, !revenant.va !2
  %3 = add <8 x i32> %1, %2, !revenant.va !2
  %bc28 = bitcast <8 x i32> %3 to <2 x i128>
  %4 = extractelement <2 x i128> %bc28, i64 0
  %5 = bitcast <8 x i32> %3 to <4 x i64>
  %6 = shufflevector <4 x i64> %5, <4 x i64> poison, <2 x i32> <i32 2, i32 3>, !revenant.va !3
  %7 = bitcast <2 x i64> %6 to <4 x i32>
  %8 = bitcast i128 %4 to <4 x i32>, !revenant.va !4
  %9 = add <4 x i32> %8, %7, !revenant.va !4
  %10 = shufflevector <4 x i32> %9, <4 x i32> poison, <4 x i32> <i32 2, i32 3, i32 2, i32 3>, !revenant.va !5
  %11 = add <4 x i32> %9, %10, !revenant.va !6
  %12 = shufflevector <4 x i32> %11, <4 x i32> poison, <4 x i32> <i32 1, i32 poison, i32 poison, i32 poison>, !revenant.va !7
  %13 = add <4 x i32> %11, %12, !revenant.va !8
  %14 = extractelement <4 x i32> %13, i64 0, !revenant.va !9
  ret i32 %14, !revenant.va !10
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

!0 = !{i32 4, !"revenant.lang", !"rust"}
!1 = !{!"type_info", !".?AVtype_info@@", i64 5368826472, i64 5368795840}
!2 = !{i64 5368714100}
!3 = !{i64 5368714105}
!4 = !{i64 5368714111}
!5 = !{i64 5368714115}
!6 = !{i64 5368714120}
!7 = !{i64 5368714124}
!8 = !{i64 5368714129}
!9 = !{i64 5368714133}
!10 = !{i64 5368714140}
