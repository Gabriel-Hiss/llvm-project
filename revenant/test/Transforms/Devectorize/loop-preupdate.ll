; The exit reduces the accumulator PHI (pre-update value on the exit edge),
; not the backedge. The PHI-rooted reduce maps to the scalar accumulator PHI
; (start-of-iteration value); mapping it to the chain end wrongly includes
; the last body update (verbatim from
; rv-scratch/LunaReviewDeopt/devectorize-preupdate-reduce.ll minus main;
; one iteration over four ones: baseline exit 0, buggy exit 4).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_previous(ptr %values) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %nextiv, %loop ]
  %acc = phi <4 x i32> [ zeroinitializer, %entry ], [ %nextacc, %loop ]
  %p = getelementptr i32, ptr %values, i64 %iv
  %v = load <4 x i32>, ptr %p, align 16
  %nextacc = add <4 x i32> %acc, %v
  %nextiv = add i64 %iv, 4
  %done = icmp eq i64 %nextiv, 4
  br i1 %done, label %exit, label %loop
exit:
  %sum = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %acc)
  ret i32 %sum
}

; The PHI-rooted reduce aliases the scalar accumulator PHI, not the chain.
; CHECK: s.unopt = phi i32 [ 0, %entry ]
; CHECK: ret i32 %s.unopt
; The reduce is gone and no vector code remains.
; CHECK-NOT: <4 x i32>
; CHECK-NOT: llvm.vector.reduce
