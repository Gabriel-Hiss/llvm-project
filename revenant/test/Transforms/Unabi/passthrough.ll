; Smoke test: the pass loads under the name revenant-unabi and leaves
; unannotated code alone.
; RUN: %revenant-opt -passes=revenant-unabi -S %s | %FileCheck %s

define i32 @scalar_add(i32 %a, i32 %b) {
entry:
  ; CHECK: add i32 %a, %b
  %s = add i32 %a, %b
  ret i32 %s
}
