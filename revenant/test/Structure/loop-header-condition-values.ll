; Real c-clang-o0 ir-final alloca form for sub_140001170.
; The final while tests an incremented/decremented header value and stores the
; value through its latch. For n=25 the function must return 100.
; RUN: python %S/loop-latch-backedge-run.py %s sub_140001170 25 100
; REQUIRES: structured-source-run

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
!1 = !{!"while", i32 1, i32 2, i32 3}
!2 = !{!"while", i32 4, i32 5, i32 8}
!3 = !{!"while", i32 6, i32 9, i32 7}

define i32 @sub_140001170(i32 %0) {
bb_140001180:
  %v_frame.sroa.0.0 = alloca i32, align 4
  %v_frame.sroa.4.0 = alloca i32, align 4
  %v_frame.sroa.8.2 = alloca i32, align 4
  store i32 0, ptr %v_frame.sroa.8.2, align 4
  store i32 0, ptr %v_frame.sroa.4.0, align 4
  br label %bb_1400011ea

bb_1400011ea:
  %1 = load i32, ptr %v_frame.sroa.4.0, align 4
  %c.not = icmp slt i32 %1, %0
  br i1 %c.not, label %bb_140001196, label %bb_1400011af, !revenant.region !1

bb_140001196:
  %2 = load i32, ptr %v_frame.sroa.4.0, align 4
  %3 = load i32, ptr %v_frame.sroa.8.2, align 4
  %4 = add i32 %2, %3
  %5 = load i32, ptr %v_frame.sroa.4.0, align 4
  %6 = add i32 %5, 1
  store i32 %4, ptr %v_frame.sroa.8.2, align 4
  store i32 %6, ptr %v_frame.sroa.4.0, align 4
  br label %bb_1400011ea

bb_1400011af:
  store i32 %0, ptr %v_frame.sroa.0.0, align 4
  br label %bb_1400011b6

bb_1400011b6:
  %7 = load i32, ptr %v_frame.sroa.0.0, align 4
  %c91 = icmp slt i32 %7, 4
  br i1 %c91, label %bb_x0, label %bb_1400011bb, !revenant.region !2

bb_1400011bb:
  %8 = load i32, ptr %v_frame.sroa.0.0, align 4
  %9 = load i32, ptr %v_frame.sroa.8.2, align 4
  %10 = add i32 %8, %9
  %11 = load i32, ptr %v_frame.sroa.0.0, align 4
  %12 = add nsw i32 %11, -2
  store i32 %10, ptr %v_frame.sroa.8.2, align 4
  store i32 %12, ptr %v_frame.sroa.0.0, align 4
  br label %bb_1400011b6

bb_1400011d4:
  %13 = load i32, ptr %v_frame.sroa.8.2, align 4
  %14 = add i32 %13, -1
  %c169 = icmp sgt i32 %14, 100
  br i1 %c169, label %bb_x1, label %bb_1400011ea206, !revenant.region !3

bb_1400011ea206:
  ret i32 %14

bb_x0:
  br label %bb_1400011d4

bb_x1:
  store i32 %14, ptr %v_frame.sroa.8.2, align 4
  br label %bb_1400011d4
}
