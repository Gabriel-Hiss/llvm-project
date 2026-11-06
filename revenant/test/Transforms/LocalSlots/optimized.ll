; RUN: %revenant-opt -passes='revenant-o0-module,function(revenant-locals-to-slots<unoptimized>)' -S %s | %FileCheck %s
;
; An optimized binary: one user function out of three spilled its argument,
; so the binary is not unoptimized. That function gets -O0 slots, the others
; keep the SSA values of the compiler's IR before instruction selection.

; CHECK-LABEL: define i32 @spilled(
; CHECK: alloca i32
define i32 @spilled(i32 %a) #0 {
  ret i32 %a
}

; CHECK-LABEL: define i32 @ssa(
; CHECK-NOT: alloca
; CHECK: ret i32 %a
define i32 @ssa(i32 %a) #1 {
  ret i32 %a
}

; CHECK-LABEL: define i32 @ssa2(
; CHECK-NOT: alloca
; CHECK: ret i32 %a
define i32 @ssa2(i32 %a) #1 {
  ret i32 %a
}

attributes #0 = { "revenant.o0" "revenant.user"="true" }
attributes #1 = { "revenant.user"="true" }
