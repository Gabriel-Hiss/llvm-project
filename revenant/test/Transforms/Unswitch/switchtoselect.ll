; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s
; Switch-to-select (SimplifyCFG trySwitchToSelect/foldSwitchToSelect) back
; to a switch: chained `icmp eq; select` feeding a return.
; Corpus shape: rust-o3 option handling / lua need_value select folds.

define i32 @selchain(i32 %c) {
; CHECK-LABEL: define i32 @selchain(
; CHECK: switch i32 %c, label %{{.*}} [
; CHECK-DAG: i32 10, label %{{.*}}
; CHECK-DAG: i32 20, label %{{.*}}
; CHECK: ]
entry:
  %e1 = icmp eq i32 %c, 10, !revenant.va !{i64 4096}
  %s1 = select i1 %e1, i32 42, i32 4
  %e2 = icmp eq i32 %c, 20, !revenant.va !{i64 4097}
  %s2 = select i1 %e2, i32 2, i32 %s1, !revenant.va !{i64 4098}
  ret i32 %s2
}
