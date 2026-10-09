; The pass loads under the name revenant-patterns and leaves scalar code
; alone (no anchor calls, nothing to match).
; RUN: %revenant-opt -passes=revenant-patterns -S %s | %FileCheck %s

define i32 @scalar_add(i32 %a, i32 %b) {
entry:
  ; CHECK: add i32 %a, %b
  %s = add i32 %a, %b
  ret i32 %s
}
