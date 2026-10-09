; Gating: without `revenant.lang == "rust"` the pass is a no-op, so C/C++
; IR never changes shape. Same flipped predicate as unrust-bounds.ll but
; with the C marker: the ugt stays put.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}

declare void @panic_bounds_check(i64, i64, ptr)

define i64 @bounds_c(i64 %i, i64 %len) {
; CHECK-LABEL: define i64 @bounds_c(
entry:
  %c = icmp ugt i64 %len, %i, !revenant.va !{i64 4096}
  br i1 %c, label %ok, label %panic
ok:
  ret i64 %i
panic:
  call void @panic_bounds_check(i64 %i, i64 %len, ptr null)
  unreachable
; CHECK: %c = icmp ugt i64 %len, %i
}
