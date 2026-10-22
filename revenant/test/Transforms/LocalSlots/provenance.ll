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

; An entry alloca feeding a phi is stored after the slot allocas, not
; right after itself (spdlog-gcc-o0 sub_140012480: the store came before
; the slot it writes).
; CHECK-LABEL: define ptr @scan(
; CHECK: %buf = alloca [16 x i8]
; CHECK-NEXT: %retval = alloca ptr
; CHECK-NEXT: store ptr %buf, ptr %retval

define ptr @scan(i64 %n) {
entry:
  %buf = alloca [16 x i8], align 8
  br label %loop

loop:
  %cur = phi ptr [ %buf, %entry ], [ %step, %loop ]
  %step = getelementptr i8, ptr %cur, i64 1
  %c = icmp eq ptr %step, null
  br i1 %c, label %exit, label %loop

exit:
  ret ptr %cur
}

!0 = !{}
!1 = !{i64 5368713216}
