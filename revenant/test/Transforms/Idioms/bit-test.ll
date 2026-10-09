; RUN: %revenant-opt -passes=revenant-idioms -S %s -o - | %FileCheck %s
; The lifted TEST pattern already has the source-level bit-test form in IR.
define i1 @bit_is_set(i32 %flags) {
entry:
  %masked = and i32 %flags, 8
  %set = icmp ne i32 %masked, 0
  ret i1 %set
}
; CHECK-LABEL: define i1 @bit_is_set(
; CHECK: and i32 %flags, 8
; CHECK: icmp ne i32 %masked, 0
