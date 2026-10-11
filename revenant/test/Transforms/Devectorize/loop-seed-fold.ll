; A reduction accumulator with a non-zero constant seed: the acc PHI starts
; at splat(3) instead of zeroinitializer. laneConst must yield the per-lane
; seed (a ConstantDataVector is what ConstantVector::get now returns for
; integer-element vectors) so the scalar accumulator is seeded with the lane
; sum and the reduce aliases to the chain end.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_seeded(ptr %base, i64 %n) {
entry:
  %wide = and i64 %n, 9223372036854775800
  %end = getelementptr i8, ptr %base, i64 %wide
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %vacc = phi <4 x i32> [ splat (i32 3), %entry ], [ %vadd, %loop ]
  %addr = getelementptr i8, ptr %base, i64 %iv
  %v = load <4 x i32>, ptr %addr, align 4
  %vadd = add <4 x i32> %v, %vacc
  %iv.next = add i64 %iv, 16
  %done = icmp eq i64 %iv.next, %wide
  br i1 %done, label %exit, label %loop

exit:
  %r = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %vadd)
  ret i32 %r
}

; The seed is the four-lane sum of the constant (3+3+3+3) computed in the
; entry block; the scalar acc is seeded with it and no vector type survives.
; CHECK: s.unopt.seed = add i32 3, 3
; CHECK: s.unopt.seed1 = add i32 {{.*}}, 3
; CHECK: s.unopt.seed2 = add i32 {{.*}}, 3
; CHECK: s.unopt = phi i32 [ %s.unopt.seed2, %entry ], [ {{.*}}, %loop ]
; CHECK-NOT: <4 x i32>
; CHECK-NOT: llvm.vector.reduce
; CHECK: ret i32
