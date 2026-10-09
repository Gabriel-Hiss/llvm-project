; Smoke test: the pass loads under the name revenant-devectorize and leaves
; scalar code alone.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define i32 @scalar_add(i32 %a, i32 %b) {
entry:
  ; CHECK: add i32 %a, %b
  %s = add i32 %a, %b
  ret i32 %s
}
