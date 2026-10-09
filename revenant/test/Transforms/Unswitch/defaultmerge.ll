; RUN: opt -load-pass-plugin=%shlibdir/RevenantTransformUnswitch.dll -passes=revenant-unswitch -S %s | FileCheck %s
; Default-block icmp/select merge
; (SimplifyCFG tryToSimplifyUncondBranchWithICmpSelectInIt) folded back
; into the switch: the default block's `icmp eq %c, 2; select` becomes a
; new case. Corpus direction: switch with N cases + default icmp/select.

define i32 @merged(i32 %c) {
; CHECK-LABEL: define i32 @merged(
; CHECK: switch i32 %c, label %default [
; CHECK-DAG: i32 0, label %[[E:.*]]
; CHECK-DAG: i32 1, label %case2
; CHECK-DAG: i32 2, label %switch.edge
; CHECK: ]
entry:
  switch i32 %c, label %default [
    i32 0, label %end
    i32 1, label %case2
  ]

case2:
  br label %end

default:
  %t = icmp eq i32 %c, 2, !revenant.va !{i64 4096}
  %v = select i1 %t, i32 30, i32 40, !revenant.va !{i64 4097}
  br label %end

end:
  %p = phi i32 [ 10, %entry ], [ 20, %case2 ], [ %v, %default ], !revenant.va !{i64 4098}
  ret i32 %p
}
