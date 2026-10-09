; i32 trip count with the sub/urem n.vec form plus a loop-exit LCSSA merge
; fed from the vector region: the reduce value reaches %s.lcssa through
; the middle.block edge. The escape rule lets R-edge incomings drop when
; the merge keeps its scalar-loop edge. Corpus shape: probe built from
; the clang vectorizer skeleton (scalar tail %loop, i32 N).
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define i32 @sum(ptr noundef %p, i32 noundef %n) {
entry:
  %c0 = icmp sgt i32 %n, 0
  br i1 %c0, label %loop.ph, label %exit

loop.ph:
  %min.iters.check = icmp ult i32 %n, 8
  br i1 %min.iters.check, label %scalar.ph, label %vector.ph

vector.ph:
  %n.mod.vf = urem i32 %n, 8
  %n.vec = sub i32 %n, %n.mod.vf
  br label %vector.body

vector.body:
  %index = phi i32 [ 0, %vector.ph ], [ %index.next, %vector.body ]
  %vec.phi = phi <4 x i32> [ zeroinitializer, %vector.ph ], [ %2, %vector.body ]
  %vec.phi1 = phi <4 x i32> [ zeroinitializer, %vector.ph ], [ %3, %vector.body ]
  %0 = getelementptr i32, ptr %p, i32 %index
  %1 = getelementptr i32, ptr %0, i64 4
  %wide.load = load <4 x i32>, ptr %0, align 4
  %wide.load2 = load <4 x i32>, ptr %1, align 4
  %2 = add <4 x i32> %vec.phi, %wide.load
  %3 = add <4 x i32> %vec.phi1, %wide.load2
  %index.next = add nuw i32 %index, 8
  %4 = icmp eq i32 %index.next, %n.vec
  br i1 %4, label %middle.block, label %vector.body

middle.block:
  %bin.rdx = add <4 x i32> %3, %2
  %5 = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %bin.rdx)
  %cmp.n = icmp eq i32 %n, %n.vec
  br i1 %cmp.n, label %loopexit, label %scalar.ph

scalar.ph:
  %bc.resume.val = phi i32 [ %n.vec, %middle.block ], [ 0, %loop.ph ]
  %bc.merge.rdx = phi i32 [ %5, %middle.block ], [ 0, %loop.ph ]
  br label %loop

loop:
  %i = phi i32 [ %bc.resume.val, %scalar.ph ], [ %i.next, %loop ]
  %s = phi i32 [ %bc.merge.rdx, %scalar.ph ], [ %s.next, %loop ]
  %gep = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %gep, align 4
  %s.next = add i32 %s, %v
  %i.next = add i32 %i, 1
  %c = icmp slt i32 %i.next, %n
  br i1 %c, label %loop, label %loopexit

loopexit:
  %s.lcssa = phi i32 [ %s.next, %loop ], [ %5, %middle.block ]
  br label %exit

exit:
  %r = phi i32 [ 0, %entry ], [ %s.lcssa, %loopexit ]
  ret i32 %r
}

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

; The vector tiers are gone; the scalar loop and the exit merge remain.
; CHECK-LABEL: define{{.*}}i32 @sum
; CHECK-NOT: <4 x i32>
; CHECK-NOT: call i32 @llvm.vector.reduce
; CHECK: load i32
; CHECK: }
