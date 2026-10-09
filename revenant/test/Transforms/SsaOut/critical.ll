; Critical edge into a phi block is split: the copy for the cond->merge
; edge runs only on that edge (in the split block), and the region
; annotation survives on a terminator.
; RUN: %revenant-opt -passes=revenant-ssaout -S %s | %FileCheck %s

; CHECK: br i1 %c, label %left, label %entry.merge_crit_edge, !revenant.va !{{[0-9]+}}, !revenant.region !{{[0-9]+}}
; CHECK: entry.merge_crit_edge:
; CHECK: store i32 %b, ptr %v_m

define i32 @cedge(i32 %a, i32 %b) {
entry:
  %c = icmp slt i32 %a, %b, !revenant.va !0
  br i1 %c, label %left, label %merge, !revenant.region !10, !revenant.va !0

left:
  %x = add i32 %a, 1, !revenant.va !0
  br label %merge, !revenant.va !0

merge:
  %m = phi i32 [ %x, %left ], [ %b, %entry ], !revenant.va !0
  ret i32 %m, !revenant.va !0
}

!0 = !{i64 4198496}
!10 = !{!"if", i32 2}
