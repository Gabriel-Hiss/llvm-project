; Adapted from c-clang-o2 add_vec: guarded vector body plus scalar fallback
; and unrolled scalar remainder. The fallback starts at zero on dispatch and
; covers the whole trip count, so the vector arm and its middle block are dead.
; RUN: %revenant-opt -passes=revenant-unloopvec,verify -S %s | %FileCheck %s

define void @add_replay(ptr %dst, ptr %src, i32 %bias, i32 %n) {
entry:
  %positive = icmp sgt i32 %n, 0
  br i1 %positive, label %dispatch, label %exit

dispatch:
  %small = icmp ult i32 %n, 4
  br i1 %small, label %scalar.pre, label %vector.pre

vector.pre:
  %bound = and i32 %n, -4
  br label %vector

vector:
  %iv = phi i32 [ 0, %vector.pre ], [ %next, %vector ]
  %sp = getelementptr i32, ptr %src, i32 %iv
  %dp = getelementptr i32, ptr %dst, i32 %iv
  %vl = load <4 x i32>, ptr %sp, align 4
  %vb = insertelement <4 x i32> poison, i32 %bias, i64 0
  %vs = shufflevector <4 x i32> %vb, <4 x i32> poison, <4 x i32> zeroinitializer
  %va = add <4 x i32> %vl, %vs
  store <4 x i32> %va, ptr %dp, align 4
  %next = add i32 %iv, 4
  %done = icmp eq i32 %next, %bound
  br i1 %done, label %middle, label %vector

middle:
  %full = icmp eq i32 %bound, %n
  br i1 %full, label %exit, label %scalar.pre

scalar.pre:
  %start = phi i32 [ 0, %dispatch ], [ %bound, %middle ]
  br label %scalar

scalar:
  %i = phi i32 [ %start, %scalar.pre ], [ %i.next, %scalar ]
  %sp.s = getelementptr i32, ptr %src, i32 %i
  %dp.s = getelementptr i32, ptr %dst, i32 %i
  %x = load i32, ptr %sp.s, align 4
  %y = add i32 %x, %bias
  store i32 %y, ptr %dp.s, align 4
  %i.next = add i32 %i, 1
  %tail.done = icmp eq i32 %i.next, %n
  br i1 %tail.done, label %exit, label %scalar

exit:
  ret void
}

; CHECK-LABEL: define void @add_replay
; CHECK-NOT: <4 x i32>
; CHECK-NOT: label %vector
; CHECK: %start = phi i32 [ 0, %dispatch ]
; CHECK: %y = add i32 %x, %bias
; CHECK: store i32 %y
; CHECK: }
