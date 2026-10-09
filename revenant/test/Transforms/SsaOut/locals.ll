; A load feeding a return materializes into a named local; provenance
; survives on the new instructions and the opaque call keeps its marker.
; RUN: %revenant-opt -passes=revenant-ssaout -S %s | %FileCheck %s

; CHECK: alloca i32
; CHECK: call i64 @revenant.asm.cpuid({{.*}}), !dbg !{{[0-9]+}}, !revenant.opaque
; CHECK: ret i32

declare i64 @revenant.asm.cpuid(i64, i64)

define i32 @ld(ptr %p, i64 %rax, i64 %rcx) {
entry:
  %r = call i64 @revenant.asm.cpuid(i64 %rax, i64 %rcx), !revenant.opaque !0, !revenant.va !0
  %t = trunc i64 %r to i32, !revenant.va !0
  %v = load i32, ptr %p, !revenant.va !0
  %s = add i32 %v, %t, !revenant.va !0
  ret i32 %s, !revenant.va !0
}

!0 = !{i64 4198496}
