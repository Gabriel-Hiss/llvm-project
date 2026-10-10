; Two independent i32 reduction accumulators with separate reduces and an
; outer scalar add must NOT merge: giving both reduces the same merged
; chain would double the total (sum(a)+sum(b) -> 2*sum(a+b)). The loop
; stays fully vector. Found by LunaReviewDeopt (miscompile: 110 -> 220).
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-devectorize -S %s | %FileCheck %s

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

; CHECK: %acc.a = phi <4 x i32>
; CHECK: %acc.b = phi <4 x i32>
; CHECK: %red.a = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.a)
; CHECK: %red.b = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %next.b)
; CHECK: %sum = add i32 %red.a, %red.b
; CHECK-NOT: s.unopt
