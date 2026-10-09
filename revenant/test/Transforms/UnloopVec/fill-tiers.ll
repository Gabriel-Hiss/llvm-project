; Three-tier vector cascade (8-wide + 4-wide + scalar, AVX2): the middle
; tier restarts its IV inside the erased region, so it matches as a
; secondary body (same trip count, R-defined start). Corpus shape:
; c-clang-o2avx2 fill_vec.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define dso_local void @fill_vec(ptr noundef writeonly captures(none) %0, i32 noundef %1, i32 noundef %2) local_unnamed_addr #12 {
  %4 = icmp sgt i32 %2, 0
  br i1 %4, label %5, label %41

5:                                                ; preds = %3
  %6 = zext nneg i32 %2 to i64
  %7 = icmp ult i32 %2, 4
  br i1 %7, label %39, label %8

8:                                                ; preds = %5
  %9 = icmp ult i32 %2, 32
  br i1 %9, label %27, label %10

10:                                               ; preds = %8
  %11 = and i64 %6, 28
  %12 = and i64 %6, 2147483616
  %13 = insertelement <8 x i32> poison, i32 %1, i64 0
  %14 = shufflevector <8 x i32> %13, <8 x i32> poison, <8 x i32> zeroinitializer
  br label %15

15:                                               ; preds = %15, %10
  %16 = phi i64 [ 0, %10 ], [ %21, %15 ]
  %17 = getelementptr inbounds nuw i32, ptr %0, i64 %16
  %18 = getelementptr inbounds nuw i8, ptr %17, i64 32
  %19 = getelementptr inbounds nuw i8, ptr %17, i64 64
  %20 = getelementptr inbounds nuw i8, ptr %17, i64 96
  store <8 x i32> %14, ptr %17, align 4
  store <8 x i32> %14, ptr %18, align 4
  store <8 x i32> %14, ptr %19, align 4
  store <8 x i32> %14, ptr %20, align 4
  %21 = add nuw i64 %16, 32
  %22 = icmp eq i64 %21, %12
  br i1 %22, label %23, label %15, !llvm.loop !26

23:                                               ; preds = %15
  %24 = icmp eq i64 %12, %6
  br i1 %24, label %41, label %25

25:                                               ; preds = %23
  %26 = icmp eq i64 %11, 0
  br i1 %26, label %39, label %27, !prof !19

27:                                               ; preds = %8, %25
  %28 = phi i64 [ %12, %25 ], [ 0, %8 ]
  %29 = and i64 %6, 2147483644
  %30 = insertelement <4 x i32> poison, i32 %1, i64 0
  %31 = shufflevector <4 x i32> %30, <4 x i32> poison, <4 x i32> zeroinitializer
  br label %32

32:                                               ; preds = %32, %27
  %33 = phi i64 [ %28, %27 ], [ %35, %32 ]
  %34 = getelementptr inbounds nuw i32, ptr %0, i64 %33
  store <4 x i32> %31, ptr %34, align 4
  %35 = add nuw i64 %33, 4
  %36 = icmp eq i64 %35, %29
  br i1 %36, label %37, label %32, !llvm.loop !27

37:                                               ; preds = %32
  %38 = icmp eq i64 %29, %6
  br i1 %38, label %41, label %39

39:                                               ; preds = %5, %25, %37
  %40 = phi i64 [ 0, %5 ], [ %12, %25 ], [ %29, %37 ]
  br label %42

41:                                               ; preds = %42, %23, %37, %3
  ret void

42:                                               ; preds = %39, %42
  %43 = phi i64 [ %45, %42 ], [ %40, %39 ]
  %44 = getelementptr inbounds nuw i32, ptr %0, i64 %43
  store i32 %1, ptr %44, align 4
  %45 = add nuw nsw i64 %43, 1
  %46 = icmp eq i64 %45, %6
  br i1 %46, label %41, label %42, !llvm.loop !28
}

; Both vector tiers are gone; the scalar loop remains.
; CHECK-LABEL: define{{.*}}void @fill_vec
; CHECK-NOT: <8 x i32>
; CHECK-NOT: <4 x i32>
; CHECK: store i32
; CHECK: }

!19 = !{!"branch_weights", i32 1, i32 100}
!26 = distinct !{!26}
!27 = distinct !{!27}
!28 = distinct !{!28}
