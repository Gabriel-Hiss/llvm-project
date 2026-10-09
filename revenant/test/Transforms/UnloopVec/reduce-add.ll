; Reduction over 8 x i32 with the standard 4-tier skeleton (clang O2):
; min-iters dispatch, wide body with 2-way interleave, middle reduce,
; scalar tail. UnloopVec folds the dispatch to the scalar edge and erases
; the vector tiers, leaving the scalar loop. Corpus shape: c-clang-o2
; main.post.ll @sum_vec, scalar tail block %30.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define i32 @sum_vec(ptr noundef readonly captures(none) %0, i32 noundef %1) {
  %3 = icmp sgt i32 %1, 0
  br i1 %3, label %4, label %28

4:
  %5 = zext nneg i32 %1 to i64
  %6 = icmp ult i32 %1, 8
  br i1 %6, label %25, label %7

7:
  %8 = and i64 %5, 2147483640
  br label %9

9:
  %10 = phi i64 [ 0, %7 ], [ %19, %9 ]
  %11 = phi <4 x i32> [ zeroinitializer, %7 ], [ %17, %9 ]
  %12 = phi <4 x i32> [ zeroinitializer, %7 ], [ %18, %9 ]
  %13 = getelementptr inbounds nuw i32, ptr %0, i64 %10
  %14 = getelementptr inbounds nuw i8, ptr %13, i64 16
  %15 = load <4 x i32>, ptr %13, align 4
  %16 = load <4 x i32>, ptr %14, align 4
  %17 = add <4 x i32> %15, %11
  %18 = add <4 x i32> %16, %12
  %19 = add nuw i64 %10, 8
  %20 = icmp eq i64 %19, %8
  br i1 %20, label %21, label %9

21:
  %22 = add <4 x i32> %18, %17
  %23 = tail call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %22)
  %24 = icmp eq i64 %8, %5
  br i1 %24, label %28, label %25

25:
  %26 = phi i64 [ 0, %4 ], [ %8, %21 ]
  %27 = phi i32 [ 0, %4 ], [ %23, %21 ]
  br label %30

28:
  %29 = phi i32 [ 0, %2 ], [ %23, %21 ], [ %35, %30 ]
  ret i32 %29

30:
  %31 = phi i64 [ %36, %30 ], [ %26, %25 ]
  %32 = phi i32 [ %35, %30 ], [ %27, %25 ]
  %33 = getelementptr inbounds nuw i32, ptr %0, i64 %31
  %34 = load i32, ptr %33, align 4
  %35 = add nsw i32 %34, %32
  %36 = add nuw nsw i64 %31, 1
  %37 = icmp eq i64 %36, %5
  br i1 %37, label %28, label %30
}

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

; The wide body and middle reduce are gone, the dispatch folds to scalar.
; CHECK-NOT: <4 x i32>
; CHECK-NOT: call i32 @llvm.vector.reduce
; CHECK: br label
; CHECK: ret i32
