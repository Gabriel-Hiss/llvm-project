; Predicated reduction: the vector body selects per-lane on a vector icmp
; before reducing. The select lanes are ordinary erasable vector tier
; instructions, so the whole region still collapses to the scalar loop.
; Corpus shape: probe built from the clang vectorizer skeleton with a
; per-element condition (scalar tail %loop with select).
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define i32 @condsum(ptr noundef %p, ptr noundef %q, i32 noundef %n) {
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
  %vec.phi = phi <4 x i32> [ zeroinitializer, %vector.ph ], [ %8, %vector.body ]
  %vec.phi1 = phi <4 x i32> [ zeroinitializer, %vector.ph ], [ %9, %vector.body ]
  %0 = getelementptr i32, ptr %p, i32 %index
  %1 = getelementptr i32, ptr %0, i64 4
  %wide.load = load <4 x i32>, ptr %0, align 4
  %wide.load2 = load <4 x i32>, ptr %1, align 4
  %2 = getelementptr i32, ptr %q, i32 %index
  %3 = getelementptr i32, ptr %2, i64 4
  %wide.load3 = load <4 x i32>, ptr %2, align 4
  %wide.load4 = load <4 x i32>, ptr %3, align 4
  %4 = icmp sgt <4 x i32> %wide.load3, zeroinitializer
  %5 = icmp sgt <4 x i32> %wide.load4, zeroinitializer
  %6 = add <4 x i32> %vec.phi, %wide.load
  %7 = add <4 x i32> %vec.phi1, %wide.load2
  %8 = select <4 x i1> %4, <4 x i32> %6, <4 x i32> %vec.phi
  %9 = select <4 x i1> %5, <4 x i32> %7, <4 x i32> %vec.phi1
  %index.next = add nuw i32 %index, 8
  %10 = icmp eq i32 %index.next, %n.vec
  br i1 %10, label %middle.block, label %vector.body

middle.block:
  %bin.rdx = add <4 x i32> %9, %8
  %11 = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %bin.rdx)
  %cmp.n = icmp eq i32 %n, %n.vec
  br i1 %cmp.n, label %loopexit, label %scalar.ph

scalar.ph:
  %bc.resume.val = phi i32 [ %n.vec, %middle.block ], [ 0, %loop.ph ]
  %bc.merge.rdx = phi i32 [ %11, %middle.block ], [ 0, %loop.ph ]
  br label %loop

loop:
  %i = phi i32 [ %bc.resume.val, %scalar.ph ], [ %i.next, %loop ]
  %s = phi i32 [ %bc.merge.rdx, %scalar.ph ], [ %s.next, %loop ]
  %gep = getelementptr i32, ptr %p, i32 %i
  %v = load i32, ptr %gep, align 4
  %gepq = getelementptr i32, ptr %q, i32 %i
  %vq = load i32, ptr %gepq, align 4
  %c = icmp sgt i32 %vq, 0
  %add = add i32 %s, %v
  %s.next = select i1 %c, i32 %add, i32 %s
  %i.next = add i32 %i, 1
  %cc = icmp slt i32 %i.next, %n
  br i1 %cc, label %loop, label %loopexit

loopexit:
  %s.lcssa = phi i32 [ %s.next, %loop ], [ %11, %middle.block ]
  br label %exit

exit:
  %r = phi i32 [ 0, %entry ], [ %s.lcssa, %loopexit ]
  ret i32 %r
}

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

; The predicated vector body and the middle reduce are gone.
; CHECK-LABEL: define{{.*}}i32 @condsum
; CHECK-NOT: <4 x i32>
; CHECK-NOT: call i32 @llvm.vector.reduce
; CHECK: load i32
; CHECK: }
