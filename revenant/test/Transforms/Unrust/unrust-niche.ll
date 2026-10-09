; Corpus: rust-o3 artifacts/corpus_rust.pre.ll @option_result keeps the
; niche pair `{i64, i64}` with a `trunc to i1` discriminant test; post
; folds the whole test to constant stores. The pass restores the trunc
; next to a sentinel store when no test survives.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

define void @option_result_niche(ptr %slot) {
; CHECK-LABEL: define void @option_result_niche(
entry:
  store i64 -1, ptr %slot, align 8, !revenant.va !{i64 4096}
  ret void
; CHECK: store i64 -1, ptr %slot
; CHECK: %discr.unrust = trunc i64 -1 to i1
}
