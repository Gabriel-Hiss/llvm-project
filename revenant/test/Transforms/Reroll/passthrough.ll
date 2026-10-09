; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-reroll -S %s | %FileCheck %s
; Smoke test: the pass loads under the name revenant-reroll and leaves
; scalar code alone.

define i32 @scalar_add(i32 %a, i32 %b) {
entry:
  %s = add i32 %a, %b
  ret i32 %s
}

; CHECK: define i32 @scalar_add
; CHECK: %s = add i32 %a, %b
; CHECK: ret i32 %s
