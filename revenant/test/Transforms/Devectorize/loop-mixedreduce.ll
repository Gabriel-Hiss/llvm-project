; A direct reduce plus a second reduce over the same acc backedge combined
; with itself: reduce(x+x) doubles the sum, so the combined reduce aliases to
; Chain OP Chain, not Chain (verbatim from
; rv-scratch/LunaReviewDeopt/devectorize-mixed-reduce-roots.ll minus main;
; baseline exit 24, buggy exit 16). Two iterations over eight ones: direct = 8,
; doubled = 16, total 24.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_mixed(ptr %values) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %nextiv, %loop ]
  %acc = phi <4 x i32> [ zeroinitializer, %entry ], [ %nextacc, %loop ]
  %p = getelementptr i32, ptr %values, i64 %iv
  %v = load <4 x i32>, ptr %p, align 16
  %nextacc = add <4 x i32> %acc, %v
  %nextiv = add i64 %iv, 4
  %done = icmp eq i64 %nextiv, 8
  br i1 %done, label %exit, label %loop
exit:
  %direct = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %nextacc)
  %twice = add <4 x i32> %nextacc, %nextacc
  %doubled = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %twice)
  %sum = add i32 %direct, %doubled
  ret i32 %sum
}

; One scalar accumulator; the direct reduce aliases the chain end and the
; combined reduce aliases to the chain added to itself.
; CHECK: s.unopt = phi i32 [ 0, %entry ]
; CHECK: s.unopt.m = add i32 {{.*}}, {{.*}}
; No vector code remains and both reduces are gone.
; CHECK-NOT: <4 x i32>
; CHECK-NOT: llvm.vector.reduce
; CHECK: ret i32
