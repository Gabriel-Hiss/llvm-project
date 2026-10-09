; A loop-vectorized i32 sum reduction: one <4 x i32> acc PHI over two
; vector loads, lane-wise adds, and an exit llvm.vector.reduce.add.
; The loop explodes into per-lane scalar adds feeding one scalar acc
; chain; the reduce aliases to the chain end. Mirrors the sum_vec loop in
; cpp-clang-o2/artifacts/main.post.ll (simplified to one acc).
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)

define i32 @sum_vec(ptr %base, i64 %n) {
entry:
  %wide = and i64 %n, 9223372036854775800
  %end = getelementptr i8, ptr %base, i64 %wide
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %vacc = phi <4 x i32> [ zeroinitializer, %entry ], [ %vadd, %loop ]
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

; The vector PHI is gone; one scalar acc phi remains.
; CHECK: s.unopt = phi i32 [ 0, %entry ], [ {{.*}}, %loop ]
; Per-lane scalar adds feed the chain.
; CHECK: add i32 {{.*}}, {{.*}}
; The reduce is gone, replaced by the scalar chain end.
; CHECK-NOT: llvm.vector.reduce
; CHECK-NOT: <4 x i32>
; CHECK: ret i32
