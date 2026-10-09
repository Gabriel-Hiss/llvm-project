; Source: corpus/src/c/main.c:209-211 (add_vec). GCC O3 uses an alias
; guard, a scalar fallback loop, a four-lane loop, then up to three scalar
; epilogue iterations. The lane-wise vector body matches scalar iterations
; 4*i + k, so the complete versioned region can fold to the fallback loop.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define void @add_vec(ptr align 4 %dst, ptr align 4 %src, i32 %value, i32 %n) {
entry:
  %small = icmp ult i32 %n, 4
  br i1 %small, label %scalar.pre, label %dispatch

dispatch:
  %dst.addr = ptrtoint ptr %dst to i64
  %src.addr = ptrtoint ptr %src to i64
  %src.next = add i64 %src.addr, 4
  %separation = sub i64 %dst.addr, %src.next
  %disjoint = icmp ugt i64 %separation, 8
  br i1 %disjoint, label %vector.pre, label %scalar.pre

scalar.pre:
  br label %scalar.loop

scalar.loop:
  %i = phi i32 [ 0, %scalar.pre ], [ %i.next, %scalar.loop ]
  %src.i = getelementptr i32, ptr %src, i32 %i
  %x = load i32, ptr %src.i, align 4
  %sum = add i32 %x, %value
  %dst.i = getelementptr i32, ptr %dst, i32 %i
  store i32 %sum, ptr %dst.i, align 4
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %exit, label %scalar.loop

vector.pre:
  %splat.e = insertelement <4 x i32> poison, i32 %value, i64 0
  %splat = shufflevector <4 x i32> %splat.e, <4 x i32> poison, <4 x i32> zeroinitializer
  %n.vec = and i32 %n, -4
  br label %vector.loop

vector.loop:
  %vi = phi i32 [ 0, %vector.pre ], [ %vi.next, %vector.loop ]
  %src.v = getelementptr i32, ptr %src, i32 %vi
  %vx = load <4 x i32>, ptr %src.v, align 4
  %vsum = add <4 x i32> %vx, %splat
  %dst.v = getelementptr i32, ptr %dst, i32 %vi
  store <4 x i32> %vsum, ptr %dst.v, align 4
  %vi.next = add i32 %vi, 4
  %vector.done = icmp eq i32 %vi.next, %n.vec
  br i1 %vector.done, label %tail.check, label %vector.loop

tail.check:
  %has.tail = icmp ult i32 %n.vec, %n
  br i1 %has.tail, label %tail.one, label %exit

tail.one:
  %src.t1 = getelementptr i32, ptr %src, i32 %n.vec
  %x.t1 = load i32, ptr %src.t1, align 4
  %sum.t1 = add i32 %x.t1, %value
  %dst.t1 = getelementptr i32, ptr %dst, i32 %n.vec
  store i32 %sum.t1, ptr %dst.t1, align 4
  %n1 = add i32 %n.vec, 1
  %more1 = icmp ult i32 %n1, %n
  br i1 %more1, label %tail.two, label %exit

tail.two:
  %src.t2 = getelementptr i32, ptr %src, i32 %n1
  %x.t2 = load i32, ptr %src.t2, align 4
  %sum.t2 = add i32 %x.t2, %value
  %dst.t2 = getelementptr i32, ptr %dst, i32 %n1
  store i32 %sum.t2, ptr %dst.t2, align 4
  %n2 = add i32 %n1, 1
  %more2 = icmp ult i32 %n2, %n
  br i1 %more2, label %tail.three, label %exit

tail.three:
  %src.t3 = getelementptr i32, ptr %src, i32 %n2
  %x.t3 = load i32, ptr %src.t3, align 4
  %sum.t3 = add i32 %x.t3, %value
  %dst.t3 = getelementptr i32, ptr %dst, i32 %n2
  store i32 %sum.t3, ptr %dst.t3, align 4
  br label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @add_vec
; CHECK-NOT: <4 x i32>
; CHECK: entry:
; CHECK: br label %scalar.pre
; CHECK: scalar.pre:
; CHECK: br label %scalar.loop
; CHECK: scalar.loop:
; CHECK: phi i32 [ 0, %scalar.pre ]
; CHECK: load i32
; CHECK: add i32
; CHECK: store i32
; CHECK: br i1
; CHECK: ret void
