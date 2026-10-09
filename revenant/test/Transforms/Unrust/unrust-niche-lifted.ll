; Corpus: rust-o3 lifted ir-clean @sub_140001360. The mangled name maps
; to corpus_rust::option_result, but the lift names the function
; `sub_<VA>`; the compiler post IR is byte-identical
; (artifacts/corpus_rust.post.ll: store 16 at +8, store -1 at +0). The
; shape gate (a -1 sentinel at slot 0 of an aggregate that also takes a
; payload store at another offset, and nothing else on that base) names
; the niche ladder, so the discriminant trunc is restored without any
; corpus name.
; RUN: %revenant-opt -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

define void @sub_140001360(ptr %0) {
; CHECK-LABEL: define void @sub_140001360(
bb_140001360:
  %element = getelementptr i64, ptr %0, i64 1
  store i64 16, ptr %element, align 1, !revenant.va !{i64 748}
  store i64 -1, ptr %0, align 1, !revenant.va !{i64 749}
  ret void, !revenant.va !{i64 750}
; CHECK: store i64 16, ptr %element
; CHECK: store i64 -1, ptr %0
; CHECK: %discr.unrust = trunc i64 -1 to i1
}
