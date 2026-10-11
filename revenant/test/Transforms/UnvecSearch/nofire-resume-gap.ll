; resume-identity: NO-FIRE: the tail resumes at %resume.ext = zext(%next + 4) = 36 while the vector covered bytes 0..31, skipping a hit at 33. The resume gate requires the dead-region incoming to be exactly the latch step or the chunk bound SSA value; arithmetic on them bails.
; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s


define i32 @resume-identity(ptr %base, i64 %n, i32 %needle) {
entry:
  %wide = zext i32 %needle to i128
  %packed = bitcast i128 %wide to <16 x i8>
  %splat = shufflevector <16 x i8> %packed, <16 x i8> poison, <16 x i32> zeroinitializer
  %n32 = trunc i64 %n to i32
  %small = icmp slt i32 %n32, 16
  br i1 %small, label %tail, label %pre

pre:
  %bound = and i32 %n32, -32
  br label %chunk

latch:
  %next = add i32 %iv, 16
  %done = icmp eq i32 %bound, %next
  %resume = add i32 %next, 4
  %resume.ext = zext i32 %resume to i64
  %ptr.next = getelementptr i8, ptr %ptr, i64 16
  br i1 %done, label %tail, label %chunk

chunk:
  %ptr = phi ptr [ %base, %pre ], [ %ptr.next, %latch ]
  %iv = phi i32 [ 0, %pre ], [ %next, %latch ]
  %v = load <16 x i8>, ptr %ptr, align 1
  %cmp = icmp eq <16 x i8> %v, %splat
  %mask = bitcast <16 x i1> %cmp to i16
  %zero = icmp eq i16 %mask, 0
  br i1 %zero, label %latch, label %found

found:
  %bit = call i16 @llvm.cttz.i16(i16 %mask, i1 true)
  %bit32 = zext i16 %bit to i32
  %result = or i32 %iv, %bit32
  ret i32 %result

tail:
  %ti = phi i64 [ 0, %entry ], [ %resume.ext, %latch ], [ %ti.next, %tail.latch ]
  %tp = getelementptr i8, ptr %base, i64 %ti
  %b = load i8, ptr %tp, align 1
  %needle8 = trunc i32 %needle to i8
  %equal = icmp eq i8 %b, %needle8
  br i1 %equal, label %tail.found, label %tail.latch

tail.latch:
  %ti.next = add i64 %ti, 1
  %tdone = icmp eq i64 %ti.next, %n
  br i1 %tdone, label %tail.miss, label %tail

tail.found:
  %result.tail = trunc i64 %ti to i32
  ret i32 %result.tail

tail.miss:
  ret i32 -1
}

; CHECK-LABEL: define i32 @resume-identity(
; The chunk scan skeleton survives: the pass must not fold.
; CHECK: icmp eq <16 x i8>
; CHECK: call {{.*}} @llvm.cttz
