; SSE2 byte search: 16-wide chunk loop (pcmpeqb + pmovmskb + bsf, lifted
; as icmp-eq + bitcast-to-i16 + llvm.cttz) with a scalar byte tail.
; Adapted from real lifted IR (mingw-gcc -O2 -msse2 explicit chunk scan).
; The pass folds the dispatch to the scalar tail at index 0 and erases the
; chunk region, leaving one scalar search loop with no vector types.
; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s

define i32 @scan(ptr %0, i64 %1, i32 %2)  {
bb_140001750:
  %3 = zext i32 %2 to i128, !revenant.va !1
  %4 = bitcast i128 %3 to <16 x i8>, !revenant.va !2
  %5 = shufflevector <16 x i8> %4, <16 x i8> poison, <16 x i32> <i32 0, i32 0, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison>
  %6 = bitcast <16 x i8> %5 to <8 x i16>
  %7 = shufflevector <8 x i16> %6, <8 x i16> poison, <8 x i32> <i32 0, i32 0, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison>
  %8 = bitcast <8 x i16> %7 to <4 x i32>
  %9 = shufflevector <4 x i32> %8, <4 x i32> poison, <4 x i32> zeroinitializer, !revenant.va !3
  %t8 = trunc i64 %1 to i32, !revenant.va !4
  %c = icmp slt i32 %t8, 16
  br i1 %c, label %bb_1400017b0, label %bb_14000176a, !revenant.va !5

bb_14000176a:                                     ; preds = %bb_140001750
  %10 = and i32 %t8, 2147483632
  br label %bb_14000178d, !revenant.va !6

bb_140001780:                                     ; preds = %bb_14000178d
  %11 = add i32 %r8.0.i32, 16, !revenant.va !7
  %c101 = icmp eq i32 %10, %11, !revenant.va !8
  %address244 = getelementptr i8, ptr %r9.0.ptr, i64 16, !revenant.va !9
  br i1 %c101, label %bb_1400017b0, label %bb_14000178d, !revenant.va !10

bb_14000178d:                                     ; preds = %bb_140001780, %bb_14000176a
  %r9.0.ptr = phi ptr [ %0, %bb_14000176a ], [ %address244, %bb_140001780 ]
  %r8.0.i32 = phi i32 [ 0, %bb_14000176a ], [ %11, %bb_140001780 ]
  %12 = load <16 x i8>, ptr %r9.0.ptr, align 1
  %13 = bitcast <4 x i32> %9 to <16 x i8>
  %14 = icmp eq <16 x i8> %12, %13, !revenant.va !11
  %15 = bitcast <16 x i1> %14 to i16, !revenant.va !12
  %c117 = icmp eq i16 %15, 0
  br i1 %c117, label %bb_140001780, label %bb_1400017a0, !revenant.va !13

bb_1400017a0:                                     ; preds = %bb_14000178d
  %16 = call range(i16 0, 17) i16 @llvm.cttz.i16(i16 %15, i1 true)
  %17 = zext nneg i16 %16 to i32
  %narrow211 = or disjoint i32 %r8.0.i32, %17
  br label %common.ret, !revenant.va !14

common.ret:                                       ; preds = %bb_1400017b0, %bb_1400017c0, %bb_1400017a0, %bb_1400017ce
  %common.ret.op = phi i32 [ %narrow211, %bb_1400017a0 ], [ %22, %bb_1400017ce ], [ -1, %bb_1400017c0 ], [ -1, %bb_1400017b0 ]
  ret i32 %common.ret.op, !revenant.va !15

bb_1400017b0:                                     ; preds = %bb_140001750, %bb_140001780
  %rax.1.i32 = phi i32 [ %10, %bb_140001780 ], [ 0, %bb_140001750 ]
  %c154.not = icmp slt i32 %rax.1.i32, %t8, !revenant.va !8
  br i1 %c154.not, label %bb_1400017b4, label %common.ret, !revenant.va !16

bb_1400017b4:                                     ; preds = %bb_1400017b0
  %18 = zext nneg i32 %rax.1.i32 to i64
  br label %bb_1400017c8, !revenant.va !17

bb_1400017c0:                                     ; preds = %bb_1400017c8
  %19 = add i64 %rax.2, 1, !revenant.va !18
  %20 = trunc i64 %rax.2 to i32, !revenant.va !18
  %narrow246 = add i32 %20, 1, !revenant.va !19
  %c188.not = icmp slt i32 %narrow246, %t8, !revenant.va !8
  br i1 %c188.not, label %bb_1400017c8, label %common.ret, !revenant.va !20

bb_1400017c8:                                     ; preds = %bb_1400017c0, %bb_1400017b4
  %rax.2 = phi i64 [ %18, %bb_1400017b4 ], [ %19, %bb_1400017c0 ]
  %t190 = trunc i32 %2 to i8
  %address = getelementptr i8, ptr %0, i64 %rax.2, !revenant.va !21
  %21 = load i8, ptr %address, align 1, !revenant.va !21
  %c207.not = icmp eq i8 %21, %t190, !revenant.va !8
  br i1 %c207.not, label %bb_1400017ce, label %bb_1400017c0, !revenant.va !22

bb_1400017ce:                                     ; preds = %bb_1400017c8
  %22 = trunc i64 %rax.2 to i32
  br label %common.ret, !revenant.va !23
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
!22 = !{i64 5368715022}
!23 = !{i64 5368715023}

; CHECK-LABEL: define i32 @scan(
; The dispatch folds straight to the scalar tail.
; CHECK: br label %bb_1400017b0
; No chunk loop, no bit-scan, no vector compare/mask remain.
; CHECK-NOT: call {{.*}} @llvm.cttz
; CHECK-NOT: icmp eq <16 x i8>
; CHECK-NOT: bitcast <16 x i1>
; The scalar tail still compares one byte per pass against the needle.
; CHECK: icmp eq i8 {{.*}}, {{.*}}
