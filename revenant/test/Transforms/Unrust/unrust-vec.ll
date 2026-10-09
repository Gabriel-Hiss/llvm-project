; Corpus: rust-o3 artifacts/corpus_rust.pre.ll @vec_ops keeps the
; `{ptr, cap, len}` triple allocas with the grow_one invoke; post SROAs
; the triple into scalar phis. The pass rebuilds a 24-byte frame when a
; buffer GEP survives without one.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

define i64 @alloc_vec_shape(ptr %buf, i64 %len) {
; CHECK-LABEL: define i64 @alloc_vec_shape(
entry:
  br label %loop
loop:
  %p = phi ptr [ %buf, %entry ], [ %g, %loop ]
  %e = getelementptr inbounds i8, ptr %p, i64 8, !revenant.va !{i64 4096}
  %v = load i64, ptr %e, align 8
  %g = getelementptr inbounds i64, ptr %p, i64 %len
  %x = load i64, ptr %g, align 8
  %s = add i64 %v, %x
  br i1 true, label %loop, label %exit
exit:
  ret i64 %s
; CHECK: %vec.unrust = alloca [24 x i8], align 8
}
