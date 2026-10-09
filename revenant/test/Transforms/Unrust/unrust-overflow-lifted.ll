; Corpus: rust-o3 lifted ir-clean @sub_140013960
; (core::str::pattern substring searcher). The optimizer folded the
; overflow flag of one `mul` into a `nuw` marker and the function keeps a
; panic edge, so the pass rebuilds the with.overflow spelling from the
; plain op: same value, the flag becomes a testable extractvalue. Lifted
; IR also keeps intact checked sites (RawVec capacity, i64 parse); those
; are left alone.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)
declare void @core__slice__index__slice_index_fail(i64, i64, i64, i64)

define i64 @sub_140013960_lifted(ptr %hay, i64 %len, i64 %x) {
; CHECK-LABEL: define i64 @sub_140013960_lifted(
entry:
  %c = icmp ugt i64 %len, %x
  br i1 %c, label %ok, label %fail

fail:                                              ; preds = %entry
  call void @core__slice__index__slice_index_fail(i64 0, i64 %x, i64 %len, i64 0)
  unreachable

ok:                                                ; preds = %entry
  %e = getelementptr i8, ptr %hay, i64 %x
  %v = load i8, ptr %e, align 1
  %z = zext i8 %v to i64
  %8 = mul nuw i64 %z, 72340172838076673
  ret i64 %8
; CHECK: call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %z, i64 72340172838076673)
; CHECK: extractvalue { i64, i1 } {{.*}}, 0
}
