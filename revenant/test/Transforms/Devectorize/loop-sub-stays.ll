; Negative: a sub-reduction loop is not a lane-wise reduction; the loop
; stays vector. Ports the reference bail on non-red ops.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_sub(ptr %base, i64 %n) {
entry:
  %wide = and i64 %n, 9223372036854775800
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %vacc = phi <4 x i32> [ zeroinitializer, %entry ], [ %vsub, %loop ]
  %addr = getelementptr i8, ptr %base, i64 %iv
  %v = load <4 x i32>, ptr %addr, align 4
  %vsub = sub <4 x i32> %v, %vacc
  %iv.next = add i64 %iv, 16
  %done = icmp eq i64 %iv.next, %wide
  br i1 %done, label %exit, label %loop

exit:
  %r = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %vsub)
  ret i32 %r
}

; CHECK: phi <4 x i32>
; CHECK: sub <4 x i32>
; CHECK: llvm.vector.reduce.add
