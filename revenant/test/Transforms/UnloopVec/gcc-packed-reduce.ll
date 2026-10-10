; GCC O3 packed integer reduction: four-lane <4 x i32> accumulate loop
; with an i128 shift lane fold and a fully unrolled guarded remainder.
; There is no scalar fallback loop: the small-N guard jumps straight
; into the unrolled remainder with the start values (0, 0), which only
; covers the at most VF-1 leftover elements. UnloopVec proves the whole
; guarded arm reads a[0..N) exactly once and replaces it with the scalar
; counted loop the vectorizer started from, keeping the exit merge.
; Corpus shape: c-gcc-o3 sum_vec (target/parity/c-gcc-o3), sub_140001f90.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define i64 @sum_vec(ptr %0, i64 %1) {
bb_140001f90:
  %abi.bits = ptrtoint ptr %0 to i64
  %2 = trunc i64 %1 to i32
  %o = icmp slt i32 %2, 1
  br i1 %o, label %common.ret, label %bb_140001f9b

bb_140001f9b:
  %c = icmp ult i32 %2, 4
  br i1 %c, label %bb_140001f9b.bb_140001ff9_crit_edge, label %bb_140001fa7

bb_140001f9b.bb_140001ff9_crit_edge:
  br label %bb_140001ff9

bb_140001fa7:
  %3 = shl i64 %1, 2
  %4 = and i64 %3, 8589934576
  %5 = add i64 %4, %abi.bits
  br label %bb_140001fc0

bb_140001fc0:
  %6 = phi <4 x i32> [ zeroinitializer, %bb_140001fa7 ], [ %9, %bb_140001fc0 ]
  %rax.0.ptr = phi ptr [ %0, %bb_140001fa7 ], [ %address, %bb_140001fc0 ]
  %rax.0 = phi i64 [ %abi.bits, %bb_140001fa7 ], [ %8, %bb_140001fc0 ]
  %7 = load <4 x i32>, ptr %rax.0.ptr, align 1
  %8 = add i64 %rax.0, 16
  %9 = add <4 x i32> %6, %7
  %c94.not = icmp eq i64 %8, %5
  %address = getelementptr i8, ptr %rax.0.ptr, i64 16
  br i1 %c94.not, label %bb_140001fd1, label %bb_140001fc0

bb_140001fd1:
  %10 = bitcast <4 x i32> %9 to i128
  %11 = lshr i128 %10, 64
  %12 = and i64 %1, 2147483644
  %13 = bitcast i128 %11 to <4 x i32>
  %14 = add <4 x i32> %9, %13
  %15 = bitcast <4 x i32> %14 to i128
  %16 = lshr i128 %15, 32
  %17 = bitcast i128 %16 to <4 x i32>
  %18 = add <4 x i32> %14, %17
  %19 = extractelement <4 x i32> %18, i64 0
  %20 = and i64 %1, 3
  %c124 = icmp eq i64 %20, 0
  br i1 %c124, label %bb_140002022, label %bb_140001ff9

bb_140001ff9:
  %rcx.0.i32 = phi i64 [ 0, %bb_140001f9b.bb_140001ff9_crit_edge ], [ %12, %bb_140001fd1 ]
  %rax.1.i32 = phi i32 [ 0, %bb_140001f9b.bb_140001ff9_crit_edge ], [ %19, %bb_140001fd1 ]
  %element = getelementptr i32, ptr %0, i64 %rcx.0.i32
  %21 = load i32, ptr %element, align 1
  %22 = add i32 %rax.1.i32, %21
  %23 = trunc nuw nsw i64 %rcx.0.i32 to i32
  %narrow283 = or disjoint i32 %23, 1
  %c168.not = icmp slt i32 %narrow283, %2
  br i1 %c168.not, label %bb_140002011, label %bb_140002022

bb_140002011:
  %sc = shl nuw nsw i64 %rcx.0.i32, 2
  %24 = or disjoint i32 %23, 2
  %address285 = getelementptr i8, ptr %0, i64 %sc
  %element1 = getelementptr i32, ptr %address285, i64 1
  %25 = load i32, ptr %element1, align 1
  %26 = add i32 %22, %25
  %c223.not = icmp slt i32 %24, %2
  br i1 %c223.not, label %bb_14000201d, label %bb_140002022

bb_14000201d:
  %element2 = getelementptr i32, ptr %address285, i64 2
  %27 = load i32, ptr %element2, align 1
  %28 = add i32 %26, %27
  br label %bb_140002022

common.ret:
  %common.ret.op.i32.i32 = phi i32 [ %rax.2.i32, %bb_140002022 ], [ 0, %bb_140001f90 ]
  %29 = zext i32 %common.ret.op.i32.i32 to i64
  ret i64 %29

bb_140002022:
  %rax.2.i32 = phi i32 [ %22, %bb_140001ff9 ], [ %26, %bb_140002011 ], [ %28, %bb_14000201d ], [ %19, %bb_140001fd1 ]
  br label %common.ret
}

; The vector loop, the lane fold and the unrolled remainder are gone; the
; guard now feeds one scalar counted loop over [0, N) that keeps the
; exit merge and the return path.
; CHECK-LABEL: define{{.*}}i64 @sum_vec
; CHECK-NOT: <4 x i32>
; CHECK-NOT: extractelement
; CHECK-NOT: bitcast
; CHECK: zext i32 %2 to i64
; CHECK: phi i64 [ 0, %bb_140001f9b ]
; CHECK: phi i32 [ 0, %bb_140001f9b ]
; CHECK: getelementptr i32, ptr %0, i64
; CHECK: load i32
; CHECK: add i32
; CHECK: add i64
; CHECK: icmp eq i64
; CHECK: br i1
; CHECK: phi i32
; CHECK: zext i32
; CHECK: ret i64
; CHECK: }
