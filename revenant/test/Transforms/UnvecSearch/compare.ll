; Dual-buffer compare (memcmp-like early exit): 16-wide chunk loop
; comparing two buffers lane-wise, continuing while the mask is all-ones
; and exiting with cttz of the inverted mask at the first difference.
; Includes the latch-exit resume chain (bound + stride block) feeding the
; scalar tail. Adapted from real lifted IR (mingw-gcc -O2 -msse2).
; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s

define i32 @scan(ptr %0, ptr %1, i64 %2)  {
bb_1400018d0:
  %t = trunc i64 %2 to i32, !revenant.va !66
  %c = icmp slt i32 %t, 16
  br i1 %c, label %bb_140001923, label %bb_1400018d9, !revenant.va !67

bb_1400018d9:                                     ; preds = %bb_1400018d0
  %narrow = add nsw i32 %t, -16, !revenant.va !68
  %3 = lshr i32 %narrow, 4, !revenant.va !69
  %4 = add nuw nsw i32 %3, 1, !revenant.va !70
  %z50 = zext nneg i32 %4 to i64, !revenant.va !70
  %5 = shl nuw nsw i64 %z50, 4, !revenant.va !71
  br label %bb_1400018fc, !revenant.va !72

bb_1400018f0:                                     ; preds = %bb_1400018fc
  %d58 = add i64 %rax.0, 16, !revenant.va !73
  %c74 = icmp eq i64 %5, %d58, !revenant.va !74
  br i1 %c74, label %bb_140001920, label %bb_1400018fc, !revenant.va !75

bb_1400018fc:                                     ; preds = %bb_1400018f0, %bb_1400018d9
  %rax.0 = phi i64 [ 0, %bb_1400018d9 ], [ %d58, %bb_1400018f0 ]
  %address = getelementptr i8, ptr %1, i64 %rax.0, !revenant.va !76
  %6 = load <16 x i8>, ptr %address, align 1
  %address232 = getelementptr i8, ptr %0, i64 %rax.0, !revenant.va !77
  %7 = load <16 x i8>, ptr %address232, align 1, !revenant.va !77
  %8 = icmp eq <16 x i8> %6, %7, !revenant.va !77
  %9 = bitcast <16 x i1> %8 to i16, !revenant.va !78
  %c99 = icmp eq i16 %9, -1
  br i1 %c99, label %bb_1400018f0, label %bb_140001913, !revenant.va !79

bb_140001913:                                     ; preds = %bb_1400018fc
  %t101 = zext i16 %9 to i32
  %10 = xor i32 %t101, -1, !revenant.va !80
  %11 = call range(i32 0, 17) i32 @llvm.cttz.i32(i32 %10, i1 true), !revenant.va !81
  %t109 = trunc i64 %rax.0 to i32, !revenant.va !82
  %12 = add i32 %11, %t109, !revenant.va !82
  br label %common.ret, !revenant.va !82

common.ret:                                       ; preds = %bb_140001923, %bb_140001930, %bb_140001913, %bb_140001943
  %common.ret.op = phi i32 [ %12, %bb_140001913 ], [ %20, %bb_140001943 ], [ -1, %bb_140001930 ], [ -1, %bb_140001923 ]
  ret i32 %common.ret.op, !revenant.va !83

bb_140001920:                                     ; preds = %bb_1400018f0
  %13 = trunc i64 %rax.0 to i32
  %14 = add i32 %13, 16
  br label %bb_140001923, !revenant.va !84

bb_140001923:                                     ; preds = %bb_1400018d0, %bb_140001920
  %rax.2.i32 = phi i32 [ %14, %bb_140001920 ], [ 0, %bb_1400018d0 ]
  %c158.not = icmp slt i32 %rax.2.i32, %t, !revenant.va !74
  br i1 %c158.not, label %bb_140001928, label %common.ret, !revenant.va !85

bb_140001928:                                     ; preds = %bb_140001923
  %15 = sext i32 %rax.2.i32 to i64, !revenant.va !86
  br label %bb_140001939, !revenant.va !87

bb_140001930:                                     ; preds = %bb_140001939
  %16 = add i64 %rax.3, 1, !revenant.va !88
  %17 = trunc i64 %rax.3 to i32, !revenant.va !88
  %narrow270 = add i32 %17, 1, !revenant.va !89
  %c192.not = icmp slt i32 %narrow270, %t, !revenant.va !74
  br i1 %c192.not, label %bb_140001939, label %common.ret, !revenant.va !90

bb_140001939:                                     ; preds = %bb_140001930, %bb_140001928
  %rax.3 = phi i64 [ %15, %bb_140001928 ], [ %16, %bb_140001930 ]
  %address235 = getelementptr i8, ptr %1, i64 %rax.3, !revenant.va !91
  %18 = load i8, ptr %address235, align 1, !revenant.va !91
  %address237 = getelementptr i8, ptr %0, i64 %rax.3, !revenant.va !92
  %19 = load i8, ptr %address237, align 1, !revenant.va !92
  %c216 = icmp eq i8 %19, %18, !revenant.va !74
  br i1 %c216, label %bb_140001930, label %bb_140001943, !revenant.va !93

bb_140001943:                                     ; preds = %bb_140001939
  %20 = trunc i64 %rax.3 to i32
  br label %common.ret, !revenant.va !94
}

declare i32 @llvm.cttz.i32(i32, i1 immarg)

!66 = !{i64 5368715066}
!67 = !{i64 5368715067}
!68 = !{i64 5368715068}
!69 = !{i64 5368715069}
!70 = !{i64 5368715070}
!71 = !{i64 5368715071}
!72 = !{i64 5368715072}
!73 = !{i64 5368715073}
!74 = !{i64 5368715074}
!75 = !{i64 5368715075}
!76 = !{i64 5368715076}
!77 = !{i64 5368715077}
!78 = !{i64 5368715078}
!79 = !{i64 5368715079}
!80 = !{i64 5368715080}
!81 = !{i64 5368715081}
!82 = !{i64 5368715082}
!83 = !{i64 5368715083}
!84 = !{i64 5368715084}
!85 = !{i64 5368715085}
!86 = !{i64 5368715086}
!87 = !{i64 5368715087}
!88 = !{i64 5368715088}
!89 = !{i64 5368715089}
!90 = !{i64 5368715090}
!91 = !{i64 5368715091}
!92 = !{i64 5368715092}
!93 = !{i64 5368715093}
!94 = !{i64 5368715094}

; CHECK-LABEL: define i32 @scan(
; The dispatch folds straight to the scalar tail, including erasing the
; latch-exit resume chain block.
; CHECK: br label %bb_140001923
; CHECK-NOT: call {{.*}} @llvm.cttz
; CHECK-NOT: icmp eq <16 x i8>
; CHECK-NOT: bitcast <16 x i1>
; The scalar tail compares one byte pair per pass.
; CHECK: icmp eq i8 {{.*}}, {{.*}}
