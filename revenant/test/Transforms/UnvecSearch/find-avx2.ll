; AVX2 byte search: 32-wide chunk loop (vpcmpeqb + vpmovmskb + tzcnt,
; lifted as icmp-eq <32 x i8> + bitcast-to-i32 + llvm.cttz) with a scalar
; byte tail. Adapted from real lifted IR (mingw-gcc -O2 -mavx2).
; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s

define i32 @scan(ptr %0, i64 %1, i32 %2)  {
bb_1400017d0:
  %3 = trunc i32 %2 to i8
  %.splatinsert = insertelement <32 x i8> poison, i8 %3, i64 0, !revenant.va !22
  %.splat = shufflevector <32 x i8> %.splatinsert, <32 x i8> poison, <32 x i32> zeroinitializer, !revenant.va !22
  %t4 = trunc i64 %1 to i32, !revenant.va !23
  %c = icmp slt i32 %t4, 32
  br i1 %c, label %bb_140001828, label %bb_1400017e2, !revenant.va !24

bb_1400017e2:                                     ; preds = %bb_1400017d0
  %4 = and i32 %t4, 2147483616
  br label %bb_140001805, !revenant.va !25

bb_1400017f8:                                     ; preds = %bb_140001805
  %5 = add i32 %r8.0.i32, 32, !revenant.va !26
  %c101 = icmp eq i32 %4, %5, !revenant.va !27
  %address238 = getelementptr i8, ptr %r9.0.ptr, i64 32, !revenant.va !28
  br i1 %c101, label %bb_140001828, label %bb_140001805, !revenant.va !29

bb_140001805:                                     ; preds = %bb_1400017f8, %bb_1400017e2
  %r9.0.ptr = phi ptr [ %0, %bb_1400017e2 ], [ %address238, %bb_1400017f8 ]
  %r8.0.i32 = phi i32 [ 0, %bb_1400017e2 ], [ %5, %bb_1400017f8 ]
  %6 = load <32 x i8>, ptr %r9.0.ptr, align 1, !revenant.va !30
  %7 = icmp eq <32 x i8> %6, %.splat, !revenant.va !30
  %8 = bitcast <32 x i1> %7 to i32, !revenant.va !31
  %c116 = icmp eq i32 %8, 0, !revenant.va !27
  br i1 %c116, label %bb_1400017f8, label %bb_140001813, !revenant.va !32

bb_140001813:                                     ; preds = %bb_140001805
  %9 = call range(i32 0, 33) i32 @llvm.cttz.i32(i32 %8, i1 true), !revenant.va !33
  %narrow210 = or disjoint i32 %9, %r8.0.i32
  br label %common.ret, !revenant.va !34

common.ret:                                       ; preds = %bb_140001828, %bb_140001830, %bb_140001813, %bb_14000183e
  %common.ret.op = phi i32 [ %narrow210, %bb_140001813 ], [ %14, %bb_14000183e ], [ -1, %bb_140001830 ], [ -1, %bb_140001828 ]
  ret i32 %common.ret.op, !revenant.va !35

bb_140001828:                                     ; preds = %bb_1400017d0, %bb_1400017f8
  %rax.1.i32 = phi i32 [ %4, %bb_1400017f8 ], [ 0, %bb_1400017d0 ]
  %c153.not = icmp slt i32 %rax.1.i32, %t4, !revenant.va !27
  br i1 %c153.not, label %bb_14000182c, label %common.ret, !revenant.va !36

bb_14000182c:                                     ; preds = %bb_140001828
  %10 = zext nneg i32 %rax.1.i32 to i64
  br label %bb_140001838, !revenant.va !37

bb_140001830:                                     ; preds = %bb_140001838
  %11 = add i64 %rax.2, 1, !revenant.va !38
  %12 = trunc i64 %rax.2 to i32, !revenant.va !38
  %narrow240 = add i32 %12, 1, !revenant.va !39
  %c187.not = icmp slt i32 %narrow240, %t4, !revenant.va !27
  br i1 %c187.not, label %bb_140001838, label %common.ret, !revenant.va !40

bb_140001838:                                     ; preds = %bb_140001830, %bb_14000182c
  %rax.2 = phi i64 [ %10, %bb_14000182c ], [ %11, %bb_140001830 ]
  %address = getelementptr i8, ptr %0, i64 %rax.2, !revenant.va !41
  %13 = load i8, ptr %address, align 1, !revenant.va !41
  %c206.not = icmp eq i8 %13, %3, !revenant.va !27
  br i1 %c206.not, label %bb_14000183e, label %bb_140001830, !revenant.va !42

bb_14000183e:                                     ; preds = %bb_140001838
  %14 = trunc i64 %rax.2 to i32
  br label %common.ret, !revenant.va !43
}

declare i32 @llvm.cttz.i32(i32, i1 immarg)

!22 = !{i64 5368715022}
!23 = !{i64 5368715023}
!24 = !{i64 5368715024}
!25 = !{i64 5368715025}
!26 = !{i64 5368715026}
!27 = !{i64 5368715027}
!28 = !{i64 5368715028}
!29 = !{i64 5368715029}
!30 = !{i64 5368715030}
!31 = !{i64 5368715031}
!32 = !{i64 5368715032}
!33 = !{i64 5368715033}
!34 = !{i64 5368715034}
!35 = !{i64 5368715035}
!36 = !{i64 5368715036}
!37 = !{i64 5368715037}
!38 = !{i64 5368715038}
!39 = !{i64 5368715039}
!40 = !{i64 5368715040}
!41 = !{i64 5368715041}
!42 = !{i64 5368715042}
!43 = !{i64 5368715043}

; CHECK-LABEL: define i32 @scan(
; CHECK: br label %bb_140001828
; CHECK-NOT: call {{.*}} @llvm.cttz
; CHECK-NOT: icmp eq <32 x i8>
; CHECK-NOT: bitcast <32 x i1>
; CHECK: icmp eq i8 {{.*}}, %3
