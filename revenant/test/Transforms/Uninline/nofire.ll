; No-fire cases: a 2-instruction getter body must not match coincidental
; code (measured 7 false sites in miniz O1 for 1 true inline), opaque
; helpers and region annotations are never disturbed, and a callee with a
; store body is ineligible.
; RUN: %revenant-opt -passes=revenant-uninline -S %s | %FileCheck %s --allow-unused-prefixes

; CHECK-LABEL: define internal i32 @getter(
define internal i32 @getter(ptr %p) {
  %g = getelementptr inbounds nuw i8, ptr %p, i64 32
  %v = load i32, ptr %g, align 8
  ret i32 %v
}

; The coincidental gep+load pair below must survive untouched.
; CHECK-LABEL: define void @coincidence(
; CHECK: getelementptr
; CHECK: load i32
; CHECK-NOT: call i32 @getter
define void @coincidence(ptr %base, ptr %dst) {
entry:
  %q = load ptr, ptr %base, align 8
  %g.i = getelementptr inbounds nuw i8, ptr %q, i64 32
  %v.i = load i32, ptr %g.i, align 8
  store i32 %v.i, ptr %dst, align 4
  ret void
}

; Opaque calls are never outlined around.
; CHECK-LABEL: define void @opaque_user(
; CHECK: call void @revenant.asm.bswap
define void @opaque_user(ptr %p) {
entry:
  call void @revenant.asm.bswap(ptr %p), !revenant.opaque !{i64 7000}
  ret void
}

declare void @revenant.asm.bswap(ptr)
