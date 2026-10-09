; Byte copy with a remainder-counted scalar tail: the tail latch counts
; (N - resume) mod 8, the S dispatch routes on the resume phi, and the
; resume feed passes through a trunc of n.vec. All three are accepted
; only through resume phis (D-incoming 0, R-incomings Bound-derived).
; Corpus shape: c-clang-o2 copy_bytes.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define dso_local i32 @copy_bytes(ptr noundef %0, ptr noundef readonly captures(none) %1, i32 noundef %2) local_unnamed_addr #10 {
  %4 = icmp sgt i32 %2, 0
  br i1 %4, label %5, label %55

5:                                                ; preds = %3
  %6 = ptrtoint ptr %0 to i64
  %7 = ptrtoint ptr %1 to i64
  %8 = zext nneg i32 %2 to i64
  %9 = icmp ult i32 %2, 32
  %10 = sub i64 %6, %7
  %11 = icmp ult i64 %10, 32
  %12 = or i1 %9, %11
  br i1 %12, label %30, label %13

13:                                               ; preds = %5
  %14 = and i64 %8, 2147483616
  %15 = trunc nuw nsw i64 %14 to i32
  %16 = getelementptr i8, ptr %1, i64 %14
  %17 = getelementptr i8, ptr %0, i64 %14
  br label %18

18:                                               ; preds = %18, %13
  %19 = phi i64 [ 0, %13 ], [ %26, %18 ]
  %20 = getelementptr i8, ptr %1, i64 %19
  %21 = getelementptr i8, ptr %0, i64 %19
  %22 = getelementptr i8, ptr %20, i64 16
  %23 = load <16 x i8>, ptr %20, align 1
  %24 = load <16 x i8>, ptr %22, align 1
  %25 = getelementptr i8, ptr %21, i64 16
  store <16 x i8> %23, ptr %21, align 1
  store <16 x i8> %24, ptr %25, align 1
  %26 = add nuw i64 %19, 32
  %27 = icmp eq i64 %26, %14
  br i1 %27, label %28, label %18, !llvm.loop !12

28:                                               ; preds = %18
  %29 = icmp eq i64 %14, %8
  br i1 %29, label %55, label %30

30:                                               ; preds = %5, %28
  %31 = phi i32 [ 0, %5 ], [ %15, %28 ]
  %32 = phi ptr [ %1, %5 ], [ %16, %28 ]
  %33 = phi ptr [ %0, %5 ], [ %17, %28 ]
  %34 = sub i32 %2, %31
  %35 = and i32 %34, 7
  %36 = icmp eq i32 %35, 0
  br i1 %36, label %48, label %37

37:                                               ; preds = %30, %37
  %38 = phi i32 [ %45, %37 ], [ %31, %30 ]
  %39 = phi ptr [ %42, %37 ], [ %32, %30 ]
  %40 = phi ptr [ %44, %37 ], [ %33, %30 ]
  %41 = phi i32 [ %46, %37 ], [ 0, %30 ]
  %42 = getelementptr inbounds nuw i8, ptr %39, i64 1
  %43 = load i8, ptr %39, align 1
  %44 = getelementptr inbounds nuw i8, ptr %40, i64 1
  store i8 %43, ptr %40, align 1
  %45 = add nuw nsw i32 %38, 1
  %46 = add i32 %41, 1
  %47 = icmp eq i32 %46, %35
  br i1 %47, label %48, label %37, !llvm.loop !15

48:                                               ; preds = %37, %30
  %49 = phi ptr [ poison, %30 ], [ %44, %37 ]
  %50 = phi i32 [ %31, %30 ], [ %45, %37 ]
  %51 = phi ptr [ %32, %30 ], [ %42, %37 ]
  %52 = phi ptr [ %33, %30 ], [ %44, %37 ]
  %53 = sub i32 %31, %2
  %54 = icmp ugt i32 %53, -8
  br i1 %54, label %55, label %61

55:                                               ; preds = %48, %61, %28, %3
  %56 = phi ptr [ %0, %3 ], [ %17, %28 ], [ %49, %48 ], [ %88, %61 ]
  %57 = ptrtoint ptr %56 to i64
  %58 = ptrtoint ptr %0 to i64
  %59 = sub i64 %57, %58
  %60 = trunc i64 %59 to i32
  ret i32 %60

61:                                               ; preds = %48, %61
  %62 = phi i32 [ %89, %61 ], [ %50, %48 ]
  %63 = phi ptr [ %86, %61 ], [ %51, %48 ]
  %64 = phi ptr [ %88, %61 ], [ %52, %48 ]
  %65 = getelementptr inbounds nuw i8, ptr %63, i64 1
  %66 = load i8, ptr %63, align 1
  %67 = getelementptr inbounds nuw i8, ptr %64, i64 1
  store i8 %66, ptr %64, align 1
  %68 = getelementptr inbounds nuw i8, ptr %63, i64 2
  %69 = load i8, ptr %65, align 1
  %70 = getelementptr inbounds nuw i8, ptr %64, i64 2
  store i8 %69, ptr %67, align 1
  %71 = getelementptr inbounds nuw i8, ptr %63, i64 3
  %72 = load i8, ptr %68, align 1
  %73 = getelementptr inbounds nuw i8, ptr %64, i64 3
  store i8 %72, ptr %70, align 1
  %74 = getelementptr inbounds nuw i8, ptr %63, i64 4
  %75 = load i8, ptr %71, align 1
  %76 = getelementptr inbounds nuw i8, ptr %64, i64 4
  store i8 %75, ptr %73, align 1
  %77 = getelementptr inbounds nuw i8, ptr %63, i64 5
  %78 = load i8, ptr %74, align 1
  %79 = getelementptr inbounds nuw i8, ptr %64, i64 5
  store i8 %78, ptr %76, align 1
  %80 = getelementptr inbounds nuw i8, ptr %63, i64 6
  %81 = load i8, ptr %77, align 1
  %82 = getelementptr inbounds nuw i8, ptr %64, i64 6
  store i8 %81, ptr %79, align 1
  %83 = getelementptr inbounds nuw i8, ptr %63, i64 7
  %84 = load i8, ptr %80, align 1
  %85 = getelementptr inbounds nuw i8, ptr %64, i64 7
  store i8 %84, ptr %82, align 1
  %86 = getelementptr inbounds nuw i8, ptr %63, i64 8
  %87 = load i8, ptr %83, align 1
  %88 = getelementptr inbounds nuw i8, ptr %64, i64 8
  store i8 %87, ptr %85, align 1
  %89 = add nuw nsw i32 %62, 8
  %90 = icmp eq i32 %89, %2
  br i1 %90, label %55, label %61, !llvm.loop !17
}

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind sspstrong willreturn memory(none) uwtable

; The vector tiers are gone; the scalar remainder and epilogue remain.
; CHECK-LABEL: define{{.*}}i32 @copy_bytes
; CHECK-NOT: <16 x i8>
; CHECK-NOT: llvm.vector.reduce
; CHECK: store i8
; CHECK: }

!10 = distinct !{!10}
!12 = distinct !{!12}
!15 = distinct !{!15}
!17 = distinct !{!17}
