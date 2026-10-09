; Lifted from c-gcc-o3 fill_vec in corpus/src/c/main.c:205-207.
; GCC O3 uses a counted loop of repeated <4 x i32> stores. Expand the
; store lanes without changing the counted-loop coverage.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unloopvec -S %s | %FileCheck %s

define void @fill_vec(ptr %dst, i32 %value, i64 %count) {
entry:
  %splat.elt = insertelement <4 x i32> poison, i32 %value, i64 0
  %splat = shufflevector <4 x i32> %splat.elt, <4 x i32> poison, <4 x i32> zeroinitializer
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %next, %loop ]
  %ptr = getelementptr i32, ptr %dst, i64 %iv
  store <4 x i32> %splat, ptr %ptr, align 4
  %next = add i64 %iv, 4
  %done = icmp eq i64 %next, %count
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

; CHECK-NOT: store <4 x i32>
; CHECK-COUNT-4: store i32 %value
; CHECK: ret void
