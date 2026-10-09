; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Lifted select chain: a range pre-check in the head branches to a chain
; block holding the `icmp eq; select` pairs, which falls into the join.
; Corpus: c-clang-o2 @switch_sparse (sub_1400012A0) — the compiler's
; single-block select chain landed in a successor block after lifting
; the guard `icmp sgt %x, 99`.

define i32 @sparse_lifted(i32 %0) {
; CHECK-LABEL: define i32 @sparse_lifted(
; CHECK: switch i32 %0, label %[[J:.*]] [
; CHECK-DAG: i32 -5, label %[[A:.*]]
; CHECK-DAG: i32 1, label %[[B:.*]]
; CHECK: ]
  %c = icmp sgt i32 %0, 99, !revenant.va !0
  br i1 %c, label %upper, label %chain, !revenant.va !1

chain:
  %e1 = icmp eq i32 %0, 1
  %s1 = select i1 %e1, i32 10, i32 50
  %e2 = icmp eq i32 %0, -5
  %s2 = select i1 %e2, i32 40, i32 %s1
  br label %join

upper:
  %e3 = icmp eq i32 %0, 100
  %s3 = select i1 %e3, i32 20, i32 50
  %e4 = icmp eq i32 %0, 10000
  %s4 = select i1 %e4, i32 30, i32 %s3
  br label %join

join:
  %r = phi i32 [ %s2, %chain ], [ %s4, %upper ]
  ret i32 %r, !revenant.va !2
}

!0 = !{i64 5369380128}
!1 = !{i64 5369380129}
!2 = !{i64 5369380130}
