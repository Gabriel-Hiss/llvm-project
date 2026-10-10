; Versioned vector loop whose body disagrees with the scalar fallback: the vector lanes add 100 while the scalar adds 1. Folding would change output, so the pass must bail and keep both arms.
; Review fixture LunaReviewDeopt/unloop-body.ll (bug board 25), pinned in the suite.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define void @add_choice(ptr %dst, ptr %src, i64 %n) {
entry:
  %small = icmp ult i64 %n, 4
  br i1 %small, label %scalar.pre, label %dispatch
dispatch:
  %choose.vector = icmp sgt i64 %n, 0
  br i1 %choose.vector, label %vector.pre, label %scalar.pre
vector.pre:
  %n.vec = and i64 %n, -4
  %splat.seed = insertelement <4 x i32> poison, i32 100, i64 0
  %splat = shufflevector <4 x i32> %splat.seed, <4 x i32> poison, <4 x i32> zeroinitializer
  br label %vector.loop
vector.loop:
  %vi = phi i64 [ 0, %vector.pre ], [ %vi.next, %vector.loop ]
  %src.v = getelementptr i32, ptr %src, i64 %vi
  %vx = load <4 x i32>, ptr %src.v, align 4
  %wrong = add <4 x i32> %vx, %splat
  %dst.v = getelementptr i32, ptr %dst, i64 %vi
  store <4 x i32> %wrong, ptr %dst.v, align 4
  %vi.next = add i64 %vi, 4
  %vector.done = icmp eq i64 %vi.next, %n.vec
  br i1 %vector.done, label %exit, label %vector.loop
scalar.pre:
  br label %scalar.loop
scalar.loop:
  %si = phi i64 [ 0, %scalar.pre ], [ %si.next, %scalar.loop ]
  %src.i = getelementptr i32, ptr %src, i64 %si
  %x = load i32, ptr %src.i, align 4
  %right = add i32 %x, 1
  %dst.i = getelementptr i32, ptr %dst, i64 %si
  store i32 %right, ptr %dst.i, align 4
  %si.next = add i64 %si, 1
  %scalar.done = icmp eq i64 %si.next, %n
  br i1 %scalar.done, label %exit, label %scalar.loop
exit:
  ret void
}

; CHECK-LABEL: define{{.*}}void @add_choice
; CHECK: entry:
; CHECK: br label %scalar.pre
; CHECK-NOT: vector.loop
; CHECK: scalar.loop:
; CHECK: add i32 %x, 1
; CHECK: ret void
; CHECK: }
