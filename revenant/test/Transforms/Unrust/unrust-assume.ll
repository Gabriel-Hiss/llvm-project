; Corpus: rust-o3 artifacts/corpus_rust.post.ll @vec_index keeps
; `llvm.assume` facts (`icmp ne %buf, null`) after the optimizer deletes
; the dead branches. The pass materializes a plain ult compare next to
; the assume when the function still calls panic_bounds_check.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @llvm.assume(i1)
declare void @panic_bounds_check(i64, i64, ptr)

define i64 @assumed(i64 %i, i64 %len, ptr %buf) {
; CHECK-LABEL: define i64 @assumed(
entry:
  %c = icmp ule i64 %i, %len, !revenant.va !{i64 4096}
  call void @llvm.assume(i1 %c)
  call void @panic_bounds_check(i64 %i, i64 %len, ptr %buf)
  ret i64 %i
; The assume fact gets a standalone compare next to it.
; CHECK: %c.unrust = icmp ule i64 %i, %len
; CHECK: call void @llvm.assume(i1 %c)
}
