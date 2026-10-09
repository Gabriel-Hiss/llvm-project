; Corpus: c-clang-o1 artifacts/main.post.ll @cmp_unsigned (ir-pre: nested
; if/return ladder in main.pre.ll). The chained selects become diamonds
; with merge phis; provenance survives on the forks.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unselect -S %s | %FileCheck %s

define i32 @cmp_unsigned(i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @cmp_unsigned(
; Forks and phis keep provenance (metadata ids are uniqued on output).
; CHECK-DAG: !revenant.va
; CHECK-DAG: !revenant.va
entry:
  %c1 = icmp ult i32 %a, %b, !revenant.va !{i64 4096}
  %c2 = icmp eq i32 %a, %b, !revenant.va !{i64 4100}
  %inner = select i1 %c2, i32 2, i32 3, !revenant.va !{i64 4104}
  %outer = select i1 %c1, i32 1, i32 %inner, !revenant.va !{i64 4108}
  ret i32 %outer
; Both levels unfold in one run: each select becomes a fork plus a merge
; phi, and the outer merge phi names the inner merge phi.
; CHECK: br i1 %c2, label %entry.t2, label %entry.f3
; CHECK: %inner.if = phi i32 [ 2, %entry.t2 ], [ 3, %entry.f3 ]
; CHECK: br i1 %c1, label %entry.t, label %entry.f
; CHECK: %outer.if = phi i32 [ 1, %entry.t ], [ %inner.if, %entry.f ]
; CHECK: ret i32 %outer.if
}
