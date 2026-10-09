; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unhoist -S %s | %FileCheck %s
;
; Provenance: every cloned instruction inherits the source's
; !revenant.va, and !revenant.opaque calls are never touched.
;
; CHECK-LABEL: define i32 @va(
define i32 @va(ptr %a, i32 %n) {
entry:
  %n.addr = alloca i32, align 4
  store i32 %n, ptr %n.addr, align 4
  %bound = load i32, ptr %n.addr, align 4, !revenant.va !{i64 4096}
  br label %loop

loop:                                             ; preds = %latch, %entry
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
; CHECK: loop:
; CHECK: [[SUNK:%.*]] = load i32, ptr %n.addr, align 4, !revenant.va !{{.*}}
  %c = icmp slt i32 %i, %bound
  br i1 %c, label %body, label %exit

body:                                             ; preds = %loop
  br label %latch

latch:                                            ; preds = %body
  %i.next = add i32 %i, 1
  br label %loop

exit:                                             ; preds = %loop
  ret i32 %i
}

; CHECK-LABEL: define i32 @opaque(
define i32 @opaque(i32 %x) {
entry:
; An opaque call in the preheader position is never sunk, even when a
; loop body uses it.
; CHECK: entry:
; CHECK: %v = call i32 @revenant.asm.bsr(i32 %x), !revenant.opaque !{{.*}}
  %v = call i32 @revenant.asm.bsr(i32 %x), !revenant.opaque !{}
  br label %loop

loop:                                             ; preds = %latch, %entry
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %s, %latch ]
  %c = icmp slt i32 %i, 10
  br i1 %c, label %body, label %exit

body:                                             ; preds = %loop
; CHECK: body:
; CHECK: %s = add i32 %v, %i
  %s = add i32 %v, %i
  br label %latch

latch:                                            ; preds = %body
  %i.next = add i32 %i, 1
  br label %loop

exit:                                             ; preds = %loop
  ret i32 %acc
}

declare i32 @revenant.asm.bsr(i32)

!0 = !{i64 4096}
