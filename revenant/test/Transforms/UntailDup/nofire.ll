; RUN: %revenant-opt -passes=revenant-untaildup -S %s | %FileCheck %s
; Negative tests: volatile ops, opaque asm calls, and mismatched opcodes
; never merge.
; CHECK-LABEL: define i32 @volatile_keep(
define i32 @volatile_keep(i1 %c, ptr %p) {
entry:
  br i1 %c, label %A, label %B
A:
  %a = load volatile i32, ptr %p, align 4, !revenant.va !{i64 4096}
  ret i32 %a
B:
  %b = load volatile i32, ptr %p, align 4, !revenant.va !{i64 4112}
  ret i32 %b
; CHECK: ret i32 %a
; CHECK: ret i32 %b
}

; CHECK-LABEL: define i32 @opaque_keep(
define i32 @opaque_keep(i1 %c, i32 %x) {
entry:
  br i1 %c, label %A, label %B
A:
  %a = call i32 @revenant.asm.bsr(i32 %x), !revenant.opaque !{}, !revenant.va !{i64 4096}
  ret i32 %a
B:
  %b = call i32 @revenant.asm.bsr(i32 %x), !revenant.opaque !{}, !revenant.va !{i64 4112}
  ret i32 %b
; CHECK: call i32 @revenant.asm.bsr
; CHECK: call i32 @revenant.asm.bsr
}
declare i32 @revenant.asm.bsr(i32)

; CHECK-LABEL: define i32 @mismatch_keep(
define i32 @mismatch_keep(i1 %c, i32 %x) {
entry:
  br i1 %c, label %A, label %B
A:
  %a = add i32 %x, 1, !revenant.va !{i64 4096}
  ret i32 %a
B:
  %b = sub i32 %x, 1, !revenant.va !{i64 4112}
  ret i32 %b
; CHECK: ret i32 %a
; CHECK: ret i32 %b
}
