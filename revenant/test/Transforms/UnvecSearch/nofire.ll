; No-fire cases: shapes the scan fold must leave alone.
; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s --allow-unused-prefixes

; A counted vector add loop (UnloopVec territory: masked trip bound, no
; mask+bit-scan skeleton) is untouched.
define void @counted_add(ptr %dst, ptr %src, i32 %bias, i32 %n) {
; CHECK-LABEL: define void @counted_add(
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
; CHECK: vector:
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

; A plain scalar search loop (no vector skeleton at all) is untouched.
define i32 @scalar_find(ptr %p, i32 %n, i8 %want) {
; CHECK-LABEL: define i32 @scalar_find(
entry:
  br label %loop

loop:
; CHECK: loop:
  %i = phi i32 [ 0, %entry ], [ %next, %nextb ]
  %done = icmp eq i32 %i, %n
  br i1 %done, label %miss, label %body

body:
  %elt = getelementptr i8, ptr %p, i32 %i
  %v = load i8, ptr %elt, align 1
  %hit = icmp eq i8 %v, %want
  br i1 %hit, label %found, label %nextb

nextb:
  %next = add i32 %i, 1
  br label %loop

found:
  ret i32 %i

miss:
  ret i32 -1
}

; A chunk loop whose "needle" varies per chunk (an add over a second chunk
; load, not a proven splat of one root) is not a scan and stays put.
define i32 @varying_needle(ptr %p, ptr %q, i32 %n) {
; CHECK-LABEL: define i32 @varying_needle(
entry:
  br label %chunk

chunk:
; CHECK: chunk:
  %i = phi i32 [ 0, %entry ], [ %step, %nextc ]
  %cp = getelementptr i8, ptr %p, i32 %i
  %qp = getelementptr i8, ptr %q, i32 %i
  %cv = load <16 x i8>, ptr %cp, align 1
  %nv = load <16 x i8>, ptr %qp, align 1
  %one = insertelement <16 x i8> poison, i8 1, i64 0
  %ones = shufflevector <16 x i8> %one, <16 x i8> poison, <16 x i32> zeroinitializer
  %other = add <16 x i8> %nv, %ones
  %eq = icmp eq <16 x i8> %cv, %other
  %mask = bitcast <16 x i1> %eq to i16
  %empty = icmp eq i16 %mask, 0
  br i1 %empty, label %nextc, label %hit

nextc:
  %step = add i32 %i, 16
  %more = icmp slt i32 %step, %n
  br i1 %more, label %chunk, label %miss

hit:
  %idx = call i16 @llvm.cttz.i16(i16 %mask, i1 true)
  %r = zext i16 %idx to i32
  %res = or i32 %i, %r
  ret i32 %res

miss:
  ret i32 -1
}

declare i16 @llvm.cttz.i16(i16, i1 immarg)
