; Smoke test: the pass loads under the name revenant-unipo and leaves
; code with no IPO shape alone (opaque stubs and live signatures stay).
; RUN: %revenant-opt -passes=revenant-unipo -verify-each -S %s | %FileCheck %s

; CHECK-LABEL: define i32 @scalar_add(i32 %a, i32 %b)
define i32 @scalar_add(i32 %a, i32 %b) {
entry:
  ; CHECK: add i32 %a, %b
  %s = add i32 %a, %b
  ret i32 %s
}

; Opaque callers never rewrite.
; CHECK-LABEL: define void @opaque_user(ptr %p)
define void @opaque_user(ptr %p) {
entry:
  ; CHECK: call void @revenant.asm.bswap
  call void @revenant.asm.bswap(ptr %p), !revenant.opaque !{i64 7000}
  ret void
}

declare void @revenant.asm.bswap(ptr)
