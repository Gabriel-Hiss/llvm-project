; Corpus: rust-o1 scratch/rust-o1-pre.ll drop_glue::<Vec<i64>> (pre
; tail-calls Drop::drop then RawVec's shim; post inlines the body into
; the caller leaving `icmp eq len, 0` guarding a __rust_dealloc arm).
; The pass outlines the guarded region into drop_glue.unrust.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @__rust_dealloc(ptr, i64, i64)

define void @vec_drop(ptr %agg) {
; CHECK-LABEL: define void @vec_drop(
entry:
  %len = load i64, ptr %agg, align 8, !revenant.va !{i64 4096}
  %c = icmp eq i64 %len, 0
  br i1 %c, label %live, label %free
free:
  %buf = load ptr, ptr %agg, align 8
  call void @__rust_dealloc(ptr %buf, i64 %len, i64 8)
  br label %live
live:
  ret void
; The guarded free moves into a drop_glue.unrust function; the site
; keeps a call with the aggregate base.
; CHECK: call void @drop_glue.unrust(ptr %agg)
; CHECK: define internal void @drop_glue.unrust(
}
