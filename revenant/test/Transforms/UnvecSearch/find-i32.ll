; SSE2 32-bit lane search: 4-wide chunk loop over <4 x i32> with the
; lifter's sign-bit mask chain (sext + bitcast + icmp-slt + bitcast) and a
; scaled bit-scan (cttz + lshr 2), plus a scalar i32 tail.
; Adapted from real lifted IR (mingw-gcc -O2 -msse2 explicit chunk scan).
; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s

define i32 @scan(ptr %0, i64 %1, i32 %2)  {
bb_140001750:
  %3 = insertelement <4 x i32> poison, i32 %2, i64 0
  %4 = shufflevector <4 x i32> %3, <4 x i32> poison, <4 x i32> zeroinitializer, !revenant.va !1
  %t2 = trunc i64 %1 to i32, !revenant.va !2
  %c = icmp slt i32 %t2, 4
  br i1 %c, label %bb_1400017a0, label %bb_14000175f, !revenant.va !3

bb_14000175f:                                     ; preds = %bb_140001750
  %5 = and i32 %t2, 2147483644
  br label %bb_14000177d, !revenant.va !4

bb_140001770:                                     ; preds = %bb_14000177d
  %6 = add i32 %r9.0.i32, 4, !revenant.va !5
  %c100 = icmp eq i32 %6, %5, !revenant.va !6
  %address = getelementptr i8, ptr %r10.0.ptr, i64 16, !revenant.va !7
  br i1 %c100, label %bb_1400017a0, label %bb_14000177d, !revenant.va !8

bb_14000177d:                                     ; preds = %bb_140001770, %bb_14000175f
  %r9.0.i32 = phi i32 [ 0, %bb_14000175f ], [ %6, %bb_140001770 ]
  %r10.0.ptr = phi ptr [ %0, %bb_14000175f ], [ %address, %bb_140001770 ]
  %7 = load <4 x i32>, ptr %r10.0.ptr, align 1, !revenant.va !9
  %8 = icmp eq <4 x i32> %4, %7, !revenant.va !9
  %9 = sext <4 x i1> %8 to <4 x i32>, !revenant.va !9
  %10 = bitcast <4 x i32> %9 to <16 x i8>
  %11 = icmp slt <16 x i8> %10, zeroinitializer, !revenant.va !10
  %12 = bitcast <16 x i1> %11 to i16, !revenant.va !10
  %c115 = icmp eq i16 %12, 0
  br i1 %c115, label %bb_140001770, label %bb_14000178b, !revenant.va !11

bb_14000178b:                                     ; preds = %bb_14000177d
  %13 = call range(i16 0, 17) i16 @llvm.cttz.i16(i16 %12, i1 true)
  %14 = lshr i16 %13, 2
  %15 = zext nneg i16 %14 to i32
  %narrow218 = or disjoint i32 %r9.0.i32, %15
  br label %common.ret, !revenant.va !12

common.ret:                                       ; preds = %bb_1400017a0, %bb_1400017b0, %bb_14000178b, %bb_1400017be
  %common.ret.op = phi i32 [ %narrow218, %bb_14000178b ], [ %20, %bb_1400017be ], [ -1, %bb_1400017b0 ], [ -1, %bb_1400017a0 ]
  ret i32 %common.ret.op, !revenant.va !13

bb_1400017a0:                                     ; preds = %bb_140001750, %bb_140001770
  %rax.1.i32 = phi i32 [ %5, %bb_140001770 ], [ 0, %bb_140001750 ]
  %c160.not = icmp slt i32 %rax.1.i32, %t2, !revenant.va !6
  br i1 %c160.not, label %bb_1400017a4, label %common.ret, !revenant.va !14

bb_1400017a4:                                     ; preds = %bb_1400017a0
  %16 = zext nneg i32 %rax.1.i32 to i64
  br label %bb_1400017b8, !revenant.va !15

bb_1400017b0:                                     ; preds = %bb_1400017b8
  %17 = add i64 %rax.2, 1, !revenant.va !16
  %18 = trunc i64 %rax.2 to i32, !revenant.va !16
  %narrow246 = add i32 %18, 1, !revenant.va !17
  %c194.not = icmp slt i32 %narrow246, %t2, !revenant.va !6
  br i1 %c194.not, label %bb_1400017b8, label %common.ret, !revenant.va !18

bb_1400017b8:                                     ; preds = %bb_1400017b0, %bb_1400017a4
  %rax.2 = phi i64 [ %16, %bb_1400017a4 ], [ %17, %bb_1400017b0 ]
  %element = getelementptr i32, ptr %0, i64 %rax.2, !revenant.va !19
  %19 = load i32, ptr %element, align 1, !revenant.va !19
  %c214.not = icmp eq i32 %19, %2, !revenant.va !6
  br i1 %c214.not, label %bb_1400017be, label %bb_1400017b0, !revenant.va !20

bb_1400017be:                                     ; preds = %bb_1400017b8
  %20 = trunc i64 %rax.2 to i32
  br label %common.ret, !revenant.va !21
}

declare i16 @llvm.cttz.i16(i16, i1 immarg)

!1 = !{i64 5368715001}
!2 = !{i64 5368715002}
!3 = !{i64 5368715003}
!4 = !{i64 5368715004}
!5 = !{i64 5368715005}
!6 = !{i64 5368715006}
!7 = !{i64 5368715007}
!8 = !{i64 5368715008}
!9 = !{i64 5368715009}
!10 = !{i64 5368715010}
!11 = !{i64 5368715011}
!12 = !{i64 5368715012}
!13 = !{i64 5368715013}
!14 = !{i64 5368715014}
!15 = !{i64 5368715015}
!16 = !{i64 5368715016}
!17 = !{i64 5368715017}
!18 = !{i64 5368715018}
!19 = !{i64 5368715019}
!20 = !{i64 5368715020}
!21 = !{i64 5368715021}

; CHECK-LABEL: define i32 @scan(
; CHECK: br label %bb_1400017a0
; CHECK-NOT: call {{.*}} @llvm.cttz
; CHECK-NOT: icmp eq <4 x i32>
; CHECK-NOT: bitcast <16 x i1>
; CHECK: icmp eq i32 {{.*}}, %2
