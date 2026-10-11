; Real ir-final alloca-form c-clang-o0 break_continue at 0x140001240.
; For n=30 the IR returns 147; the emitted source must terminate with that value.
; RUN: python %S/loop-latch-backedge-run.py %s sub_140001240 30 147
; REQUIRES: structured-source-run

source_filename = "c-clang-o0.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}

!10514 = !{i64 5368713808}
!10515 = !{i64 5368713887}
!10516 = !{i64 5368713824}
!10517 = !{!"while", i32 1, i32 5, i32 6}
!10518 = !{i64 5368713836}
!10519 = !{i64 5368713841}
!10520 = !{!"if", i32 6}
!10521 = !{i64 5368713850}
!10522 = !{i64 5368713858}
!10523 = !{i64 5368713862}
!10524 = !{i64 5368713870}
!10525 = !{i64 5368713877}

define i32 @sub_140001240(i32 %0) #13 {
bb_140001250:
  %v_frame.sroa.0.0 = alloca i32, align 4
  %v_frame.sroa.4.0 = alloca i32, align 4
  store i32 0, ptr %v_frame.sroa.4.0, align 4, !revenant.va !10514
  store i32 0, ptr %v_frame.sroa.0.0, align 4, !revenant.va !10514
  br label %bb_14000129f, !revenant.va !10514

bb_14000129f:                                     ; preds = %bb_14000128e, %bb_140001250
  %1 = load i32, ptr %v_frame.sroa.0.0, align 4, !revenant.va !10515
  %c.not = icmp slt i32 %1, %0, !revenant.va !10515
  br i1 %c.not, label %bb_14000126c, label %bb_14000129f147, !revenant.va !10516, !revenant.region !10517

bb_14000126c:                                     ; preds = %bb_14000129f
  %2 = load i32, ptr %v_frame.sroa.0.0, align 4
  %3 = sext i32 %2 to i64
  %4 = srem i64 %3, 3, !revenant.va !10518
  %c58.not = icmp eq i64 %4, 0
  br i1 %c58.not, label %bb_x0, label %bb_14000127a, !revenant.va !10519, !revenant.region !10520

bb_14000127a:                                     ; preds = %bb_14000126c
  %5 = load i32, ptr %v_frame.sroa.0.0, align 4
  %c73 = icmp slt i32 %5, 21
  br i1 %c73, label %bb_140001282, label %bb_14000129f147, !revenant.va !10521, !revenant.region !10520

bb_140001282:                                     ; preds = %bb_14000127a
  %6 = load i32, ptr %v_frame.sroa.0.0, align 4, !revenant.va !10522
  %7 = load i32, ptr %v_frame.sroa.4.0, align 4, !revenant.va !10522
  %8 = add i32 %6, %7, !revenant.va !10522
  store i32 %8, ptr %v_frame.sroa.4.0, align 4, !revenant.va !10523
  br label %bb_14000128e, !revenant.va !10523

bb_14000128e:                                     ; preds = %bb_x0, %bb_140001282
  %9 = load i32, ptr %v_frame.sroa.0.0, align 4, !revenant.va !10524
  %10 = add i32 %9, 1, !revenant.va !10524
  store i32 %10, ptr %v_frame.sroa.0.0, align 4, !revenant.va !10525
  br label %bb_14000129f, !revenant.va !10525

bb_14000129f147:                                  ; preds = %bb_14000127a, %bb_14000129f
  %11 = load i32, ptr %v_frame.sroa.4.0, align 4, !revenant.va !10515
  ret i32 %11, !revenant.va !10515

bb_x0:                                            ; preds = %bb_14000126c
  br label %bb_14000128e
}
