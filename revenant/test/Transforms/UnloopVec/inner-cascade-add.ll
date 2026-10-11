; Inner-cascade map fold: the middle <4 x i32> tier plus its
; preheader and exit are erased through the shared lane-equivalence
; gate, leaving the countdown drain and the counted tail to cover
; the full range. Corpus shape: c-clang-o2avx2 add_vec inner dispatch.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define void @avx2_add_inner(ptr %0, ptr %1, i32 %2, i32 %3) {
entry:
  %o = icmp slt i32 %3, 1
  br i1 %o, label %bb_140001b0b, label %entry.ok
entry.ok:
  %z = zext nneg i32 %3 to i64
  %outer.cond = icmp ugt i32 %3, 31
  br i1 %outer.cond, label %bb_140001a68, label %bb_140001a9a
bb_140001a68:
                                     
  %m.and = and i32 %3, 28
  %c265 = icmp eq i32 %m.and, 0
  br i1 %c265, label %bb_140001a9a, label %bb_140001a6c
bb_140001a9a:
                                     
  %r9.1.i32 = phi i32 [ 0, %bb_140001a68 ], [ %m.nvec4, %bb_140001a95 ], [ 0, %entry.ok ]
  %s.start64 = zext nneg i32 %r9.1.i32 to i64
  %s.rem = and i64 %z, 3
  %c348 = icmp eq i64 %s.rem, 0
  br i1 %c348, label %bb_140001ac3, label %bb_140001ab0
bb_140001a6c:
                                     
  %r9.0.i32 = phi i32 [ 0, %bb_140001a68 ]
  %m.start64 = zext nneg i32 %r9.0.i32 to i64
  %m.nvec4 = and i32 %3, 2147483644
  %z278 = zext nneg i32 %m.nvec4 to i64
  %.splatinsert280 = insertelement <4 x i32> poison, i32 %2, i64 0
  %.splat281 = shufflevector <4 x i32> %.splatinsert280, <4 x i32> poison, <4 x i32> zeroinitializer
  br label %bb_140001a80
bb_140001a80:
                                     
  %r10.0 = phi i64 [ %m.start64, %bb_140001a6c ], [ %m.next, %bb_140001a80 ]
  %sc284 = shl i64 %r10.0, 2
  %address615 = getelementptr i8, ptr %1, i64 %sc284
  %m.load = load <4 x i32>, ptr %address615, align 1
  %m.add = add <4 x i32> %.splat281, %m.load
  %address617 = getelementptr i8, ptr %0, i64 %sc284
  store <4 x i32> %m.add, ptr %address617, align 1
  %m.next = add i64 %r10.0, 4
  %c320.not = icmp eq i64 %m.next, %z278
  br i1 %c320.not, label %bb_140001a95, label %bb_140001a80
bb_140001a95:
                                     
  %c338 = icmp eq i32 %m.nvec4, %3
  br i1 %c338, label %bb_140001b0b, label %bb_140001a9a
bb_140001ab0:
                                     
  %r11.1 = phi i64 [ %d.cnt, %bb_140001ab0 ], [ %s.rem, %bb_140001a9a ]
  %r10.1 = phi i64 [ %d.next, %bb_140001ab0 ], [ %s.start64, %bb_140001a9a ]
  %element658 = getelementptr i32, ptr %1, i64 %r10.1
  %d.load = load i32, ptr %element658, align 1
  %d.sum = add i32 %d.load, %2
  %element659 = getelementptr i32, ptr %0, i64 %r10.1
  store i32 %d.sum, ptr %element659, align 1
  %d.next = add i64 %r10.1, 1
  %d.cnt = add i64 %r11.1, -1
  %c408.not = icmp eq i64 %d.cnt, 0
  br i1 %c408.not, label %bb_140001ac3, label %bb_140001ab0
bb_140001ac3:
                                     
  %r10.2 = phi i64 [ %s.start64, %bb_140001a9a ], [ %d.next, %bb_140001ab0 ]
  %t.left = sub nsw i64 %s.start64, %z
  %c437 = icmp ugt i64 %t.left, -4
  br i1 %c437, label %bb_140001b0b, label %bb_140001ad0
bb_140001ad0:
                                     
  %r10.3 = phi i64 [ %t.next, %bb_140001ad0 ], [ %r10.2, %bb_140001ac3 ]
  %element660 = getelementptr i32, ptr %1, i64 %r10.3
  %t.load = load i32, ptr %element660, align 1
  %t.sum = add i32 %t.load, %2
  %element661 = getelementptr i32, ptr %0, i64 %r10.3
  store i32 %t.sum, ptr %element661, align 1
  %sc469 = shl i64 %r10.3, 2
  %address634 = getelementptr i8, ptr %1, i64 %sc469
  %element7 = getelementptr i32, ptr %address634, i64 1
  %t.l1 = load i32, ptr %element7, align 1
  %t.s1 = add i32 %t.l1, %2
  %address637 = getelementptr i8, ptr %0, i64 %sc469
  %element8 = getelementptr i32, ptr %address637, i64 1
  store i32 %t.s1, ptr %element8, align 1
  %element9 = getelementptr i32, ptr %address634, i64 2
  %t.l2 = load i32, ptr %element9, align 1
  %t.s2 = add i32 %t.l2, %2
  %element10 = getelementptr i32, ptr %address637, i64 2
  store i32 %t.s2, ptr %element10, align 1
  %element11 = getelementptr i32, ptr %address634, i64 3
  %t.l3 = load i32, ptr %element11, align 1
  %t.s3 = add i32 %t.l3, %2
  %element12 = getelementptr i32, ptr %address637, i64 3
  store i32 %t.s3, ptr %element12, align 1
  %t.next = add i64 %r10.3, 4
  %c589.not = icmp eq i64 %t.next, %z
  br i1 %c589.not, label %bb_140001b0b, label %bb_140001ad0
bb_140001b0b:
                                     
  ret void
}
; The middle tier is gone through the shared lane-equivalence gate, and
; the countdown drain folds away through drain-tail fusion: S jumps to
; the guard block with the tail-feed phi rewired to the drain end, and
; the guard keeps its skip/exit decision over the counted tail.
; CHECK-LABEL: define{{.*}}void @avx2_add_inner
; CHECK-NOT: <4 x i32>
; CHECK: br label %bb_140001a9a
; CHECK: br label %bb_140001ac3
; CHECK-NOT: store i32 %d.sum
; CHECK: br i1 %c437, label %bb_140001b0b, label %bb_140001ad0
; CHECK: store i32 %t.sum
; CHECK: ret void
