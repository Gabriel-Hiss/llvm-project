; Stores and loads the pass creates carry the instruction's provenance
; (`!revenant.va`) only: `!nonnull` and `!noundef` describe the loaded
; pointer and are invalid on a store (the verifier rejects them).
; RUN: %revenant-opt -passes=revenant-locals-to-slots -S %s | %FileCheck %s

; CHECK-LABEL: define ptr @walk(
; CHECK: %next = load ptr, ptr %{{.*}}, align 8, !nonnull !{{[0-9]+}}, !noundef !{{[0-9]+}}, !revenant.va ![[VA:[0-9]+]]
; CHECK-NEXT: store ptr %next, ptr %v_p, align 8, !revenant.va ![[VA]]
; CHECK-NOT: store ptr %next, ptr %v_p, {{.*}}!nonnull

define ptr @walk(ptr %head, i64 %n) {
entry:
  br label %loop

loop:
  %p = phi ptr [ %head, %entry ], [ %next, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %next = load ptr, ptr %p, align 8, !nonnull !0, !noundef !0, !revenant.va !1
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %exit

exit:
  ret ptr %next
}

!0 = !{}
!1 = !{i64 5368713216}
