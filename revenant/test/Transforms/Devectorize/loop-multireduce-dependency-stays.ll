; One reduction update consumes another accumulator PHI, so no per-acc
; scalar chain can explode its stream: %next.b adds %acc.a (an acc PHI),
; and the lane exploder has no PHI case. The pass must bail WITHOUT
; mutating (verbatim from rv-scratch/LunaReviewDeopt/multi-acc-dependency.ll
; minus main; baseline exit 0, buggy pass aborted the verifier with
; "PHINode should have one entry for each predecessor ... s.unopt1").
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_dependency(ptr %a) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %acc.a = phi <4 x i32> [ zeroinitializer, %entry ], [ %next.a, %loop ]
  %acc.b = phi <4 x i32> [ zeroinitializer, %entry ], [ %next.b, %loop ]
  %ptr.a = getelementptr i8, ptr %a, i64 %iv
  %vec.a = load <4 x i32>, ptr %ptr.a, align 4
  %next.a = add <4 x i32> %acc.a, %vec.a
  %next.b = add <4 x i32> %acc.b, %acc.a
  %iv.next = add i64 %iv, 16
  %done = icmp eq i64 %iv.next, 32
  br i1 %done, label %exit, label %loop
exit:
  %red.a = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.a)
  %red.b = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.b)
  %sum = add i32 %red.a, %red.b
  ret i32 %sum
}

; The loop stays fully vector; no scalar accumulator is created.
; CHECK: %acc.a = phi <4 x i32>
; CHECK: %acc.b = phi <4 x i32>
; CHECK: %next.b = add <4 x i32> %acc.b, %acc.a
; CHECK: %red.a = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.a)
; CHECK: %red.b = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.b)
; CHECK-NOT: s.unopt
; CHECK: ret i32 %sum
