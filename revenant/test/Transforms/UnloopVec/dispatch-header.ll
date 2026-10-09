; Dispatch-to-header shapes from the normalized lifted IR: the lifter
; jumps straight into the scalar loop header, so the header's phis
; carry both the start/resume values and the backedge.
;
; @fill_header is the lifted c-clang-o2 fill_vec shape: the vector
; bound is the scaled byte form shl(zext(lshr(N, 3)), 5) and the
; resume phi feeds n.vec through an and-mask sibling of the bound.
;
; @add_guarded is the lifted c-o2 add_vec shape: the alias-check chain
; restarts the header at 0 mid-dispatch, and the vector latch's exit
; value (IV.next == n.vec) feeds the header resume edge.
;
; RUN: %revenant-opt -passes=revenant-unloopvec -S %s | %FileCheck %s

define void @fill_header(ptr %dst, i32 %value, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %dispatch

dispatch:
  %z = zext nneg i32 %n to i64
  %big = icmp ugt i32 %n, 7
  br i1 %big, label %vector.pre, label %scalar

vector.pre:
  %n.vec = and i32 %n, -8
  %z.vec = zext nneg i32 %n.vec to i64
  %splat.e = insertelement <4 x i32> poison, i32 %value, i64 0
  %splat = shufflevector <4 x i32> %splat.e, <4 x i32> poison, <4 x i32> zeroinitializer
  %groups = lshr i32 %n, 3
  %z.groups = zext nneg i32 %groups to i64
  %bound = shl nuw nsw i64 %z.groups, 5
  br label %vector.body

vector.body:
  %iv = phi i64 [ 0, %vector.pre ], [ %iv.next, %vector.body ]
  %addr = getelementptr i8, ptr %dst, i64 %iv
  store <4 x i32> %splat, ptr %addr, align 1
  %addr2 = getelementptr <4 x i32>, ptr %addr, i64 1
  store <4 x i32> %splat, ptr %addr2, align 1
  %iv.next = add i64 %iv, 32
  %done = icmp eq i64 %bound, %iv.next
  br i1 %done, label %middle, label %vector.body

middle:
  %covered = icmp eq i32 %n.vec, %n
  br i1 %covered, label %exit, label %scalar

scalar:
  %i = phi i64 [ 0, %dispatch ], [ %i.next, %scalar ], [ %z.vec, %middle ]
  %slot = getelementptr i32, ptr %dst, i64 %i
  store i32 %value, ptr %slot, align 1
  %i.next = add i64 %i, 1
  %tail.done = icmp eq i64 %i.next, %z
  br i1 %tail.done, label %exit, label %scalar

exit:
  ret void
}

define i64 @add_guarded(ptr %dst, ptr %src, i32 %value, i32 %n) {
entry:
  %empty = icmp slt i32 %n, 1
  br i1 %empty, label %exit, label %dispatch

dispatch:
  %small = icmp ult i32 %n, 16
  br i1 %small, label %scalar, label %guard1

guard1:
  %dst.addr = ptrtoint ptr %dst to i64
  %src.addr = ptrtoint ptr %src to i64
  %src.end = add i64 %src.addr, 4
  %sep = sub i64 %dst.addr, %src.end
  %ok1 = icmp ugt i64 %sep, 8
  br i1 %ok1, label %vector.pre, label %guard2

guard2:
  %sep2 = sub i64 %src.addr, %dst.addr
  %ok2 = icmp ugt i64 %sep2, 8
  br i1 %ok2, label %vector.pre, label %scalar

vector.pre:
  %n.vec = and i32 %n, -16
  br label %vector.body

vector.body:
  %iv = phi i32 [ 0, %vector.pre ], [ %iv.next, %vector.body ]
  %iv.w = sext i32 %iv to i64
  %iv.bytes = shl nsw i64 %iv.w, 2
  %src.a = getelementptr i8, ptr %src, i64 %iv.bytes
  %w = load i128, ptr %src.a, align 1
  %wl = trunc i128 %w to i64
  %vs = zext i32 %value to i64
  %sum = add i64 %wl, %vs
  %sum.w = zext i64 %sum to i128
  %dst.a = getelementptr i8, ptr %dst, i64 %iv.bytes
  store i128 %sum.w, ptr %dst.a, align 1
  %iv.next = add i32 %iv, 16
  %more = icmp slt i32 %iv.next, %n.vec
  br i1 %more, label %vector.body, label %middle

middle:
  %resume = zext i32 %iv.next to i64
  %tail = icmp slt i32 %iv.next, %n
  br i1 %tail, label %scalar, label %exit

scalar:
  %i = phi i32 [ 0, %dispatch ], [ 0, %guard2 ], [ %i.next, %scalar ], [ %iv.next, %middle ]
  %i.w = sext i32 %i to i64
  %i.next = add i32 %i, 1
  %i.z = zext i32 %i.next to i64
  %src.s = getelementptr i32, ptr %src, i64 %i.w
  %x = load i32, ptr %src.s, align 1
  %y = add i32 %x, %value
  %dst.s = getelementptr i32, ptr %dst, i64 %i.w
  store i32 %y, ptr %dst.s, align 1
  %done = icmp slt i32 %i.next, %n
  br i1 %done, label %scalar, label %exit

exit:
  %r = phi i64 [ undef, %entry ], [ %i.z, %scalar ], [ %resume, %middle ]
  ret i64 %r
}

; The vector tier is gone; the scalar header loop remains.
; CHECK-LABEL: define{{.*}}void @fill_header
; CHECK-NOT: <4 x i32>
; CHECK: scalar:
; CHECK: store i32 %value
; CHECK: }
; CHECK-LABEL: define{{.*}}i64 @add_guarded
; CHECK-NOT: i128
; CHECK: scalar:
; CHECK: store i32 %y
; CHECK: }
