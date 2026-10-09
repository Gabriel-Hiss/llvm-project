; Compiler IR shape: the optimizer also spells the deleted-guard bounds
; test as `icmp ule len, idx` (len <= idx = out of bounds) with the
; panic edge on the true side. That spelling negates the rustc form, so
; the pass swaps the compare to `icmp ult idx, len` AND the branch
; successors, keeping the same boolean on the same edges.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @panic_bounds_check(i64, i64)

define i64 @ule_flip(ptr %buf, i64 %len, i64 %idx) {
; CHECK-LABEL: define i64 @ule_flip(
entry:
  %c = icmp ule i64 %len, %idx, !revenant.va !{i64 4096}
  br i1 %c, label %bb_fail, label %bb_ok, !revenant.va !{i64 4097}

bb_fail:                                          ; preds = %entry
  call void @panic_bounds_check(i64 %idx, i64 %len)
  unreachable

bb_ok:                                            ; preds = %entry
  %e = getelementptr i64, ptr %buf, i64 %idx
  %v = load i64, ptr %e, align 8
  ret i64 %v
; The compare flips to the canonical form with the edges swapped: the
; ok label is now the true edge.
; CHECK: %c = icmp ult i64 %idx, %len
; CHECK: br i1 %c, label %bb_ok, label %bb_fail
}
