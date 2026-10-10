; Two independent i32 sum reductions over different arrays, combined by an
; outer add. Each reduce must alias to its own scalar accumulator chain;
; sharing one merged chain doubles the total (multi-reduce.ll: baseline
; exit 0, buggy exit 1). Verbatim from
; rv-scratch/LunaReviewDeopt/multi-reduce.ll (minus main).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum2(ptr %a, ptr %b) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %acc.a = phi <4 x i32> [ zeroinitializer, %entry ], [ %next.a, %loop ]
  %acc.b = phi <4 x i32> [ zeroinitializer, %entry ], [ %next.b, %loop ]
  %ptr.a = getelementptr i8, ptr %a, i64 %iv
  %ptr.b = getelementptr i8, ptr %b, i64 %iv
  %vec.a = load <4 x i32>, ptr %ptr.a, align 4
  %vec.b = load <4 x i32>, ptr %ptr.b, align 4
  %next.a = add <4 x i32> %acc.a, %vec.a
  %next.b = add <4 x i32> %acc.b, %vec.b
  %iv.next = add i64 %iv, 16
  %done = icmp eq i64 %iv.next, 16
  br i1 %done, label %exit, label %loop
exit:
  %red.a = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.a)
  %red.b = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.b)
  %sum = add i32 %red.a, %red.b
  ret i32 %sum
}

; Two scalar accumulators, one per reduction.
; CHECK: s.unopt1 = phi i32 [ 0, %entry ]
; CHECK: s.unopt = phi i32 [ 0, %entry ]
; No vector code remains and both reduces are gone.
; CHECK-NOT: <4 x i32>
; CHECK-NOT: llvm.vector.reduce
; CHECK: ret i32
