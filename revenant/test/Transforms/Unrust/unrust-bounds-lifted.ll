; Corpus: rust-o3 lifted ir-clean @sub_1400123E0
; (core::str::pattern::StrSearcher::new). The optimizer flipped the
; bounds compare to `icmp ugt len, idx`; the lift keeps that spelling and
; the sigs recover the panic callee as
; `core__slice__index__slice_index_fail` (the slice Index panic, in the
; bounds family). The pass flips the compare back to the rustc ir-pre
; spelling `icmp ult idx, len`, true = in bounds.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @core__slice__index__slice_index_fail(i64, i64, i64, i64)

define void @sub_1400123E0_lifted(ptr %hay, i64 %len, i64 %idx) {
; CHECK-LABEL: define void @sub_1400123E0_lifted(
entry:
  %c152 = icmp ugt i64 %len, %idx, !revenant.va !{i64 5650}
  br i1 %c152, label %bb_ok, label %bb_fail, !revenant.va !{i64 5654}

bb_fail:                                          ; preds = %entry
  call void @core__slice__index__slice_index_fail(i64 7, i64 %idx, i64 %len, i64 0), !revenant.va !{i64 5814}
  unreachable

bb_ok:                                            ; preds = %entry
  %e = getelementptr i8, ptr %hay, i64 %idx
  %v = load i8, ptr %e, align 1
  ret void
; CHECK: icmp ult i64 %idx, %len
; CHECK-NOT: icmp ugt
}
