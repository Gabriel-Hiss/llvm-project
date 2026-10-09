; Corpus: rust-o3 artifacts/corpus_rust.pre.ll @vec_index (ir-pre keeps
; `icmp ult %i, %_11` plus a branch to panic_bounds_check; the optimizer
; flips the surviving compare to `ugt`). The pass flips ugt back to ult.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @panic_bounds_check(i64, i64, ptr)

define i64 @bounds(i64 %i, i64 %len) {
; CHECK-LABEL: define i64 @bounds(
entry:
  %c = icmp ugt i64 %len, %i, !revenant.va !{i64 4096}
  br i1 %c, label %ok, label %panic
ok:
  ret i64 %i
panic:
  call void @panic_bounds_check(i64 %i, i64 %len, ptr null)
  unreachable
; The flipped predicate returns to the rustc ir-pre spelling.
; CHECK: %c = icmp ult i64 %i, %len
; CHECK: br i1 %c, label %ok, label %panic
}
