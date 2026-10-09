; Lifted from c-gcc-o3 copy_vec in corpus/src/c/main.c:201-203.
; GCC's wide counted-copy loop uses i128 memory operations after lifting.
; Split each iteration into two i64 loads followed by two stores.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define void @copy_vec(ptr %dst, ptr %src, i64 %count) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %src.ptr = getelementptr i8, ptr %src, i64 %iv
  %wide = load i128, ptr %src.ptr, align 1
  %dst.ptr = getelementptr i8, ptr %dst, i64 %iv
  store i128 %wide, ptr %dst.ptr, align 1
  %next = add i64 %iv, 16
  %done = icmp eq i64 %next, %count
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; CHECK-LABEL: define{{.*}}void @copy_vec
; CHECK-NOT: load i128
; CHECK-NOT: store i128
; CHECK-COUNT-2: load i64
; CHECK-NOT: store i128
; CHECK-COUNT-2: store i64
; CHECK: ret void
