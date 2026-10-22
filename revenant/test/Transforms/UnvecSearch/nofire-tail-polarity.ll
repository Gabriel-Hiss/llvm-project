; RUN: %revenant-opt -passes=revenant-unvecsearch,verify -S %s | %FileCheck %s --allow-unused-prefixes
; The SIMD chunk loop searches for the first equal byte. The scalar tail
; below instead returns the first mismatching byte. matchTailLoop currently
; checks only for an equality compare and step-1 IV, not branch polarity.

declare i16 @llvm.cttz.i16(i16, i1 immarg)

define i32 @tailpolarity(ptr %base, i64 %n, i32 %needle) {
entry:
  %wide = zext i32 %needle to i128
  %packed = bitcast i128 %wide to <16 x i8>
  %splat = shufflevector <16 x i8> %packed, <16 x i8> poison, <16 x i32> zeroinitializer
  %n32 = trunc i64 %n to i32
  %small = icmp slt i32 %n32, 16
  br i1 %small, label %tail, label %pre

pre:
  %bound = and i32 %n32, 2147483632
  br label %chunk

latch:
  %next = add i32 %iv, 16
  %done = icmp eq i32 %bound, %next
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
  %ti = phi i64 [ 0, %entry ], [ 0, %latch ], [ %ti.next, %tail ]
  %tp = getelementptr i8, ptr %base, i64 %ti
  %b = load i8, ptr %tp, align 1
  %needle8 = trunc i32 %needle to i8
  %equal = icmp eq i8 %b, %needle8
  %ti.next = add i64 %ti, 1
  %tdone = icmp eq i64 %ti.next, %n
  br i1 %equal, label %tail, label %tail.exit

tail.exit:
  %result.tail = trunc i64 %ti to i32
  ret i32 %result.tail
}


; CHECK-LABEL: define i32 @tailpolarity(
; The chunk scan skeleton survives: the pass must not fold.
; CHECK: icmp eq <16 x i8>
; CHECK: call {{.*}} @llvm.cttz
