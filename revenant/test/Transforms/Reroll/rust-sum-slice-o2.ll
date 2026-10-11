; RUN: %revenant-opt -passes=revenant-reroll,verify -S %s | %FileCheck %s
; Real rust-o2 lifted IR: corpus Rust `sum_slice_i32`, VA 0x140001380.
; rustc specializes the source loop to 16 sequential i32 loads and an
; accumulator recurrence. Rebuild that fixed-trip recurrence as a counted loop.
;
; CHECK-LABEL: define i32 @sub_140001380(ptr %0)
; CHECK: unroll:
; CHECK: %u0i = phi i64 [ 0, %bb_140001380 ], [ %u0inext, %unroll ]
; CHECK: %u0acc = phi i32 [ 0, %bb_140001380 ], [ %u0next, %unroll ]
; CHECK: %u0addr = getelementptr i32, ptr %0, i64 %u0i
; CHECK: %u0v = load i32, ptr %u0addr, align 1
; CHECK: %u0next = add i32 %u0acc, %u0v
; CHECK-NOT: load i32
; CHECK: %u0inext = add i64 %u0i, 1
; CHECK: %u0c = icmp ult i64 %u0inext, 16
; CHECK: br i1 %u0c, label %unroll, label %unroll.exit
; CHECK: unroll.exit:
; CHECK: ret i32 %u0next
; CHECK-NOT: br i1 undef
; CHECK-NOT: br i1 poison

@values = global [16 x i32] [i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8, i32 9, i32 10, i32 11, i32 12, i32 13, i32 14, i32 15, i32 16], align 4
@overflow = global [16 x i32] [i32 2147483647, i32 1, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0], align 4

define i32 @sub_140001380(ptr %0) {
bb_140001380:
  %element1 = getelementptr i32, ptr %0, i64 1
  %1 = load i32, ptr %element1, align 1, !revenant.va !0
  %2 = load i32, ptr %0, align 1, !revenant.va !1
  %3 = add i32 %1, %2, !revenant.va !1
  %element2 = getelementptr i32, ptr %0, i64 2
  %4 = load i32, ptr %element2, align 1, !revenant.va !2
  %5 = add i32 %3, %4, !revenant.va !2
  %element = getelementptr i32, ptr %0, i64 3
  %6 = load i32, ptr %element, align 1, !revenant.va !3
  %7 = add i32 %5, %6, !revenant.va !3
  %element3 = getelementptr i32, ptr %0, i64 4
  %8 = load i32, ptr %element3, align 1, !revenant.va !4
  %9 = add i32 %7, %8, !revenant.va !4
  %element4 = getelementptr i32, ptr %0, i64 5
  %10 = load i32, ptr %element4, align 1, !revenant.va !5
  %11 = add i32 %9, %10, !revenant.va !5
  %element5 = getelementptr i32, ptr %0, i64 6
  %12 = load i32, ptr %element5, align 1, !revenant.va !6
  %13 = add i32 %11, %12, !revenant.va !6
  %element6 = getelementptr i32, ptr %0, i64 7
  %14 = load i32, ptr %element6, align 1, !revenant.va !7
  %15 = add i32 %13, %14, !revenant.va !7
  %element7 = getelementptr i32, ptr %0, i64 8
  %16 = load i32, ptr %element7, align 1, !revenant.va !8
  %17 = add i32 %15, %16, !revenant.va !8
  %element8 = getelementptr i32, ptr %0, i64 9
  %18 = load i32, ptr %element8, align 1, !revenant.va !9
  %19 = add i32 %17, %18, !revenant.va !9
  %element9 = getelementptr i32, ptr %0, i64 10
  %20 = load i32, ptr %element9, align 1, !revenant.va !10
  %21 = add i32 %19, %20, !revenant.va !10
  %element10 = getelementptr i32, ptr %0, i64 11
  %22 = load i32, ptr %element10, align 1, !revenant.va !11
  %23 = add i32 %21, %22, !revenant.va !11
  %element11 = getelementptr i32, ptr %0, i64 12
  %24 = load i32, ptr %element11, align 1, !revenant.va !12
  %25 = add i32 %23, %24, !revenant.va !12
  %element12 = getelementptr i32, ptr %0, i64 13
  %26 = load i32, ptr %element12, align 1, !revenant.va !13
  %27 = add i32 %25, %26, !revenant.va !13
  %element13 = getelementptr i32, ptr %0, i64 14
  %28 = load i32, ptr %element13, align 1, !revenant.va !14
  %29 = add i32 %27, %28, !revenant.va !14
  %element14 = getelementptr i32, ptr %0, i64 15
  %30 = load i32, ptr %element14, align 1, !revenant.va !15
  %31 = add i32 %29, %30, !revenant.va !15
  ret i32 %31, !revenant.va !16
}

define i32 @main() {
entry:
  %sum = call i32 @sub_140001380(ptr @values)
  %ok = icmp eq i32 %sum, 136
  %wrapped = call i32 @sub_140001380(ptr @overflow)
  %wrap.ok = icmp eq i32 %wrapped, -2147483648
  %both = and i1 %ok, %wrap.ok
  %fail = xor i1 %both, true
  %ret = zext i1 %fail to i32
  ret i32 %ret
}

!0 = !{i64 5368714112}
!1 = !{i64 5368714115}
!2 = !{i64 5368714117}
!3 = !{i64 5368714120}
!4 = !{i64 5368714123}
!5 = !{i64 5368714126}
!6 = !{i64 5368714129}
!7 = !{i64 5368714132}
!8 = !{i64 5368714135}
!9 = !{i64 5368714138}
!10 = !{i64 5368714141}
!11 = !{i64 5368714144}
!12 = !{i64 5368714147}
!13 = !{i64 5368714150}
!14 = !{i64 5368714153}
!15 = !{i64 5368714156}
!16 = !{i64 5368714159}
